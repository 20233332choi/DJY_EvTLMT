'use strict';
const $=id=>document.getElementById(id),finite=v=>typeof v==='number'&&Number.isFinite(v);
const fmt=(v,d=1)=>finite(v)?v.toFixed(d):'—';
const lapText=v=>finite(v)?`${Math.floor(v/60)}:${(v%60).toFixed(2).padStart(5,'0')}`:'—';
const embedded=new URLSearchParams(location.search).has('embed');
if(embedded)document.body.classList.add('embedded');
const map=L.map('map',{preferCanvas:true}).setView([35.14358,126.93324],15);
const tiles=L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'}).addTo(map);
let tileFailed=false;
tiles.on('tileerror',()=>{tileFailed=true;$('tileStatus').textContent='배경 지도 연결 실패 · 인터넷 연결을 확인하세요. 좌표와 랩 판정은 계속 유지됩니다.';});
tiles.on('loading',()=>{tileFailed=false;});tiles.on('load',()=>{if(!tileFailed)$('tileStatus').textContent='OpenStreetMap · © OpenStreetMap contributors';});
L.control.scale({imperial:false}).addTo(map);
const rawTrail=L.polyline([],{color:'#888',weight:2,opacity:.8}).addTo(map);
const trail=L.polyline([],{color:'#00c9dc',weight:3}).addTo(map);
const gateLayer=L.layerGroup().addTo(map);
let marker=null,accuracy=null,first=false,follow=true,mode='live',cursor=0,epoch='',rows=[],liveRows=[],liveState=null,gate=null,draft=null,pick=null,picked=[],canEdit=false,busy=false,historySerial=0,page=0,pages=[0],through=0,historyLaps=[];
let loadedGateKey='';
let gateHandle=null,gateDragging=false,canTimeEdit=false;
const timingControls=new Set(['currentGate','pickGate','gateAngle','gateAngleSlider','gateWidth','reverse','saveGate','cancelGate','run']);
const gateEditable=()=>canTimeEdit&&!liveState?.active&&mode==='live'&&!busy;
function gateGeometry(g){
  const center=[(g.a[0]+g.b[0])/2,(g.a[1]+g.b[1])/2],c=Math.cos(center[0]*Math.PI/180);
  const east=(g.b[1]-g.a[1])*111195*c,north=(g.b[0]-g.a[0])*111195;
  return {center,width:Math.hypot(east,north),angle:(Math.atan2(east,north)*180/Math.PI+360)%360};
}
function makeGate(center,angle,width){
  const rad=angle*Math.PI/180,meters=6371000*Math.PI/180,target=Math.max(5.00001,Math.min(199.99999,width));
  let length=target,g;
  // Match the server's endpoint-based projection, including at the 5/200 m limits.
  for(let i=0;i<3;i++){
    const north=Math.cos(rad)*length/2/meters,east=Math.sin(rad)*length/2/(meters*Math.cos(center[0]*Math.PI/180));
    g={a:[center[0]-north,center[1]-east],b:[center[0]+north,center[1]+east]};
    const actual=Math.hypot((g.b[0]-g.a[0])*meters,(g.b[1]-g.a[1])*meters*Math.cos(g.a[0]*Math.PI/180));
    length*=target/actual;
  }
  return g;
}
function syncGateControls(g){
  if(!g){$('gateGeometry').textContent='현재 위치 또는 지도 두 점으로 선을 만드세요.';return;}
  const v=gateGeometry(g);
  $('gateAngle').value=$('gateAngleSlider').value=v.angle.toFixed(1);$('gateWidth').value=v.width.toFixed(1);
  $('gateGeometry').textContent=`중심 ${v.center.map(p=>p.toFixed(7)).join(', ')} · 폭 ${fmt(v.width)} m · 통과 방향 ${fmt((v.angle+270)%360,1)}° (북 0 / 동 90)`;
}
function previewGate(g){draft=g;pick=null;picked=[];syncGateControls(g);renderGate(g);$('gateStatus').textContent='출발선 미리보기 · 저장해야 적용됩니다.';$('run').disabled=true;}
function adjustGate(){
  if(!gateEditable())return;const g=draft||gate;if(!g){$('error').textContent='현재 위치 또는 지도 두 점으로 선을 먼저 만드세요.';return;}
  const angle=Number($('gateAngle').value),width=Number($('gateWidth').value);
  if(!$('gateAngle').value||!$('gateWidth').value||!finite(angle)||angle<0||angle>=360||!finite(width)||width<5||width>200){$('error').textContent='각도는 0~359.9°, 폭은 5~200m로 입력하세요.';return;}
  $('error').textContent='';previewGate({...g,...makeGate(gateGeometry(g).center,angle,width)});
}
const reasons={GPS_STALE:'위치 미수신 / 오래된 위치',WAITING_GPS:'GPS 대기',READY:'GPS 준비',TIMING:'랩 측정 중',WAITING_CROSSING:'첫 정방향 통과 대기',SAMPLE_GAP:'GPS 공백 · 현재 랩 무효',POSITION_JUMP:'위치 튐 · 현재 랩 무효',LOW_ACCURACY:'위치 정확도 부족',TIMING_ACCURACY:'지도 표시 가능 · 랩 정확도 부족',DELAYED_FIX:'늦게 도착한 위치 · 랩 무효',NO_SOURCE_TIME:'측정 시각 없음 · 랩 불가'};
async function json(url,options){const r=await fetch(url,{cache:'no-store',...options,signal:AbortSignal.timeout(6000)});const d=await r.json();if(!r.ok||d.ok===false)throw Error(d.error||r.status);return d;}
function segments(points,raw=false){let groups=[],part=[],key=null;for(const p of points){const next=`${p.segment}:${p.source_id}:${p.stream_id}`;if(key!==next||(!raw&&!p.accepted)){if(part.length)groups.push(part);part=[];}key=next;if(raw||p.accepted)part.push(raw?[p.raw_lat,p.raw_lon]:[p.lat,p.lon]);}if(part.length)groups.push(part);return groups;}
function renderTrack(){trail.setLatLngs(segments(rows));rawTrail.setLatLngs(segments(rows,true));}
function showFix(p,live){
  if(!p){$('position').textContent='위치 미수신';$('quality').textContent='위치 미수신';$('speed').textContent='GPS 속도 —';$('sample').textContent='측정 간격 —';if(marker){map.removeLayer(marker);map.removeLayer(accuracy);marker=accuracy=null;}return;}
  $('position').textContent=`${p.lat.toFixed(7)}, ${p.lon.toFixed(7)}`;
  $('speed').textContent=`GPS 속도 ${fmt(p.speed_kmh)} km/h · 진행 ${fmt(p.heading,0)}°`;
  const before=rows.length>1?rows.at(-2):null;
  $('sample').textContent=`측정 간격 ${before?fmt(p.ts-before.ts,2):'—'} s · 수신 시각 차이 ${fmt(p.received_ts-p.ts,2)} s`;
  $('quality').textContent=`정확도 ${fmt(p.accuracy_m)} m · ${live?'현재 위치':'과거 / 미수신'}`;
  const point=[p.lat,p.lon],icon=L.divIcon({className:'',iconSize:[36,36],iconAnchor:[18,18],html:`<div class="car-arrow" style="color:${live?'#00ffff':'#aaa'};transform:rotate(${finite(p.heading)?p.heading:0}deg)">${finite(p.heading)?'▲':'●'}</div>`});
  if(!marker){marker=L.marker(point,{icon}).addTo(map);accuracy=L.circle(point,{color:'#00aabc',weight:1,fillOpacity:.1,radius:p.accuracy_m}).addTo(map);}
  else{marker.setLatLng(point).setIcon(icon);accuracy.setLatLng(point).setRadius(p.accuracy_m);}
  if(!first&&p.accepted){map.setView(point,18);first=true;}
  else if(follow&&live&&p.accepted)map.panTo(point,{animate:false});
}
function renderGate(g){
  if(gateDragging)return;
  gateLayer.clearLayers();gateHandle=null;if(!g)return;
  L.polyline([g.a,g.b],{color:'#fff',weight:9}).addTo(gateLayer);L.polyline([g.a,g.b],{color:'#ff3333',weight:5}).addTo(gateLayer);
  const center=[(g.a[0]+g.b[0])/2,(g.a[1]+g.b[1])/2],c=Math.cos(center[0]*Math.PI/180),dx=(g.b[1]-g.a[1])*c,dy=g.b[0]-g.a[0],len=Math.hypot(dx,dy),scale=12/111195;
  const end=[center[0]+dx/len*scale,center[1]-dy/len*scale/c];
  L.polyline([center,end],{color:'#ffff00',weight:4}).addTo(gateLayer);
  const angle=Math.atan2(-dy,dx)*180/Math.PI;
  L.marker(end,{interactive:false,icon:L.divIcon({className:'',iconSize:[28,28],iconAnchor:[14,14],html:`<div style="font-size:24px;color:#ffff00;text-shadow:0 0 3px #000;transform:rotate(${angle}deg)">▲</div>`})}).addTo(gateLayer);
  L.marker(center,{interactive:false,icon:L.divIcon({className:'',iconAnchor:[20,-10],html:'<b class="gate-label">START</b>'})}).addTo(gateLayer);
  if(gateEditable()){
    gateHandle=L.marker(center,{draggable:true,icon:L.divIcon({className:'gate-handle',iconSize:[24,24],iconAnchor:[12,12],html:'↔'}),title:'출발선 중심 이동'}).addTo(gateLayer);
    gateHandle.on('dragstart',()=>{gateDragging=true;follow=false;$('follow').textContent='차량 따라가기 OFF';});
    gateHandle.on('dragend',e=>{gateDragging=false;if(!gateEditable()){renderGate(draft||gate);return;}const p=e.target.getLatLng(),v=gateGeometry(g);previewGate({...g,...makeGate([p.lat,p.lng],v.angle,v.width)});});
  }
}
function renderLaps(laps){$('laps').replaceChildren();for(const lap of [...laps].reverse()){const tr=document.createElement('tr');for(const value of [lap.number,lapText(lap.lap_time_s),`약 ${fmt(lap.estimated_error_s,2)} s`,`${fmt(lap.distance_m,0)} m`]){const td=document.createElement('td');td.textContent=value;tr.append(td);}$('laps').append(tr);}}
function renderStatus(d){
  liveState=d;canEdit=d.can_edit&&!embedded;
  canTimeEdit=(d.can_time_edit??d.can_edit)&&!embedded;
  $('link').textContent=d.live?'GPS 수신 중':'GPS 대기';
  $('access').textContent=canEdit?'현재 피트 PC · 설정 / 측정 가능':canTimeEdit?'인터넷 연결 · 출발선 설정 / 랩 측정 가능':'읽기 전용';
  for(const el of document.querySelectorAll('[data-edit]'))el.disabled=!(timingControls.has(el.id)?canTimeEdit:canEdit)||(d.active&&el.id!=='run')||mode!=='live';
  const fixReady=d.live&&d.latest?.timing_ok;
  $('currentGate').disabled=!gateEditable()||!fixReady;
  $('gateFixStatus').textContent=fixReady?`현재 GPS로 생성 가능 · 정확도 ${fmt(d.latest.accuracy_m)} m`:'현재 위치 생성에는 정확도 10m 이내의 최신 GPS가 필요합니다.';
  for(const id of ['gateAngle','gateAngleSlider','gateWidth','reverse','saveGate','cancelGate'])$(id).disabled=!gateEditable()||!(draft||d.gate);
  $('run').disabled=!canTimeEdit||mode!=='live'||(!d.active&&!!draft);
  $('run').textContent=d.active?'랩 측정 종료':'랩 측정 시작';
  if(mode!=='live')return;
  gate=d.gate;if(!draft)renderGate(gate);
  const gateKey=JSON.stringify(gate);if(gate&&gateKey!==loadedGateKey&&!draft){syncGateControls(gate);loadedGateKey=gateKey;}
  if(pick!=='gate')$('gateStatus').textContent=draft?'선택한 출발선 · 저장 대기':gate?'저장된 출발선 사용 중':'출발선 없음';
  $('currentLap').textContent=lapText(d.lap_time_s);$('lastLap').textContent=lapText(d.last_lap_s);$('bestLap').textContent=lapText(d.best_lap_s);$('lapCount').textContent=d.lap_count;
  $('timingState').textContent=`${reasons[d.reason]||d.reason} · 거부 ${d.rejected}점`;
  const c=d.calibration,r=d.calibration_report;
  $('calStatus').textContent=c?`기준점 보정: 동 ${fmt(c.east_m)} m / 북 ${fmt(c.north_m)} m · ${d.latest?.calibration_id===c.id?'적용 중':'현재 위치에는 미적용'}`:'기준점 보정 없음';
  $('calReport').textContent=r.collecting?`${r.count}점 · ${fmt(r.elapsed_s,0)}초 · 흔들림 95% ${fmt(r.spread95_m)} m · ${r.ready&&r.recent?'적용 가능':'정지·정확도 조건 확인 중'}`:'수집 대기';
  $('calibrate').disabled=!canEdit||d.active||!r.ready||!r.recent;
  renderLaps(d.laps);showFix(d.latest,d.live);
}
async function poll(){let delay=500;try{
  const d=await json(`/api/gnss/live?after=${cursor}`);
  if(epoch&&d.epoch!==epoch){cursor=0;liveRows=[];epoch=d.epoch;delay=0;return;}
  epoch=d.epoch;cursor=d.cursor;liveRows.push(...d.points);liveRows=liveRows.slice(-12000);
  if(mode==='live'){rows=liveRows;renderTrack();}
  renderStatus(d);if(d.more)delay=0;
}catch(e){$('link').textContent='게이트웨이 연결 끊김';$('quality').textContent='현재 위치 확인 불가';$('currentLap').textContent='—';for(const el of document.querySelectorAll('[data-edit]'))el.disabled=true;}finally{setTimeout(poll,delay);}}
async function command(action,extra={}){if(busy)return;busy=true;try{$('error').textContent='';await json('/api/gnss/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,...extra})});if(action==='stop')await runs();}catch(e){$('error').textContent=e.message;}finally{busy=false;}}
$('follow').onclick=()=>{follow=!follow;$('follow').textContent=`차량 따라가기 ${follow?'ON':'OFF'}`;};
map.on('dragstart',()=>{follow=false;$('follow').textContent='차량 따라가기 OFF';});
$('fit').onclick=()=>{if(trail.getLatLngs().length)map.fitBounds(trail.getBounds(),{padding:[25,25],maxZoom:19});};
$('startView').onclick=()=>{const g=draft||gate;if(g)map.fitBounds([g.a,g.b],{padding:[50,50],maxZoom:19});};
$('raw').onchange=()=>{$('raw').checked?rawTrail.addTo(map):map.removeLayer(rawTrail);};
$('pickGate').onclick=()=>{pick='gate';picked=[];$('gateStatus').textContent='지도에서 출발선 양 끝을 차례로 클릭하세요.';};
$('currentGate').onclick=()=>{if(!gateEditable()||!liveState?.live||!liveState.latest?.timing_ok)return;const p=liveState.latest;previewGate(makeGate([p.lat,p.lon],90,20));$('error').textContent='';follow=false;$('follow').textContent='차량 따라가기 OFF';map.fitBounds([draft.a,draft.b],{padding:[60,60],maxZoom:19});};
$('gateAngleSlider').oninput=()=>{$('gateAngle').value=$('gateAngleSlider').value;adjustGate();};
$('gateAngle').onchange=adjustGate;$('gateWidth').onchange=adjustGate;
$('cancelGate').onclick=()=>{if(!gateEditable())return;draft=null;pick=null;loadedGateKey='';syncGateControls(gate);renderStatus(liveState);};
$('pickRef').onclick=()=>{pick='reference';$('calReport').textContent='지도에서 실제 정지 위치를 클릭하세요.';};
map.on('click',e=>{if(!gateEditable())return;const p=[e.latlng.lat,e.latlng.lng];if(pick==='reference'){$('refLat').value=p[0].toFixed(7);$('refLon').value=p[1].toFixed(7);$('refType').value='map';pick=null;}else if(pick==='gate'){picked.push(p);if(picked.length===2){const g={a:picked[0],b:picked[1]},v=gateGeometry(g);if(v.width<5||v.width>200){picked=[];$('error').textContent='선의 두 끝은 5~200m 간격으로 선택하세요.';return;}$('error').textContent='';previewGate(g);}}});
$('reverse').onclick=()=>{const g=draft||gate;if(g&&gateEditable())previewGate({...g,a:g.b,b:g.a});};
$('saveGate').onclick=async()=>{const g=draft||gate;if(!g){$('error').textContent='지도에서 두 점을 먼저 선택하세요.';return;}await command('gate',{a:g.a,b:g.b});if(!$('error').textContent)draft=null;};
$('collect').onclick=()=>command('collect');
$('calibrate').onclick=()=>{if(!$('refLat').value||!$('refLon').value){$('error').textContent='기준 좌표를 입력하세요.';return;}command('calibrate',{lat:Number($('refLat').value),lon:Number($('refLon').value),reference_type:$('refType').value});};
$('clearCal').onclick=()=>command('clear_calibration');
$('run').onclick=()=>{if(!liveState?.active&&draft){$('error').textContent='편집한 출발선을 저장하거나 편집을 취소하세요.';return;}command(liveState?.active?'stop':'start',{label:$('label').value});};
async function runs(){const d=await json('/api/gnss/runs'),selected=$('history').value;$('history').replaceChildren(new Option('실시간 보기',''));for(const r of d.runs)$('history').append(new Option(`${r.id} · ${r.label||'GPS 측정'} · ${new Date(r.started*1000).toLocaleString()}`,r.id));$('history').value=selected;}
async function history(){const serial=++historySerial;try{
  const id=$('history').value;if(!id)return;
  const d=await json(`/api/gnss/history?run_id=${id}&after=${pages[page]}&through=${through}`);if(serial!==historySerial||mode==='live')return;
  through=d.through;pages[page+1]=d.cursor;rows=d.points;historyLaps=d.laps;renderTrack();gate=d.settings.gate;renderGate(gate);showFix(rows.at(-1),false);renderLaps(d.laps);
  $('currentLap').textContent='—';$('lastLap').textContent=lapText(d.laps.at(-1)?.lap_time_s);$('bestLap').textContent=lapText(d.laps.length?Math.min(...d.laps.map(p=>p.lap_time_s)):null);$('lapCount').textContent=d.laps.length;
  $('mode').textContent=`과거 기록 ${id} · ${page+1}페이지 · ${rows.length}점`;$('timingState').textContent='저장된 측정 결과';$('prev').disabled=page===0;$('next').disabled=!d.more;$('fit').click();
}catch(e){$('error').textContent=e.message;}}
$('history').onchange=()=>{mode=$('history').value?'history':'live';historySerial++;draft=null;pick=null;page=0;pages=[0];through=0;if(mode==='history')history();else{rows=liveRows;$('mode').textContent='실시간';$('prev').disabled=$('next').disabled=true;renderTrack();if(liveState)renderStatus(liveState);}};
$('prev').onclick=()=>{page--;history();};$('next').onclick=()=>{page++;history();};$('reload').onclick=()=>runs().catch(e=>$('error').textContent=e.message);
function exportRows(data,keys,name){const escape=v=>'"'+String(v??'').replaceAll('"','""')+'"',csv=[keys,...data.map(p=>keys.map(k=>p[k]))].map(r=>r.map(escape).join(',')).join('\r\n'),url=URL.createObjectURL(new Blob(['\uFEFF'+csv],{type:'text/csv;charset=utf-8'})),a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
$('csv').onclick=()=>exportRows(rows,['ts','received_ts','raw_lat','raw_lon','lat','lon','accuracy_m','speed_kmh','heading','altitude','accepted','timing_ok','reason','source_id','stream_id','calibration_id','segment'],`gps-${mode}-${page+1}.csv`);
$('lapCsv').onclick=()=>exportRows(mode==='live'?(liveState?.laps||[]):historyLaps,['number','start_ts','end_ts','lap_time_s','distance_m','estimated_error_s','calibration_id'],'gps-laps.csv');
runs().catch(e=>$('error').textContent=e.message);poll();
