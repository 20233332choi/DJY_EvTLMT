"""Isolated map/phone checks; never requests real OSM tiles or sends live fixes."""
import base64
import csv
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(Path.home()/'telemetry-tools/browser-tests'))
sys.path.insert(0,str(ROOT/'gateway'))
sys.path.insert(0,str(ROOT/'tests'))
from playwright.sync_api import sync_playwright
from ev_gateway import EVGateway, make_handler
from test_gnss import setup, feed, LOOP, GNSSManager, ORIGIN

# Neutral test tiles: automated UI tests do not fetch the community tile service.
PNG=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a9iUAAAAASUVORK5CYII=')
with tempfile.TemporaryDirectory() as folder:
    g=EVGateway('127.0.0.1',0,'127.0.0.1',19004,database_path=Path(folder)/'test.db')
    setup(g.gnss)
    for i,(x,y) in enumerate(LOOP):feed(g.gnss,x,y,1001+i)
    g.gnss.command({'action':'stop'})
    server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(g))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    base=f'http://127.0.0.1:{server.server_port}'
    try:
        with sync_playwright() as pw:
            browser=pw.chromium.launch(channel='msedge',headless=True)
            context=browser.new_context(viewport={'width':1440,'height':1080},accept_downloads=True)
            context.route('https://tile.openstreetmap.org/**',lambda route:route.fulfill(body=PNG,content_type='image/png'))
            page=context.new_page();errors=[]
            page.on('pageerror',lambda e:errors.append(str(e)))
            page.goto(base+'/map')
            page.wait_for_function("liveState!==null && document.querySelectorAll('#history option').length===2")
            assert page.locator('.leaflet-control-attribution').inner_text().find('OpenStreetMap')>=0
            page.locator('#history').select_option('1')
            page.wait_for_function("mode==='history'&&rows.length===13")
            assert page.locator('#lastLap').inner_text()=='0:11.00'
            assert page.locator('#laps tr').count()==1
            assert page.evaluate('trail.getLatLngs().length')>0
            assert page.locator('#next').is_disabled()
            with page.expect_download() as info:page.locator('#csv').click()
            exported=Path(info.value.path()).read_text(encoding='utf-8-sig')
            records=list(csv.DictReader(io.StringIO(exported)))
            assert len(records)==13 and 'raw_lat' in records[0] and 'ts' in records[0]
            with page.expect_download() as info:page.locator('#lapCsv').click()
            assert len(list(csv.DictReader(io.StringIO(Path(info.value.path()).read_text(encoding='utf-8-sig')))))==1
            page.locator('#history').select_option('')
            page.locator('#pickGate').click()
            page.evaluate("map.fire('click',{latlng:L.latLng(35.1435,126.9331)});map.fire('click',{latlng:L.latLng(35.1435,126.9333)});")
            page.locator('#saveGate').click()
            page.wait_for_function("draft===null&&gate.a[0]===35.1435")
            assert g.gnss.gate['a'][0]==35.1435
            page.locator('#reverse').click();page.locator('#saveGate').click()
            page.wait_for_function("draft===null&&gate.a[1]===126.9333")
            page.locator('#collect').click()
            page.wait_for_function('liveState.calibration_report.collecting')
            assert page.locator('#calibrate').is_disabled()
            out=Path.home()/'telemetry-audit-20260920';out.mkdir(exist_ok=True)
            page.screenshot(path=str(out/'gnss-ui-desktop.png'),full_page=True)
            page.set_viewport_size({'width':390,'height':844})
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
            page.screenshot(path=str(out/'gnss-ui-mobile.png'),full_page=True)
            # Public clients may edit lap gates; calibration remains local.
            public=browser.new_context(extra_http_headers={'X-Forwarded-For':'203.0.113.9'})
            public.route('https://tile.openstreetmap.org/**',lambda route:route.fulfill(body=PNG,content_type='image/png'))
            remote=public.new_page();remote.goto(base+'/map')
            remote.wait_for_function("liveState!==null")
            assert remote.locator('#run').is_enabled()
            assert remote.locator('#collect').is_disabled()
            assert remote.locator('#currentGate').is_disabled()
            assert remote.locator('#gateAngleSlider').is_enabled()
            remote.locator('#pickGate').click()
            remote.evaluate("map.fire('click',{latlng:L.latLng(35.1435,126.9331)});map.fire('click',{latlng:L.latLng(35.1435,126.9333)});")
            remote.locator('#saveGate').click()
            remote.wait_for_function('draft===null && !busy')
            assert g.gnss.gate['a'][1]==126.9331
            assert remote.locator('#minLap').count()==0
            assert remote.locator('#minDistance').count()==0
            public.close()
            # Phone callback batching must retain several closely spaced fixes.
            phone=context.new_page();phone.on('pageerror',lambda e:errors.append(str(e)))
            phone.add_init_script("Object.defineProperty(navigator,'geolocation',{value:{watchPosition(ok){window.gpsCallback=ok;return 1;},clearWatch(){}}});")
            phone.goto(base+'/phone');phone.locator('#toggle').click()
            phone.evaluate("""()=>{for(let i=0;i<3;i++)gpsCallback({timestamp:Date.now()-200+i*100,coords:{latitude:35.14358,longitude:126.93324,accuracy:2,speed:0,heading:null,altitude:null}});} """)
            phone.wait_for_function('accepted===3')
            assert len([p for p in g.gnss.points if p['ts']>time.time()-10])==3
            phone.locator('#toggle').click()
            phone.close()
            # Native uploads do not depend on an open geolocation web page.
            native = context.request.post(base+'/api/gnss/native', form={
                'id':'iphone-browser-test', 'lat':'35.14358', 'lon':'126.93324',
                'accuracy':'2', 'timestamp':str(time.time()), 'speed':'0'})
            assert native.json()['accepted']
            phone=context.new_page();phone.on('pageerror',lambda e:errors.append(str(e)))
            phone.goto(base+'/phone')
            phone.wait_for_function("document.querySelector('#nativeStatus').textContent.includes('채택 1')")
            assert phone.locator('#nativeUrl').input_value()==base+'/api/gnss/native'
            phone.set_viewport_size({'width':390,'height':844})
            assert phone.evaluate('document.documentElement.scrollWidth <= innerWidth+1')
            phone.screenshot(path=str(Path.home()/'telemetry-audit-20260920/phone-background-mobile.png'),full_page=True)
            # A stale location must not become a new current-position gate.
            page.wait_for_function('!liveState.live',timeout=8000)
            assert page.locator('#currentGate').is_disabled()
            stop_fixes=threading.Event()
            def fresh_fixes():
                while not stop_fixes.is_set():
                    feed(g.gnss,0,0,time.time(),speed_kmh=0)
                    stop_fixes.wait(.3)
            worker=threading.Thread(target=fresh_fixes,daemon=True);worker.start()
            try:
                page.wait_for_function("!document.querySelector('#currentGate').disabled")
                saved=dict(g.gnss.gate)
                page.locator('#currentGate').click()
                assert page.evaluate('gateGeometry(draft).center')==ORIGIN
                assert g.gnss.gate==saved  # Preview must never write settings.
                assert page.locator('#run').is_disabled()
                page.locator('#gateAngle').fill('0');page.locator('#gateAngle').dispatch_event('change')
                page.locator('#gateWidth').fill('30');page.locator('#gateWidth').dispatch_event('change')
                assert abs(page.evaluate('gateGeometry(draft).width')-30)<.01
                assert page.evaluate('gateGeometry(draft).angle')==0
                # Rotation must match backend crossing direction: angle 0 means westward crossing.
                rotated=page.evaluate('draft')
                m=GNSSManager();m.command(dict(action='gate',**rotated,min_lap_s=5,min_distance_m=20))
                feed(m,20,0,1000);m.command({'action':'start'},now=1000.1)
                feed(m,10,0,1001);feed(m,-10,0,1002)
                assert m.anchor is not None
                # Center drag retains angle/width; save and cancel are explicit.
                page.evaluate("gateHandle.fire('dragstart');gateHandle.setLatLng([35.1436,126.9333]);gateHandle.fire('dragend')")
                assert page.evaluate('gateGeometry(draft).center')==[35.1436,126.9333]
                page.locator('#saveGate').click();page.wait_for_function('draft===null && !busy')
                page.wait_for_function('gate.a[0]>35.1434')
                assert abs((g.gnss.gate['a'][0]+g.gnss.gate['b'][0])/2-35.1436)<1e-8
                # Boundary widths must also pass server validation at several angles.
                for width in [5,200]:
                    for angle in [0,45,90,180,270]:
                        endpoints=page.evaluate('v=>makeGate([35.14358,126.93324],v[0],v[1])',[angle,width])
                        GNSSManager().command(dict(action='gate',**endpoints))
                page.locator('#reverse').click()
                assert abs(page.evaluate('gateGeometry(draft).angle')-180)<.01
                page.locator('#cancelGate').click();assert page.evaluate('draft') is None
                page.screenshot(path=str(out/'gnss-current-gate-mobile.png'),full_page=True)
                assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
            finally:
                stop_fixes.set();worker.join(timeout=2)
            assert not errors,errors
            browser.close()
        print('GPS UI PASS: map layers, gate edit, read-only sharing, laps, history, CSV, mobile, phone batches')
    finally:
        server.shutdown();server.server_close();g.database.close();g.forward_socket.close()
