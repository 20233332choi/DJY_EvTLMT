"""Local browser regression with synthetic data; no vehicle connection.

Usage: NODE_PATH=... python3 scripts/check_high_rate_web.py NODE CHROME
Requires Playwright for Node; the gateway uses only a temporary SQLite DB.
"""
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'gateway'))
from ev_gateway import EVGateway, make_handler

script = r'''
const {chromium}=require('playwright');
const fs=require('fs');
const assert=require('assert');
(async()=>{
 const browser=await chromium.launch({headless:true,executablePath:process.argv[2]});
 try {
  const page=await browser.newPage({viewport:{width:1440,height:1000}});
  const errors=[]; page.on('pageerror',e=>errors.push(String(e)));
  const base=process.argv[1];
  await page.goto(base+'/pit');
  await page.waitForFunction(()=>document.querySelectorAll('#plots canvas').length===13);
  await page.locator('#session').selectOption('1');
  await page.waitForFunction(()=>document.querySelector('#mode').textContent.includes('1페이지'));
  assert((await page.locator('#value-battery_power_kw').innerText()).includes('-9.100'));
  await page.locator('#next').click();
  await page.waitForFunction(()=>document.querySelector('#mode').textContent.includes('2페이지'));
  assert(await page.locator('#next').isDisabled());
  let ready=page.waitForEvent('download'); await page.locator('#export').click();
  let file=await ready; let data=fs.readFileSync(await file.path(),'utf8');
  fs.writeFileSync('/tmp/ev-highrate-export.csv',data);
  assert(data.trim().split(/\r?\n/).length===1011, 'CSV rows='+data.trim().split(/\r?\n/).length+' filename='+file.suggestedFilename()); // all pages, not just page 2
  assert(data.includes('battery_power_kw') && data.includes('rpm_left_quality'));
  await page.locator('#prev').click();
  await page.waitForFunction(()=>document.querySelector('#mode').textContent.includes('1페이지'));
  await page.locator('#plot-battery_power').screenshot({path:'/tmp/ev-high-rate-battery.png'});
  await page.locator('#live').click();
  const canvas=page.locator('#canvas-yaw'); await canvas.scrollIntoViewIfNeeded();
  const bounds=await canvas.boundingBox();
  await page.mouse.click(bounds.x+bounds.width*.65,bounds.y+100);
  assert(await page.evaluate(()=>!state.paused && state.pausedRows===null && state.lockedSample!==null));
  const locked=await page.locator('#cursorTime').innerText();
  const selected=await page.locator('#plot-value-yaw_rate_rad_s').innerText();
  const interval=await page.evaluate(()=>[viewport().start,viewport().end]);
  assert(locked.startsWith('값 고정 '));
  assert((await page.locator('#cursor').innerText()).length<110);
  assert((await page.locator('.plot-time').allTextContents()).every(t=>t===locked));
  await page.mouse.move(0,0);
  const before=await page.evaluate(()=>state.live.filter(r=>r.id).length);
  const rows=Array.from({length:40},(_,i)=>[i+1,100+i*5,i/100,true,true]);
  const response=await page.request.post(base+'/api/vehicle/exchange',{
    headers:{Authorization:'Bearer local-test-only-123456'},
    data:{stream_id:'0123456789abcdef',sent_ms:300,
      columns:['seq','timestamp_ms','yaw_rate_rad_s','imu_ok','tqv_internal_online'],samples:rows}});
  assert(response.ok());
  await page.waitForFunction(n=>state.live.filter(r=>r.id).length>=n,before+40);
  assert(await page.evaluate(()=>!state.paused && state.pausedRows===null));
  assert.strictEqual(await page.locator('#cursorTime').innerText(),locked);
  assert.strictEqual(await page.locator('#plot-value-yaw_rate_rad_s').innerText(),selected);
  assert((await page.evaluate(()=>viewport().end))>interval[1]);
  // A pinned sample survives expiry from the rolling live buffer.
  const expired=await page.evaluate(()=>{
    const now=state.now;state.now+=120000;state.live=state.live.slice(-40);draw();
    const result={time:document.querySelector('#cursorTime').textContent,
      value:document.querySelector('#plot-value-yaw_rate_rad_s').textContent,
      hint:document.querySelector('#cursorHint').textContent};
    state.now=now;draw();return result;
  });
  assert.strictEqual(expired.time,locked);
  assert.strictEqual(expired.value,selected);
  assert(expired.hint.includes('표시 구간 밖'));
  const stamps=await page.evaluate(()=>state.live.filter(r=>r.id).slice(-40).map(r=>r.time_ms));
  assert(stamps.slice(1).every((v,i)=>v-stamps[i]===5));
  await page.locator('#live').click();
  assert(await page.evaluate(()=>!state.paused && state.lockedSample===null && state.hover===null));
  await page.waitForFunction(()=>viewport().rows.length>0);
  await canvas.scrollIntoViewIfNeeded();
  const current=await canvas.boundingBox();
  await page.mouse.move(current.x+current.width*.7,current.y+100);
  assert((await page.locator('#cursorTime').innerText()).startsWith('선택 '));
  assert((await page.locator('#plot-value-yaw_rate_rad_s').innerText()).includes('rad/s'));
  await page.mouse.click(current.x+current.width*.7,current.y+100);
  await page.locator('#window').selectOption('10');
  assert(await page.evaluate(()=>state.lockedSample===null && !state.paused));
  await page.locator('#pause').click();
  assert(await page.evaluate(()=>state.paused && state.pausedRows!==null));
  const pausedInterval=await page.evaluate(()=>[viewport().start,viewport().end]);
  const now=await page.evaluate(()=>state.now);
  await page.waitForFunction(t=>state.now>t,now);
  assert.deepStrictEqual(await page.evaluate(()=>[viewport().start,viewport().end]),pausedInterval);
  await page.setViewportSize({width:390,height:844});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.screenshot({path:'/tmp/ev-high-rate-mobile.png'});
  await page.setViewportSize({width:1440,height:1000});
  // The current BMS page shares the same plot code and keeps every upstream panel.
  await page.goto(base+'/battery');
  await page.waitForFunction(()=>document.querySelectorAll('#plots canvas').length===11);
  await page.locator('#session').selectOption('1');
  await page.waitForFunction(()=>document.querySelector('#mode').textContent.includes('1페이지'));
  assert((await page.locator('#packKw').innerText()).includes('-9.100'));
  ready=page.waitForEvent('download');await page.locator('#export').click();
  file=await ready;data=fs.readFileSync(await file.path(),'utf8');
  assert(data.trim().split(/\r?\n/).length===1011 && data.includes('battery_power_w'));
  await page.locator('#live').click();
  const bmsCanvas=page.locator('#canvas-power');await bmsCanvas.scrollIntoViewIfNeeded();
  const bb=await bmsCanvas.boundingBox();
  await page.mouse.click(bb.x+bb.width*.6,bb.y+100);
  const bmsLocked=await page.locator('#cursorTime').innerText();
  const bmsValue=await page.locator('#packKw').innerText();
  const bmsEnd=await page.evaluate(()=>viewport().end);
  assert(await page.evaluate(()=>!state.paused && state.lockedSample!==null));
  await page.mouse.move(0,0);
  const bmsResponse=await page.request.post(base+'/api/vehicle/exchange',{
    headers:{Authorization:'Bearer local-test-only-123456'},
    data:{stream_id:'0123456789abcdef',sent_ms:500,
      columns:['seq','timestamp_ms','battery_pack_voltage_v','battery_current_a','bms_online'],
      samples:Array.from({length:40},(_,i)=>[41+i,300+i*5,60,-100,true])}});
  assert(bmsResponse.ok());
  await page.waitForFunction(()=>state.current.battery_power_kw===-6);
  assert.strictEqual(await page.locator('#cursorTime').innerText(),bmsLocked);
  assert.strictEqual(await page.locator('#packKw').innerText(),bmsValue);
  assert((await page.evaluate(()=>viewport().end))>bmsEnd);
  assert(await page.evaluate(()=>!state.paused));
  await page.locator('#live').click();
  assert((await page.locator('#packKw').innerText()).includes('-6.000'));
  await page.setViewportSize({width:390,height:844});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.screenshot({path:'/tmp/ev-high-rate-bms-mobile.png'});
  await page.setViewportSize({width:1440,height:1000});
  await page.goto(base+'/records');
  await page.waitForFunction(()=>state.rows.length===1010 && !state.busy);
  assert(await page.locator('#rows tr').count()===200);
  await page.locator('#next').click();
  await page.waitForFunction(()=>state.page===1);
  await page.locator('#rows tr').first().click();
  await page.waitForFunction(()=>state.rows[state.index].id===201);
  assert((await page.locator('#detail').innerText()).includes('battery_power_w'));
  ready=page.waitForEvent('download');await page.locator('#downloadRaw').click();
  file=await ready;data=fs.readFileSync(await file.path(),'utf8');
  const all=data.trim().split('\n').map(JSON.parse);
  assert(all.length===1010 && all[1009].telemetry.seq===1010);
  assert(all.every(row=>row.telemetry.preserved_custom_field==='all fields retained'));
  await page.screenshot({path:'/tmp/ev-high-rate-records.png'});
  assert.deepStrictEqual(errors,[]);
  console.log(JSON.stringify({panels:13,historyRows:1010,batchedLiveRows:40,valueOnlyLock:true,perPlotValues:true,errors,
    screenshots:['/tmp/ev-high-rate-battery.png','/tmp/ev-high-rate-mobile.png','/tmp/ev-high-rate-records.png']}));
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
'''

with tempfile.TemporaryDirectory() as folder:
    gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                        relay_token='local-test-only-123456', database_path=Path(folder)/'test.db')
    gateway.forward_socket.close(); gateway.forward_socket = Mock()
    gateway.database.start('SYNTHETIC BROWSER TEST')
    epoch = time.time_ns()//1000000 - 10100
    with gateway.database.batch():
        for seq in range(1,1011):
            gateway.accept({'seq':seq, 'timestamp_ms':seq*10, 'sample_time_ms':epoch+seq*10,
                            'desired_yaw_rad_s':math.sin(seq/30)*.5, 'yaw_rate_rad_s':math.sin(seq/30)*.4,
                            'imu_ok':True, 'tqv_internal_online':True,
                            'battery_pack_voltage_v':52.0, 'battery_current_a':-175.0,
                            'battery_power_kw':-9.1, 'bms_online':True,
                            'preserved_custom_field':'all fields retained'}, transport='INTERNET_RELAY')
    gateway.database.stop()
    server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(gateway))
    worker=threading.Thread(target=server.serve_forever,daemon=True); worker.start()
    try:
        subprocess.run([sys.argv[1],'-e',script,f'http://127.0.0.1:{server.server_port}',sys.argv[2]],
                       check=True,timeout=90)
    finally:
        server.shutdown(); server.server_close(); worker.join(); gateway.database.close()
