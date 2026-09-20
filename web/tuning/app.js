'use strict';
const $ = id => document.getElementById(id);
const batteryPage = Boolean(window.batteryDashboard);
const api = batteryPage ? '/api/battery' : '/api/tuning';
const channelStorage = batteryPage ? 'ev-battery-channels-v1' : 'ev-tuning-channels-v2';
const colors = ['#00ffff','#ffff00','#00ff00','#ff3333','#ff00ff','#ffffff'];
const groups = batteryPage ? window.batteryDashboard.groups : {yaw:'01 · Yaw 제어 / 목표·실제·STM 오차',power:'02 · 출력 배분 / 좌·우·차동전력',rpm:'03 · 모터 RPM / 측정·보고·목표',speed:'04 · 차량 속도',steering:'05 · 조향 입력',throttle:'06 · 스로틀 입력',traction:'07 · 트랙션 개입 배율',activity:'08 · TV / ED 개입 상태',voltage:'09 · 모터 전압'};
const groupTitle = group => groups[group] || (group==='readouts'?'W / kW':group==='settings'?'PID 보드 보고값':'STM 차속 원본');
const color = c => colors[state.channels.filter(x=>x.group===c.group).indexOf(c)%colors.length];
const rpmQualities = {OK:'펄스 품질 통과 · 실회전수 보정은 별도',NOISY:'노이즈 의심 · 실제 회전수로 사용 불가',NO_PULSES:'새 펄스 없음 · 정지/단선 구분 불가',UNVERIFIED:'펄스 품질 확인 중',NO_DATA:'현재 수신값 없음'};
const rpmStatusKeys = ['rpm_left_quality','rpm_right_quality','rpm_left_rejected_pct','rpm_right_rejected_pct','motor_command_mode'];
const state = {channels:[],enabled:new Set(),live:[],history:[],mode:'live',paused:false,cursor:0,epoch:'',now:0,recording:false,session:0,through:0,pages:[0],page:0,more:false,busyHistory:false,generation:0,hover:null,track:[],lastFix:''};
state.pausedRows = null;
const finite = x => typeof x === 'number' && Number.isFinite(x);
const format = (v,d=3) => finite(v)?v.toFixed(d):'미수신';
const time = t => new Date(t).toLocaleTimeString('ko-KR',{hour12:false})+'.'+String(Math.floor(t)%1000).padStart(3,'0');
async function json(url,options={}) {
  const response=await fetch(url,{cache:'no-store',signal:AbortSignal.timeout(8000),...options});
  if(!response.ok) throw new Error(`요청 실패 (${response.status})`);
  return response.json();
}
function init(channels) {
  state.channels=channels;
  let saved=null;try{saved=JSON.parse(localStorage.getItem(channelStorage));}catch{}
  state.enabled=new Set(Array.isArray(saved)?saved:channels.map(c=>c.key));
  const containers={};
  for(const group of [...new Set([...Object.keys(groups),...channels.map(c=>c.group)])]){
    const section=document.createElement('details');section.className='channel-group';section.open=innerWidth>900;
    const title=document.createElement('summary');title.textContent=groupTitle(group);section.append(title);
    $('channels').append(section);containers[group]=section;
  }
  channels.forEach(c=>{
    const label=document.createElement('label');label.className='channel';
    const input=document.createElement('input');input.type='checkbox';input.checked=state.enabled.has(c.key);
    input.onchange=()=>{input.checked?state.enabled.add(c.key):state.enabled.delete(c.key);try{localStorage.setItem(channelStorage,JSON.stringify([...state.enabled]));}catch{}draw();};
    const text=document.createElement('span');text.style.color=color(c);text.textContent=c.label;
    const value=document.createElement('b');value.id='value-'+c.key;value.textContent='미수신';
    const range=document.createElement('small');range.id='range-'+c.key;
    text.append(value,range);label.append(input,text);containers[c.group].append(label);
  });
  for(const [group,title] of Object.entries(groups)){
    const section=document.createElement('section');section.className='plot';section.id='plot-'+group;
    const heading=document.createElement('h2');heading.textContent=title;
    const legend=document.createElement('div');legend.className='legend';
    channels.filter(c=>c.group===group).forEach(c=>{const label=document.createElement('span');label.style.color=color(c);label.textContent=c.label;legend.append(label);});
    const canvas=document.createElement('canvas');canvas.id='canvas-'+group;canvas.setAttribute('role','img');canvas.setAttribute('aria-label',title+' 시간 그래프');
    section.append(heading,legend,canvas);$('plots').append(section);
    canvas.onpointermove=e=>{const rect=canvas.getBoundingClientRect();state.hover=Math.max(0,Math.min(1,(e.clientX-rect.left-72)/(rect.width-88)));draw();};
    canvas.onpointerleave=()=>{state.hover=null;draw();};
  }
}
function viewport(){
  const rows=state.mode==='live'?(state.pausedRows||state.live):state.history;
  const first=rows[0]?.time_ms??state.now;
  const last=state.mode==='live'&&!state.paused?state.now:(rows.at(-1)?.time_ms??state.now);
  const duration=$('window').value==='all'?Math.max(1000,last-first):Number($('window').value)*1000;
  const endMax=Math.max(last,first+1000);
  const end=Math.min(endMax,first+duration)+Math.max(0,endMax-first-duration)*Number($('scrub').value)/1000;
  const start=$('window').value==='all'?first:end-duration;
  return {rows:rows.filter(r=>r.time_ms>=start&&r.time_ms<=end),start,end:Math.max(start+1000,end)};
}
function surface(canvas){const r=canvas.getBoundingClientRect(),dpr=devicePixelRatio||1;canvas.width=Math.round(r.width*dpr);canvas.height=Math.round(r.height*dpr);const ctx=canvas.getContext('2d');ctx.scale(dpr,dpr);return {ctx,w:r.width,h:r.height};}
function draw(){
  if(!state.channels.length)return;
  const view=viewport();let selected=view.rows.at(-1),values=selected?.values||{},status=selected?.status||{};
  if(state.hover!==null&&view.rows.length){const stamp=view.start+(view.end-view.start)*state.hover;const row=view.rows.reduce((a,b)=>Math.abs(b.time_ms-stamp)<Math.abs(a.time_ms-stamp)?b:a);selected=row;values=row.values;status=row.status||{};$('cursor').textContent=time(row.time_ms)+' · '+state.channels.filter(c=>state.enabled.has(c.key)).map(c=>`${c.label} ${format(values[c.key])} ${c.unit}`).join(' / ');}
  else $('cursor').textContent=`${time(view.start)} → ${time(view.end)} · ${view.rows.filter(r=>r.id).length}개 · 포인터로 동시점 비교`;
  const showingCurrent=state.mode==='live'&&!state.paused&&state.hover===null&&Number($('scrub').value)===1000;
  if(showingCurrent){values=state.current||{};status=state.currentStatus||{};}
  state.channels.forEach(c=>$('value-'+c.key).textContent=!finite(values[c.key])&&c.key.endsWith('_target')?'목표값 미제공':format(values[c.key])+' '+c.unit);
  if(batteryPage){window.batteryDashboard.render(values,status,view,selected,showingCurrent);}
  else {
  $('rpmContext').textContent=showingCurrent?'현재 수신값':`선택 시점 · ${selected?time(selected.time_ms):'기록 없음'}`;
  for(const side of ['left','right']){
    const quality=status[`rpm_${side}_quality`]||'NO_DATA',rejected=status[`rpm_${side}_rejected_pct`];
    $('rpm-'+side).textContent=finite(values[`rpm_${side}`])?format(values[`rpm_${side}`],0)+' rpm':quality==='NOISY'?'노이즈 · 측정 불가':quality==='NO_PULSES'?'새 펄스 없음':'미수신/품질 미확인';
    $('reported-'+side).textContent=format(values[`rpm_${side}_reported`],0)+(finite(values[`rpm_${side}_reported`])?' rpm':'');
    $('target-'+side).textContent=finite(values[`rpm_${side}_target`])?format(values[`rpm_${side}_target`],0)+' rpm':'미제공';
    $('power-'+side).textContent=format(values[`power_${side}_kw`])+(finite(values[`power_${side}_kw`])?' kW':'');
    $('quality-'+side).textContent=(rpmQualities[quality]||rpmQualities.UNVERIFIED)+(finite(rejected)?` · 구간 펄스 폐기 ${format(rejected,1)}%`:'');
  }
  $('rpmNote').textContent=status.motor_command_mode==='POWER'?'현재 STM은 전력 명령(kW) 제어입니다. 목표 RPM은 생성·전송하지 않으며, 스로틀로 환산하지 않습니다. STM 보고 RPM은 검증 전 계산값입니다.':'목표 RPM은 송신기가 제공한 값만 표시합니다. STM 보고 RPM은 검증 전 계산값이며, 품질 통과도 실제 회전수 보정 완료를 뜻하지 않습니다.';
  $('desired').textContent=format(values.desired_yaw_rad_s);$('actual').textContent=format(values.yaw_rate_rad_s);
  $('errorYaw').textContent=finite(values.desired_yaw_rad_s)&&finite(values.yaw_rate_rad_s)?format(values.desired_yaw_rad_s-values.yaw_rate_rad_s):'—';
  const pairs=view.rows.filter(r=>r.id&&finite(r.values.desired_yaw_rad_s)&&finite(r.values.yaw_rate_rad_s));
  $('rms').textContent=pairs.length?format(Math.sqrt(pairs.reduce((a,r)=>a+(r.values.desired_yaw_rad_s-r.values.yaw_rate_rad_s)**2,0)/pairs.length)):'—';
  $('telemetryContext').textContent=$('rpmContext').textContent;
  $('speed').textContent=format(values.speed_kmh,1);
  for(const key of ['vehicle_speed_m_s','desired_yaw_rad_s','yaw_error_rad_s','delta_power_kw','traction_scale'])$('tv-'+key).textContent=format(values[key]);
  $('tvPower').textContent=`${format(values.power_left_kw)} / ${format(values.power_right_kw)}`;
  for(const key of ['tv_active','ed_active'])$('tv-'+key).textContent=finite(values[key])?(values[key]?'true · 개입':'false · 비개입'):'미수신';
  const conflict=values.tv_active===1&&values.ed_active===1;
  $('tvMode').textContent=conflict?'오류 · TV/ED 동시 개입':!finite(values.tv_active)||!finite(values.ed_active)?'미수신':values.tv_active?'TV · 폐루프':values.ed_active?'ED · 개루프':'대기 · 개입 없음';
  $('tvMode').classList.toggle('alarm',conflict);
  const gains=['pid_kp','pid_ki','pid_kd'];
  $('pid').textContent=gains.map(k=>format(values[k])).join(' / ');
  $('pidNote').textContent=gains.every(k=>finite(values[k]))?'STM 전송 설정값 · 읽기 전용':'PID 미수신 · PID 전송 지원 Rear 펌웨어 필요 (무선은 ESP도 필요)';
  }
  state.channels.forEach(c=>$('range-'+c.key).textContent=groups[c.group]?(state.enabled.has(c.key)?groupTitle(c.group):'그래프 숨김'):'수신값 표시 / CSV 기록');
  for(const group of Object.keys(groups))drawPlot(group,view);
}
function drawPlot(group,view){
  const channels=state.channels.filter(c=>c.group===group&&state.enabled.has(c.key));
  let min=Infinity,max=-Infinity;
  for(const row of view.rows)for(const c of channels){const v=row.values[c.key];if(finite(v)){min=Math.min(min,v);max=Math.max(max,v);}}
  const hasValues=Number.isFinite(min);
  if(!hasValues){min=0;max=1;}
  const margin=Math.max((max-min)*.12,group==='rpm'?10:['yaw','power','traction'].includes(group)?.02:1);
  if(group==='traction'||group==='activity'){min=-.1;max=1.1;}else{min-=margin;max+=margin;}
  const unit=state.channels.find(c=>c.group===group)?.unit||'';
  const {ctx,w,h}=surface($('canvas-'+group)),left=72,right=w-16,top=24,bottom=h-30;
  const x=t=>left+(t-view.start)/(view.end-view.start)*(right-left),y=v=>bottom-(v-min)/(max-min)*(bottom-top);
  ctx.fillStyle='#000000';ctx.fillRect(0,0,w,h);ctx.font='bold 12px Consolas,monospace';ctx.lineWidth=1;
  ctx.fillStyle='#ffffff';ctx.fillText(unit,left,12);
  for(let i=0;i<=4;i++){
    const yy=top+(bottom-top)*i/4,xx=left+(right-left)*i/4;
    ctx.strokeStyle='#555555';ctx.beginPath();ctx.moveTo(left,yy);ctx.lineTo(right,yy);ctx.moveTo(xx,top);ctx.lineTo(xx,bottom);ctx.stroke();
    ctx.fillStyle='#ffffff';ctx.textAlign='right';ctx.fillText((max-(max-min)*i/4).toFixed(group==='rpm'?0:2),left-8,yy+4);
  }
  ctx.textAlign='left';ctx.fillText(time(view.start),left,h-8);ctx.textAlign='right';ctx.fillText(time(view.end),right,h-8);
  for(const c of channels){
    ctx.strokeStyle=color(c);ctx.fillStyle=ctx.strokeStyle;ctx.lineWidth=3;
    ctx.setLineDash(c.key.endsWith('_reported')?[2,5]:c.key==='desired_yaw_rad_s'||c.key.endsWith('_target')?[9,5]:[]);ctx.beginPath();let prev=null;
    for(const r of view.rows){const v=r.values[c.key];if(!finite(v)){prev=null;continue;}if(prev===null||r.time_ms-prev.time_ms>(batteryPage?3000:1500))ctx.moveTo(x(r.time_ms),y(v));else{if(group==='activity'||(batteryPage&&c.key.startsWith('battery_'))||(batteryPage&&c.key.startsWith('bms_')))ctx.lineTo(x(r.time_ms),y(prev.values[c.key]));ctx.lineTo(x(r.time_ms),y(v));}prev=r;}ctx.stroke();ctx.setLineDash([]);
    if(view.rows.length<150)for(const r of view.rows){const v=r.values[c.key];if(finite(v)){ctx.beginPath();ctx.arc(x(r.time_ms),y(v),2.5,0,Math.PI*2);ctx.fill();}}
  }
  if(!hasValues||!channels.length){ctx.fillStyle='#ffff00';ctx.textAlign='center';ctx.fillText(channels.length?'유효한 수신값 없음':'표시 신호 없음',w/2,h/2);}
  if(state.hover!==null){ctx.strokeStyle='#ffffff';ctx.setLineDash([4,4]);ctx.beginPath();ctx.moveTo(left+(right-left)*state.hover,top);ctx.lineTo(left+(right-left)*state.hover,bottom);ctx.stroke();ctx.setLineDash([]);}
}
async function sessions(){const data=await json(api+'/sessions');const selected=$('session').value;$('session').replaceChildren(new Option('과거 측정 선택',''));for(const s of data.sessions)$('session').add(new Option(`#${s.id} · ${s.label||'측정'} · ${new Date(s.started_at_utc).toLocaleString('ko-KR')} · ${s.sample_count}개`,s.id));$('session').value=selected;}
async function history(page=0){
  const generation=++state.generation;state.busyHistory=true;$('prev').disabled=$('next').disabled=true;
  try{const data=await json(`${api}/samples?session_id=${state.session}&after=${state.pages[page]||0}&through=${state.through}`);if(generation!==state.generation)return;state.history=data.samples;state.through=data.through;state.page=page;state.pages[page+1]=data.cursor;state.more=data.more;$('scrub').value=1000;$('mode').textContent=`과거 세션 #${state.session} · ${page+1}페이지 · ${data.samples.length}개`;$('error').textContent='';draw();}
  catch(e){$('error').textContent=e.message;}finally{if(generation===state.generation){state.busyHistory=false;navigation();}}
}
function navigation(){$('prev').disabled=state.mode!=='history'||state.busyHistory||state.page===0;$('next').disabled=state.mode!=='history'||state.busyHistory||!state.more;$('pause').disabled=state.mode!=='live';}
async function poll(){
  let catchUp=false;
  try{
    const data=await json(api+'/live?after='+state.cursor);
    if(!state.channels.length)init(data.channels);
    if(state.epoch&&state.epoch!==data.epoch){state.cursor=0;state.live=[];state.epoch=data.epoch;$('notice').textContent='게이트웨이가 재시작되어 실시간 버퍼를 새로 불러옵니다.';return;}
    state.epoch=data.epoch;state.cursor=data.cursor;state.current=data.current;state.currentStatus=data.current_status||{};state.now=data.now_ms;
    if(data.truncated)$('notice').textContent='실시간 버퍼를 벗어난 구간이 있습니다. 저장한 측정 세션에서 확인하세요.';
    // Pause only freezes the view. Every fetched sample still enters the
    // buffer, and a multi-page response is drained without render throttling.
    state.live.push(...data.samples);
    if(!data.more&&Object.values(data.current).every(v=>v===null))state.live.push({id:0,time_ms:data.now_ms,values:data.current,status:data.current_status});
    state.live=state.live.slice(-12000);
    catchUp=data.more;
    if(data.relay_dropped_samples>0)$('notice').textContent=`차량 전송 버퍼 초과: ${data.relay_dropped_samples}개 표본 누락. 통신 상태를 확인하세요.`;
    RecordingClock.update(data);state.recording=data.recording_active;$('recordStatus').textContent=data.recording_active?`● 기록 중 · 세션 #${data.recording_session_id} · ${data.recording_sample_count}개`:'기록 대기 · 측정 시작부터 DB에 저장';
    if(data.recording_error)$('recordStatus').textContent=data.recording_error;
    else if(data.recording_active&&(!data.recording_last_received_at_utc||data.now_ms-Date.parse(data.recording_last_received_at_utc)>3000))$('recordStatus').textContent+=' · 수신 대기';
    $('record').textContent=data.recording_active?'측정 종료':'측정 시작';$('record').disabled=!data.can_record;$('label').disabled=data.recording_active||!data.can_record;$('rawLink').hidden=false;
    const wired=data.input_mode==='STM_USB';
    $('link').textContent=wired?(data.input_online?`● STM USB ${data.input_port} · ${format(data.receive_rate_hz,1)} Hz 수신`:`○ STM USB ${data.input_port} · 미수신`):(Object.values(data.current).some(finite)?'● 실측 신호 수신 중':'○ 게이트웨이 연결됨 · 센서 미수신');
    $('signalStatus').textContent=wired?(data.input_online?data.signal_warning:'USB 수신 중단 · 마지막 값은 현재값으로 표시하지 않습니다.') : '';
    if(state.mode==='live')$('mode').textContent=state.paused?'화면 정지 · 표본 수집은 계속됨':`실시간 · 버퍼 ${state.live.filter(r=>r.id).length}점`;
    draw();
  }catch(e){$('link').textContent='게이트웨이 연결 끊김';$('signalStatus').textContent='';$('error').textContent=e.message;$('record').disabled=true;state.current={};state.currentStatus={};if(state.mode==='live'&&!state.paused)draw();}
  finally{setTimeout(poll,catchUp?0:250);}
}
$('record').onclick=async()=>{const active=state.recording;$('record').disabled=true;try{await json('/api/records/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:active?'stop':'start',label:$('label').value})});await sessions();$('error').textContent='';}catch(e){$('error').textContent=e.message;}};
$('session').onchange=()=>{if(!$('session').value)return;state.mode='history';state.session=Number($('session').value);state.pages=[0];state.through=0;state.history=[];navigation();history();};
$('live').onclick=()=>{state.generation++;state.mode='live';state.paused=false;state.pausedRows=null;state.busyHistory=false;$('pause').textContent='화면 일시정지';$('scrub').value=1000;$('session').value='';navigation();draw();};
$('pause').onclick=()=>{state.paused=!state.paused;state.pausedRows=state.paused?state.live.slice():null;$('pause').textContent=state.paused?'실시간 재개':'화면 일시정지';$('scrub').value=1000;draw();};
$('prev').onclick=()=>history(state.page-1);$('next').onclick=()=>history(state.page+1);
$('scrub').oninput=()=>{if(state.mode==='live'&&!state.paused){state.paused=true;state.pausedRows=state.live.slice();$('pause').textContent='실시간 재개';}draw();};$('window').onchange=draw;$('reload').onclick=()=>sessions().catch(e=>$('error').textContent=e.message);
$('export').onclick=()=>{
  const rows=state.mode==='live'?(state.pausedRows||state.live).filter(r=>r.id):state.history;
  const keys=state.channels.map(c=>c.key);
  const statusKeys=batteryPage?['bms_online','bms_fault','bms_state','bms_age_ms','bms_charge_mos_on','bms_discharge_mos_on','bms_alarm_hex',...Array.from({length:48},(_,i)=>`cell_${i+1}_v`)]:rpmStatusKeys;
  const escape=v=>'"'+String(v??'').replaceAll('"','""')+'"';
  const statusValue=(r,k)=>k.startsWith('cell_')?r.status?.bms_cell_voltages_v?.[Number(k.split('_')[1])-1]:r.status?.[k];
  const csv=[['captured_at_utc','source_timestamp_ms',...keys,...statusKeys].map(escape).join(','),...rows.map(r=>[new Date(r.time_ms).toISOString(),r.source_timestamp_ms??'',...keys.map(k=>finite(r.values[k])?r.values[k]:''),...statusKeys.map(k=>statusValue(r,k))].map(escape).join(','))].join('\r\n');
  const url=URL.createObjectURL(new Blob(['\uFEFF'+csv],{type:'text/csv;charset=utf-8'}));
  const a=document.createElement('a');a.href=url;a.download=`${batteryPage?'bms':'ev'}-${state.mode}-${state.session||'live'}-page-${state.page+1}.csv`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
};
$('share').onclick=async()=>{try{await navigator.clipboard.writeText(location.origin+(batteryPage?'/battery':'/pit'));$('notice').textContent='주소 복사됨. 다른 기기에는 localhost 대신 ngrok 주소를 공유하세요.';}catch{$('notice').textContent=location.origin+(batteryPage?'/battery':'/pit');}};
window.addEventListener('resize',draw);

async function overview(){try{const t=await json('/api/tuning/overview');$('vehicle').textContent=`STM ${t.stm_online?'수신':'미수신'} · IMU ${t.imu_online?'수신':'미수신'} · TV ${t.tqv_internal_online?(t.tv_active?'개입':'대기'):'미수신'} · 속도 ${t.speed_online?format(t.speed_kmh,1)+' km/h':'미수신'}`;
  $('battery').textContent=t.bms_online?`팩 ${format(t.battery_pack_voltage_v,1)} V · 잔량 ${format(t.battery_soc_pct,1)} % · 전류 ${format(t.battery_current_a,1)} A`:'BMS 미수신';$('cells').replaceChildren();if(t.bms_online)for(const [i,v] of (t.bms_cell_voltages_v||[]).entries()){const cell=document.createElement('div');cell.className='cell';cell.textContent=`셀 ${i+1} · ${format(v)} V`;$('cells').append(cell);}
  $('gnss').textContent=t.gnss_online?`${format(t.gnss_latitude_deg,6)}, ${format(t.gnss_longitude_deg,6)} · 정확도 ${format(t.gnss_accuracy_m,1)} m`:'위치 미수신';

}catch{$('vehicle').textContent='게이트웨이 연결 끊김';$('battery').textContent='현재 BMS 상태 확인 불가';$('gnss').textContent='현재 위치 상태 확인 불가';$('cells').replaceChildren();}finally{setTimeout(overview,1000);}}
sessions().catch(e=>$('error').textContent=e.message);poll();if(!batteryPage)overview();
