"""Analysis interactions against synthetic fixtures in a temporary DB only."""
import base64
import csv
import io
import math
from pathlib import Path
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(Path.home()/'telemetry-tools/browser-tests'))
sys.path.insert(0,str(ROOT/'gateway'))
from playwright.sync_api import sync_playwright
from ev_gateway import EVGateway,make_handler

PNG=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aN1sAAAAASUVORK5CYII=')
with tempfile.TemporaryDirectory() as directory:
    gateway=EVGateway('127.0.0.1',0,'127.0.0.1',19004,database_path=Path(directory)/'test.db')
    server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(gateway))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    base=f'http://127.0.0.1:{server.server_port}'
    try:
        with sync_playwright() as pw:
            browser=pw.chromium.launch(channel='msedge',headless=True)
            context=browser.new_context(viewport={'width':1550,'height':1080},extra_http_headers={'X-Forwarded-For':'203.0.113.9'})
            context.route('https://tile.openstreetmap.org/**',lambda route:route.fulfill(body=PNG,content_type='image/png'))
            page=context.new_page();errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
            page.goto(base+'/records');page.wait_for_function("!document.querySelector('#record').disabled")
            page.locator('#label').fill('ANALYSIS UI TEST ONLY');page.locator('#record').click()
            page.wait_for_function('state.active && state.sid===1 && !state.busy')
            assert gateway.database.status()['recording_sample_count']==0
            page.wait_for_function("document.querySelector('#recordClock').textContent==='00:00:01'",timeout=5000)
            start=time.time()-12
            gateway.database._started_epoch=start;gateway.database._elapsed_anchor=12
            with gateway.database._lock:
                gateway.database._connection.execute('UPDATE measurement_sessions SET started_at_utc=? WHERE id=1',(gateway.database._utc_text(start),))
                gateway.database._connection.commit()
            for seq in range(1007):
                phase=seq*.00628
                packet={'seq':seq,'tqv_internal_online':True,'imu_online':True,'imu_ok':True,
                        'desired_yaw_rad_s':math.sin(phase)*.4,'yaw_rate_rad_s':math.sin(phase-.15)*.36,
                        'power_left_kw':3+math.sin(phase),'power_right_kw':4-math.sin(phase),
                        'bms_online':seq!=500,'battery_pack_voltage_v':72,
                        'battery_current_a':-200 if seq==1003 else 0 if seq==600 else 90+20*math.sin(phase),
                        'bms_state':'DISCHARGING' if seq==1003 else 'CHARGING',
                        'gnss_online':seq!=500,'gnss_latitude_deg':35.14358+.0004*math.sin(phase),
                        'gnss_longitude_deg':126.93324+.0006*math.cos(phase),'gnss_accuracy_m':2,
                        'gnss_age_ms':100,'gnss_source_timestamp_s':start+seq*.01,
                        'bms_cell_voltages_v':[3.3+i*.001 for i in range(24)],
                        'sample_time_ms':round((start+min(seq,1000)*.01)*1000)}
                gateway.database.record(packet)
            page.locator('#latest').click();page.wait_for_function('state.rows.length===1007 && !state.busy && state.visible.length===1007')
            assert len(page.evaluate('state.channels.filter(c=>c.category===\'battery\')'))>5
            assert page.locator('#rows tr').count()==200
            page.locator('#peak').click();page.wait_for_function('state.rows[state.index].id===1004')
            page.wait_for_function("document.querySelector('#pointW').textContent==='-14.40 kW'")
            assert page.locator('#powerUnit').input_value()=='kW'
            assert page.locator('#pointCommand').inner_text()=='7.00 kW'
            assert '-14.40 kW' in page.locator('#mapInfo').inner_text()
            assert page.locator('#mapMax').inner_text()=='|kW| 14.40'
            assert 'kW' in page.locator('#powerColumn').inner_text()
            assert 'kW' in page.locator('[data-plot="battery:W"] h3').inner_text()
            assert '-14.40 kW' in page.locator('[data-plot="battery:W"] .plot-cursor').inner_text()
            assert '-14.40 kW' in page.locator('#values').inner_text()
            assert page.evaluate("state.plots.find(p=>p.id==='battery:W').geometry.low>-16")
            assert page.evaluate("state.rows[state.index].values.battery_power_w===-14400")
            assert page.evaluate("formatValue(9004.21,'W')")=='9.00421'
            assert float(page.locator('#rows tr').first.locator('td').nth(1).inner_text())==6.48
            page.locator('#powerUnit').select_option('W')
            page.wait_for_function("document.querySelector('#pointW').textContent==='-14,400.00 W'")
            assert page.locator('#pointW').inner_text()=='-14,400.00 W'
            assert '방전' in page.locator('#powerDirection').inner_text()
            assert page.evaluate('marker!==null')
            assert page.locator('#pointCommand').inner_text()=='7,000.00 W'
            assert page.evaluate("state.plots.find(p=>p.id==='battery:W').geometry.low<=-14400")
            page.locator('#zoomIn').click();page.wait_for_function('state.range[1]-state.range[0]<6')
            page.locator('#xMin').fill('2');page.locator('#xMax').fill('4');page.locator('#applyX').click()
            page.wait_for_function('state.range[0]===2 && state.range[1]===4 && state.visible.length>0 && state.visible.every(r=>x(r)>=2&&x(r)<=4)')
            assert page.evaluate('state.visible.every(r=>x(r)>=2&&x(r)<=4)')
            power=page.locator('[data-plot="battery:W"]')
            power.locator('input[placeholder="Y 최소"]').fill('0');power.locator('input[placeholder="Y 최대"]').fill('15000');power.get_by_text('Y 적용',exact=True).click()
            page.wait_for_function("state.plots.find(p=>p.id==='battery:W').geometry.high===15000")
            page.locator('#powerUnit').select_option('kW')
            page.wait_for_function("state.plots.find(p=>p.id==='battery:W').geometry.high===15")
            assert power.locator('input[placeholder="Y 최대"]').input_value()=='15'
            assert page.evaluate('state.range')==[2,4]
            page.locator('#powerUnit').select_option('W')
            page.wait_for_function("state.plots.find(p=>p.id==='battery:W').geometry.high===15000")
            power.get_by_text('Y 자동',exact=True).click()
            page.locator('#resetZoom').click();page.wait_for_function('state.visible.length===1007')
            power.locator('canvas.plot-overlay').scroll_into_view_if_needed();box=power.locator('canvas.plot-overlay').bounding_box()
            page.mouse.move(box['x']+110,box['y']+100);page.mouse.down();page.mouse.move(box['x']+260,box['y']+100,steps=6);page.mouse.up()
            page.wait_for_function('state.range[1]-state.range[0]<9')
            page.locator('#resetZoom').click();page.wait_for_function('state.visible.length===1007')
            page.evaluate('choose(state.rows.findIndex(r=>r.id===501),true)');page.wait_for_function("document.querySelector('#pointW').textContent==='— W'")
            assert page.evaluate('marker===null')
            page.evaluate('choose(state.rows.findIndex(r=>r.id===601),true)');page.wait_for_function("document.querySelector('#pointW').textContent==='0.00 W'")
            before=page.evaluate('state.rows.length')
            page.locator('#categories input[data-key="battery_power_w"]').uncheck()
            assert page.locator('[data-plot="battery:W"]').count()==0
            assert page.evaluate('state.rows.length')==before
            page.locator('#hideAll').click();assert page.locator('.plot-panel').count()==0
            page.locator('#tvPreset').click();assert page.locator('[data-plot="tv:rad/s"]').count()==1
            page.locator('#batteryPreset').click()
            page.locator('#next').click();page.wait_for_function('state.page===1')
            page.locator('#rows tr').first.click();page.wait_for_function('state.rows[state.index].id===201')
            page.locator('#timeAxis').select_option('receive');page.wait_for_function("state.axis==='receive'")
            assert page.evaluate('state.visible.length')==1007
            page.locator('#timeAxis').select_option('sample');page.wait_for_function("state.axis==='sample'")
            page.locator('#record').click();page.wait_for_function('!state.active && !state.busy && state.rows.length===1007')
            assert gateway.database.status()['recording_sample_count']==1007
            with page.expect_download() as download:page.locator('#download').click()
            records=list(csv.DictReader(io.StringIO(Path(download.value.path()).read_text(encoding='utf-8-sig'))))
            assert len(records)==1007 and float(records[1003]['battery_power_w'])==-14400
            assert records[500]['battery_power_w']==''
            assert float(records[600]['battery_power_w'])==0
            assert float(records[0]['power_command_total_w'])==7000
            assert 'sample_elapsed_s' in records[0] and 'cell_24_v' in records[0]
            page.locator('#powerUnit').select_option('kW')
            page.evaluate('choose(state.rows.findIndex(r=>r.id===501),true)')
            page.wait_for_function("document.querySelector('#pointW').textContent==='— kW'")
            page.evaluate('choose(state.rows.findIndex(r=>r.id===601),true)')
            page.wait_for_function("document.querySelector('#pointW').textContent==='0.00 kW'")
            page.reload();page.wait_for_function('state.rows.length===1007 && !state.busy')
            assert page.locator('#powerUnit').input_value()=='kW'
            assert page.evaluate("localStorage.getItem('djy-analysis-power-unit')")=='kW'
            page.locator('#peak').click();page.wait_for_function('state.rows[state.index].id===1004')
            page.wait_for_function("document.querySelector('#pointW').textContent==='-14.40 kW'")
            out=Path.home()/'telemetry-audit-20260920'
            page.evaluate('window.scrollTo(0,0)');page.screenshot(path=str(out/'analysis-desktop.png'),full_page=True)
            page.set_viewport_size({'width':390,'height':844});page.wait_for_timeout(200)
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
            page.screenshot(path=str(out/'analysis-mobile.png'),full_page=True)
            assert not errors,errors
            browser.close()
        print('ANALYSIS PASS: full session, duplicate timestamps, W peak past page 1, exact readouts, GPS association, X/Y ranges, drag zoom, checkboxes, presets, null/zero, CSV, mobile, recording')
    finally:
        server.shutdown();server.server_close();gateway.database.close();gateway.forward_socket.close()
