"""Isolated UI test: temporary database/HTTP port; never sends vehicle commands."""
import json
import math
import sys
import tempfile
import threading
from pathlib import Path
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.local/browser-tools'))
if (Path.home()/'telemetry-tools/browser-tests').is_dir():
    sys.path.insert(0, str(Path.home()/'telemetry-tools/browser-tests'))
sys.path.insert(0, str(ROOT / 'gateway'))
from playwright.sync_api import sync_playwright
from ev_gateway import EVGateway, make_handler
from tuning import CHANNELS
from stm_serial import RearStatusParser

with tempfile.TemporaryDirectory() as directory:
    gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=Path(directory)/'test.db')
    gateway.set_recording(True, 'UI TEST ONLY')
    for seq in range(1010):
        gateway.accept({'seq':seq, 'timestamp_ms':seq*100, 'desired_yaw_rad_s':math.sin(seq/30)*.5,
                        'yaw_rate_rad_s':math.sin((seq-5)/30)*.4, 'imu_ok':True,
                        'tqv_internal_online':True, 'rpm_left':0, 'rpm_right':0,
                        'rpm_left_reported':0, 'rpm_right_reported':7980,
                        'rpm_left_quality':'OK', 'rpm_right_quality':'NOISY',
                        'rpm_right_rejected_pct':99.3, 'motor_right_ok':False,
                        'rpm_left_target':1200, 'rpm_right_target':1300,
                        'power_left_kw':2, 'power_right_kw':2.5, 'motor_command_mode':'RPM',
                        'vehicle_speed_m_s':12, 'speed_kmh':43.2, 'speed_online':True,
                        'yaw_error_rad_s':-.125, 'delta_power_kw':.5, 'traction_scale':.75,
                        'tv_active':True, 'ed_active':False,
                        'pid_kp':20, 'pid_ki':1, 'pid_kd':0, 'pid_online':True,
                        'tps_raw':0, 'tps_pct':0, 'tps_ok':True, 'sas_deg':seq%20-10, 'sas_ok':True})
    gateway.set_recording(False)
    # Give the test samples a realistic, ordered clock without waiting 100s.
    with gateway.database._lock:
        for i, (rowid,) in enumerate(gateway.database._connection.execute('SELECT id FROM telemetry_samples').fetchall()):
            gateway.database._connection.execute('UPDATE telemetry_samples SET captured_at_utc=? WHERE id=?',
                (gateway.database._utc_text(1700000000+i*.1),rowid))
        gateway.database._connection.commit()
    server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(gateway))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        with sync_playwright() as pw:
            browser=pw.chromium.launch(channel='msedge',headless=True)
            page=browser.new_page(viewport={'width':1440,'height':1050})
            errors=[]
            page.on('pageerror',lambda error:errors.append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/pit')
            page.wait_for_function(f"document.querySelectorAll('#channels input').length === {len(CHANNELS)}")
            page.locator('#session').select_option('1')
            page.wait_for_function("document.querySelector('#mode').textContent.includes('1페이지')")
            assert page.locator('#actual').inner_text() != '미수신'
            assert page.locator('#value-motor_left_voltage_v').inner_text().startswith('미수신')
            assert page.locator('#plots canvas').count() == 9
            assert page.evaluate('getComputedStyle(document.body).backgroundColor') == 'rgb(0, 0, 0)'
            assert page.locator('#pid').inner_text() == '20.000 / 1.000 / 0.000'
            assert page.locator('#speed').inner_text() == '43.2'
            assert page.locator('#tv-yaw_error_rad_s').inner_text() == '-0.125'
            assert page.locator('#tv-ed_active').inner_text() == 'false · 비개입'
            assert page.locator('#reported-right').inner_text() == '7980 rpm'
            assert '노이즈' in page.locator('#rpm-right').inner_text()
            assert '99.3%' in page.locator('#quality-right').inner_text()
            assert page.locator('#target-left').inner_text() == '1200 rpm'
            assert '선택 시점' in page.locator('#rpmContext').inner_text()
            def stroke_colors():
                return page.evaluate("""() => {
                    const ctx=document.querySelector('#canvas-steering').getContext('2d');
                    const original=ctx.stroke, colors=[];
                    ctx.stroke=function(){colors.push(this.strokeStyle); original.call(this);};
                    try { draw(); } finally { ctx.stroke=original; }
                    return colors;
                }""")
            assert '#00ffff' in stroke_colors()
            steering = page.locator('.channel').filter(has=page.locator('#value-sas_deg')).locator('input')
            steering.uncheck()
            assert '#00ffff' not in stroke_colors()
            assert page.locator('#canvas-steering').is_visible()
            steering.check()
            assert '#00ffff' in stroke_colors()
            for checkbox in page.locator('#channels input').all():
                checkbox.uncheck()
            assert page.locator('#plots canvas').count() == 9
            assert set(stroke_colors()) <= {'#555555', '#ffffff'}
            for checkbox in page.locator('#channels input').all():
                checkbox.check()
            page.locator('#next').click()
            page.wait_for_function("document.querySelector('#mode').textContent.includes('2페이지')")
            assert page.locator('#next').is_disabled()
            page.locator('#prev').click()
            page.wait_for_function("document.querySelector('#mode').textContent.includes('1페이지')")
            page.locator('#window').select_option('all')
            page.locator('#canvas-yaw').hover()
            assert '목표 yaw rate' in page.locator('#cursor').inner_text()
            with page.expect_download() as download:
                page.locator('#export').click()
            assert download.value.suggested_filename.endswith('.csv')
            csv = Path(download.value.path()).read_text(encoding='utf-8-sig')
            assert 'rpm_right_reported' in csv and 'rpm_right_quality' in csv
            assert 'NOISY' in csv and '7980' in csv
            assert 'pid_kp' in csv and 'yaw_error_rad_s' in csv and 'tv_active' in csv
            page.mouse.move(0,0)
            page.evaluate('window.scrollTo(0,0)')
            page.screenshot(path=str(ROOT/'.local/tuning-desktop.png'),full_page=True)
            page.locator('#live').click()
            gateway.accept(RearStatusParser().parse(
                b'L=0 R=7980 cap=0/1000 glt=0/990 tps=868 pct=0 idle=868 '
                b'vs=0 dy=0 ye=0 dp=0 pl=0 pr=0 tva=0 eda=0 tr=1000 fault=3'))
            page.wait_for_function("document.querySelector('#rpmNote').textContent.includes('전력 명령(kW) 제어')")
            page.wait_for_function("document.querySelector('#target-left').textContent==='미제공'")
            assert page.locator('#value-rpm_left_target').inner_text() == '목표값 미제공'
            page.wait_for_function("document.querySelector('#pidNote').textContent.includes('PID 미수신')")
            page.wait_for_function("document.querySelector('#tv-vehicle_speed_m_s').textContent==='미수신'")
            assert page.locator('#speed').inner_text() == '미수신'
            page.locator('#record').click()
            page.wait_for_function("document.querySelector('#record').textContent==='측정 종료'")
            page.locator('#pause').click()
            assert gateway.database.status()['recording_active']
            before = page.evaluate('state.live.filter(r=>r.id).length')
            frozen = page.evaluate('state.pausedRows.map(r=>r.id)')
            clock = 1800000000000
            for i in range(4):
                gateway.accept({'seq':2000+i, 'timestamp_ms':200*i,
                    'sample_time_ms':clock+200*i, 'yaw_rate_rad_s':i/10,
                    'imu_ok':True, 'desired_yaw_rad_s':.5, 'tqv_internal_online':True})
            page.wait_for_function(f'state.live.filter(r=>r.id).length >= {before+4}')
            assert page.evaluate('state.pausedRows.map(r=>r.id)') == frozen
            times = page.evaluate('state.live.filter(r=>r.id).slice(-4).map(r=>r.time_ms)')
            assert times == [clock+200*i for i in range(4)], times
            page.locator('#pause').click()
            assert page.evaluate('state.pausedRows === null && !state.paused')
            ids = page.evaluate('state.live.filter(r=>r.id).map(r=>r.id)')
            assert len(ids) == len(set(ids))
            page.locator('#record').click()
            page.wait_for_function("document.querySelector('#record').textContent==='측정 시작'")
            page.set_viewport_size({'width':390,'height':844})
            page.screenshot(path=str(ROOT/'.local/tuning-mobile.png'),full_page=True)
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
            assert not errors, errors
            browser.close()
        print(json.dumps({'browser':'Edge','errors':errors,'checks':['9 purpose panels','black background',f'{len(CHANNELS)} channels','PID and TV values','stale/missing PID and speed','reported RPM vs quality','explicit/missing target RPM','history RPM status','per-line visibility','hide all','history pages','missing voltage','cursor','CSV values and quality','record start/stop','pause keeps recording','mobile layout']}))
    finally:
        server.shutdown()
        server.server_close()
        gateway.database.close()
        gateway.forward_socket.close()
