'use strict';
const button=document.getElementById('toggle'),statusEl=document.getElementById('status');
const show=(id,text)=>document.getElementById(id).textContent=text;
let watchId=null,sending=false,accepted=0,failed=0,dropped=0,queue=[],busy=false,wake=null,stream='';
let source=localStorage.getItem('djy-gps-source');
if(!source){source=crypto.randomUUID();localStorage.setItem('djy-gps-source',source);}
async function keepAwake(){try{if(sending&&document.visibilityState==='visible'&&navigator.wakeLock)wake=await navigator.wakeLock.request('screen');}catch{}}
function stop(){if(watchId!==null)navigator.geolocation.clearWatch(watchId);watchId=null;sending=false;wake?.release();wake=null;button.textContent='GPS 전송 시작';button.classList.remove('active');}
function receive(p){
  if(!sending)return;
  const c=p.coords,item={ts:p.timestamp/1000,lat:c.latitude,lon:c.longitude,accuracy_m:c.accuracy,
    speed_kmh:c.speed==null?null:c.speed*3.6,heading:c.heading,altitude:c.altitude,source_id:source,stream_id:stream};
  show('speed',item.speed_kmh==null?'미제공':item.speed_kmh.toFixed(1)+' km/h');
  show('accuracy',item.accuracy_m.toFixed(1)+' m');show('lat',item.lat.toFixed(7));show('lon',item.lon.toFixed(7));
  show('heading',item.heading==null?'미제공':item.heading.toFixed(1)+'°');
  queue.push(item);if(queue.length>100){queue.shift();dropped++;}
}
async function flush(){
  if(busy||!queue.length)return;
  busy=true;const batch=queue.splice(0,10);
  try{
    const r=await fetch('/api/gnss',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({samples:batch}),signal:AbortSignal.timeout(5000)});
    const d=await r.json();if(!r.ok||!d.ok)throw Error(d.error||r.status);
    for(const result of d.results||[]){if(result.accepted)accepted++;else failed++;}
    const last=d.results?.at(-1);show('server',last?.accepted?'수신 중':last?.reason||'품질 확인');
  }catch(e){failed+=batch.length;show('server','전송 오류');statusEl.textContent=String(e);}
  finally{busy=false;statusEl.textContent=`${sending?'GNSS 전송 중':'전송 중지'} · 성공 ${accepted} / 거부·실패 ${failed} / 대기 초과 ${dropped}\n대기 ${queue.length}점 · ${document.hidden?'화면 숨김: GPS 중단 가능':'화면을 켜둔 채 사용하세요.'}\n위치 정확도 개선과 브라우저의 정확한 위치 권한을 켜주세요. 도로에 강제로 맞추지 않습니다.`;}
}
setInterval(flush,250);
button.onclick=()=>{
  if(sending){stop();return;}
  if(!isSecureContext||!navigator.geolocation){statusEl.textContent='GPS는 HTTPS 주소와 위치 권한이 필요합니다.';return;}
  stream=crypto.randomUUID();sending=true;button.textContent='GPS 전송 중지';button.classList.add('active');
  statusEl.textContent='고정밀 위치를 기다리는 중';keepAwake();
  watchId=navigator.geolocation.watchPosition(receive,e=>{failed++;show('server','GPS 오류');statusEl.textContent=e.message;},{enableHighAccuracy:true,maximumAge:0,timeout:15000});
};
document.addEventListener('visibilitychange',keepAwake);
