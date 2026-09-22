'use strict';
// Layout/interaction reference: 20233332choi/ASC_TLMTSYS static/analysis.{html,js}.
// Raw samples remain intact; selection always resolves to an actual stored row.
const $=id=>document.getElementById(id), finite=v=>typeof v==='number'&&Number.isFinite(v);
const CATEGORIES={battery:'배터리 · BMS 실측',power:'모터 출력 요구 · 실측 아님',tv:'토크벡터링 · yaw / PID',drive:'구동 · RPM / 차속',inputs:'운전자 입력 · 조향 / 스로틀',cells:'배터리 셀 전압',gps:'GPS · 위치 / 품질'};
const COLORS=['#00e7ff','#ffce54','#ff729f','#67ff9b','#bd99ff','#ff9d51','#88bfff','#ffffff'];
const DEFAULT=['battery_power_w','battery_pack_voltage_v','battery_current_a','power_command_total_w','desired_yaw_rad_s','yaw_rate_rad_s','speed_kmh','tps_pct'];
const state={sessions:[],sid:0,rows:[],channels:[],selected:new Set(DEFAULT),active:false,busy:false,generation:0,controller:null,
  session:null,through:0,range:[0,1],domain:[0,1],yRanges:{},plots:[],index:0,pinned:false,page:0,visible:[],axis:'sample',requestedX:null,powerUnit:'kW'};
try{if(localStorage.getItem('djy-analysis-power-unit')==='W')state.powerUnit='W';}catch{}
try{const v=JSON.parse(localStorage.getItem('djy-analysis-channels'));if(Array.isArray(v))state.selected=new Set(v.filter(k=>typeof k==='string'));}catch{}
let frame=0,needBase=false,needTable=false,needMap=false,drag=null;
const fmt=(v,d=3)=>finite(v)?v.toLocaleString('en-US',{minimumFractionDigits:d,maximumFractionDigits:d}):'—';
const displayUnit=unit=>unit==='W'?state.powerUnit:unit;
const displayValue=(v,unit)=>finite(v)&&unit==='W'&&state.powerUnit==='kW'?v/1000:v;
const formatValue=(v,unit,d=4)=>unit==='W'&&state.powerUnit==='kW'
  ?finite(v)?displayValue(v,unit).toLocaleString('en-US',{minimumFractionDigits:2,maximumFractionDigits:5}):'—'
  :fmt(v,unit==='W'?2:d);
const powerText=v=>`${formatValue(v,'W')} ${state.powerUnit}`;
function syncPowerUnit(){
  $('powerUnit').value=state.powerUnit;
  $('powerColumn').textContent=`배터리 (${state.powerUnit})`;
  $('peak').textContent=`구간 최대 |${state.powerUnit}| 지점`;
  $('mapPowerNote').textContent=`선분 색상은 끝점 표본의 전력 크기 |${state.powerUnit}| · 선택값은 원본 부호 유지`;
}
function setPowerUnit(unit){
  if(!['W','kW'].includes(unit)||unit===state.powerUnit)return;
  const factor=unit==='kW'?.001:1000;
  for(const id of Object.keys(state.yRanges))if(id.endsWith(':W'))state.yRanges[id]=state.yRanges[id].map(v=>v*factor);
  state.powerUnit=unit;
  try{localStorage.setItem('djy-analysis-power-unit',unit);}catch{}
  syncPowerUnit();renderCategories();renderPanels();schedule(true,true,true);
}
const stamp=v=>v?new Date(v).toLocaleString('ko-KR',{hour12:false})+'.'+String(new Date(v).getMilliseconds()).padStart(3,'0'):'—';
const x=row=>state.axis==='receive'?row.elapsed_s:(row.sample_elapsed_s??row.elapsed_s);
const channel=key=>state.channels.find(c=>c.key===key);
const color=key=>COLORS[Math.max(0,state.channels.findIndex(c=>c.key===key))%COLORS.length];
const displayChannels=()=>state.channels.filter(c=>!['battery_power_kw','power_left_kw','power_right_kw','power_command_total_kw','delta_power_kw'].includes(c.key));
const chosen=()=>displayChannels().filter(c=>state.selected.has(c.key));
async function api(path,options={}){const r=await fetch(path,options);let d;try{d=await r.json();}catch{throw Error('서버 응답을 읽지 못했습니다.');}if(!r.ok||d.ok===false)throw Error(d.error||`HTTP ${r.status}`);return d;}
function fail(e){$('error').textContent=e.message;}
function renderSessions(){
  const query=$('search').value.toLowerCase();$('sessions').replaceChildren();
  for(const s of state.sessions.filter(s=>`${s.label} ${s.started_at_utc} ${stamp(s.started_at_utc)}`.toLowerCase().includes(query))){
    const b=document.createElement('button');b.className='session'+(s.id===state.sid?' selected':'');
    const title=document.createElement('b');title.textContent=`#${s.id} · ${s.label||'이름 없는 측정'}`;
    const info=document.createElement('small');info.textContent=`${stamp(s.started_at_utc)} · ${s.sample_count.toLocaleString()}개 · ${s.stopped_at_utc?'종료':'기록 중'}`;
    b.append(title,info);b.onclick=()=>select(s.id);$('sessions').append(b);
  }
  if(!$('sessions').children.length)$('sessions').textContent='저장된 세션이 없습니다.';
}
async function sessions(){state.sessions=(await api('/api/records/sessions')).sessions;renderSessions();}
async function select(sid){
  state.controller?.abort();const controller=new AbortController(),generation=++state.generation;
  state.controller=controller;state.sid=sid;state.busy=true;state.rows=[];state.visible=[];state.index=0;state.through=0;state.page=0;state.pinned=false;mapInitial=false;
  $('cancel').hidden=false;$('latest').disabled=true;$('download').hidden=true;$('downloadRaw').hidden=true;$('title').textContent=`세션 #${sid} 불러오는 중`;
  $('error').textContent='';renderSessions();renderPanels();schedule(true,true,true);
  let after=0;const rows=[];
  try{
    let d;
    do{
      d=await api(`/api/records/samples?session_id=${sid}&after=${after}&through=${state.through}`,{signal:controller.signal});
      if(generation!==state.generation)return;
      rows.push(...d.samples);state.through=d.through;after=d.cursor;state.session=d.session;state.channels=d.channels;
      $('loadStatus').textContent=`전체 기록 불러오는 중 · ${rows.length.toLocaleString()}개`;
    }while(d.more);
    state.rows=rows;state.yRanges={};state.requestedX=null;prepareTime();renderCategories();renderPanels();
    $('title').textContent=`#${sid} · ${state.session.label||'이름 없는 측정'}`;
    $('sessionInfo').textContent=`시작 ${stamp(state.session.started_at_utc)} · ${state.session.stopped_at_utc?'종료 '+stamp(state.session.stopped_at_utc):'기록 중 · 최신 저장분은 다시 읽기'} · ${rows.length.toLocaleString()}표본`;
    $('loadStatus').textContent=`전체 ${rows.length.toLocaleString()}개 로드 완료`;
    $('downloadRaw').hidden=false;$('downloadRaw').href=`/api/recording/export?session_id=${sid}&through=${state.through}`;
    $('download').hidden=false;$('download').href=`/api/records/export.csv?session_id=${sid}&through=${state.through}`;
    state.index=Math.max(0,state.rows.findIndex(r=>finite(r.values.battery_power_w)));
    schedule(true,true,true);
  }catch(e){if(generation===state.generation){$('loadStatus').textContent=e.name==='AbortError'?'불러오기 취소됨':'전체 기록 불러오기 실패';if(e.name!=='AbortError')fail(e);}}
  finally{if(generation===state.generation){state.busy=false;$('cancel').hidden=true;$('latest').disabled=false;}}
}
function prepareTime(){
  state.rows.sort((a,b)=>x(a)-x(b)||a.id-b.id);
  const a=state.rows.length?x(state.rows[0]):0,b=state.rows.length?x(state.rows.at(-1)):1;
  state.domain=[a,Math.max(a+.001,b)];state.range=[...state.domain];state.index=0;state.page=0;syncRange();
}
function persist(){try{localStorage.setItem('djy-analysis-channels',JSON.stringify([...state.selected]));}catch{}}
function setChannel(key,on){if(on)state.selected.add(key);else state.selected.delete(key);persist();syncChecks();renderPanels();schedule(true,false,false);}
function renderCategories(){
  $('categories').replaceChildren();
  for(const [category,label] of Object.entries(CATEGORIES)){
    const channels=displayChannels().filter(c=>c.category===category);
    if(!channels.length)continue;
    const detail=document.createElement('details');detail.className='category';detail.open=channels.some(c=>state.selected.has(c.key));
    const summary=document.createElement('summary'),all=document.createElement('input');all.type='checkbox';all.dataset.category=category;all.setAttribute('aria-label',label+' 전체 선택');
    all.onclick=e=>e.stopPropagation();all.onchange=()=>{for(const c of channels)all.checked?state.selected.add(c.key):state.selected.delete(c.key);persist();syncChecks();renderPanels();schedule(true);};
    const title=document.createElement('b');title.textContent=label;const count=document.createElement('span');count.dataset.count=category;
    summary.append(all,title,count);detail.append(summary);
    for(const c of channels){const row=document.createElement('label');row.className='channel-option';const input=document.createElement('input');input.type='checkbox';input.dataset.key=c.key;input.onchange=()=>setChannel(c.key,input.checked);
      const text=document.createElement('span');text.textContent=c.label;const unit=document.createElement('small');unit.textContent=`${displayUnit(c.unit)||'무차원'} · ${c.key}`;text.append(unit);row.append(input,text);detail.append(row);}
    $('categories').append(detail);
  }
  syncChecks();
}
function syncChecks(){
  document.querySelectorAll('input[data-key]').forEach(e=>e.checked=state.selected.has(e.dataset.key));
  document.querySelectorAll('input[data-category]').forEach(e=>{const keys=displayChannels().filter(c=>c.category===e.dataset.category),n=keys.filter(c=>state.selected.has(c.key)).length;e.checked=n===keys.length;e.indeterminate=n>0&&n<keys.length;document.querySelector(`[data-count="${e.dataset.category}"]`).textContent=`${n}/${keys.length}`;});
}
function renderPanels(){
  state.plots=[];$('charts').replaceChildren();
  for(const [category,title] of Object.entries(CATEGORIES)){
    const channels=chosen().filter(c=>c.category===category);if(!channels.length)continue;
    const block=document.createElement('section');block.className='chart-category';const h=document.createElement('h2');h.className='category-heading';h.textContent=title;block.append(h);
    const grid=document.createElement('div');grid.className='unit-plots';block.append(grid);
    const units=[...new Set(channels.map(c=>c.unit))].sort((a,b)=>(a==='W'?-1:b==='W'?1:0));
    for(const unit of units){
      const list=channels.filter(c=>c.unit===unit),id=category+':'+unit;
      const panel=document.createElement('section');panel.className='plot-panel'+(units.length===1||(category==='battery'&&unit==='W')?' wide':'');panel.dataset.plot=id;
      const head=document.createElement('div');head.className='plot-head';const h3=document.createElement('h3');h3.textContent=`Y축 · ${displayUnit(unit)||'무차원'} / X축 · 초 (s)`;head.append(h3);
      const controls=document.createElement('div');controls.className='y-controls';
      const low=document.createElement('input'),high=document.createElement('input');for(const e of [low,high]){e.type='number';e.step='any';}low.placeholder='Y 최소';high.placeholder='Y 최대';low.setAttribute('aria-label',id+' Y 최소');high.setAttribute('aria-label',id+' Y 최대');
      if(state.yRanges[id]){low.value=state.yRanges[id][0];high.value=state.yRanges[id][1];}
      const apply=document.createElement('button');apply.textContent='Y 적용';apply.onclick=()=>{const a=Number(low.value),b=Number(high.value);if(!low.value||!high.value||!finite(a)||!finite(b)||a>=b){fail(Error('Y축 최소·최대값을 확인하세요. 최소가 최대보다 작아야 합니다.'));return;}state.yRanges[id]=[a,b];$('error').textContent='';schedule(true);};
      const auto=document.createElement('button');auto.textContent='Y 자동';auto.onclick=()=>{delete state.yRanges[id];low.value=high.value='';schedule(true);};controls.append(low,high,apply,auto);head.append(controls);panel.append(head);
      const legend=document.createElement('div');legend.className='legend';for(const c of list){const label=document.createElement('label');label.style.color=color(c.key);const check=document.createElement('input');check.type='checkbox';check.checked=true;check.dataset.key=c.key;check.onchange=()=>setChannel(c.key,check.checked);label.append(check,document.createTextNode(c.label));legend.append(label);}panel.append(legend);
      const stage=document.createElement('div');stage.className='plot-stage';const base=document.createElement('canvas'),overlay=document.createElement('canvas');overlay.className='plot-overlay';overlay.tabIndex=0;overlay.setAttribute('aria-label',`${title} ${displayUnit(unit)} 그래프 · 드래그 확대, 방향키 표본 이동`);stage.append(base,overlay);panel.append(stage);
      const readout=document.createElement('div');readout.className='plot-cursor';panel.append(readout);grid.append(panel);
      const plot={id,unit,channels:list,base,overlay,stage,readout,geometry:null};state.plots.push(plot);wirePlot(plot);
    }
    $('charts').append(block);
  }
  if(!state.plots.length){const empty=document.createElement('div');empty.className='empty-chart';empty.textContent=state.busy?'세션을 불러오는 중입니다.':'체크박스로 표시할 신호를 선택하세요. 저장값은 그대로 유지됩니다.';$('charts').append(empty);}
}
function lower(value){let lo=0,hi=state.rows.length;while(lo<hi){const mid=(lo+hi)>>1;if(x(state.rows[mid])<value)lo=mid+1;else hi=mid;}return lo;}
function upper(value){let lo=0,hi=state.rows.length;while(lo<hi){const mid=(lo+hi)>>1;if(x(state.rows[mid])<=value)lo=mid+1;else hi=mid;}return lo;}
function nearest(value){const right=lower(value);if(right===0)return 0;if(right===state.rows.length)return right-1;return value-x(state.rows[right-1])<x(state.rows[right])-value?right-1:right;}
function choose(index,pin=false,requested=null){if(!state.rows.length)return;state.index=Math.max(0,Math.min(state.rows.length-1,index));if(pin)state.pinned=true;state.requestedX=requested;schedule();}
function syncRange(){$('xMin').value=state.range[0].toFixed(3);$('xMax').value=state.range[1].toFixed(3);}
function setRange(a,b){
  if(!finite(a)||!finite(b)||a>=b)return;
  const [lo,hi]=state.domain,span=Math.min(hi-lo,Math.max(.001,b-a));a=Math.max(lo,Math.min(a,hi-span));state.range=[a,a+span];state.page=0;syncRange();schedule(true,true,true);
}
function zoom(factor,anchor=(state.range[0]+state.range[1])/2){const [a,b]=state.range;setRange(anchor+(a-anchor)*factor,anchor+(b-anchor)*factor);}
function moveRange(fraction){const [a,b]=state.range,d=(b-a)*fraction;setRange(a+d,b+d);}
function eventX(plot,event){const rect=plot.overlay.getBoundingClientRect(),g=plot.geometry;if(!g)return state.range[0];const px=Math.max(g.left,Math.min(g.right,event.clientX-rect.left));return state.range[0]+(px-g.left)/(g.right-g.left)*(state.range[1]-state.range[0]);}
function wirePlot(plot){
  const canvas=plot.overlay;
  canvas.onpointerdown=e=>{if(!state.rows.length||e.button!==0)return;canvas.setPointerCapture(e.pointerId);drag={plot,start:eventX(plot,e),px:e.clientX,last:eventX(plot,e),range:[...state.range],pan:e.shiftKey||$('gesture').value==='pan'};};
  canvas.onpointermove=e=>{const v=eventX(plot,e);if(drag&&drag.plot===plot){drag.last=v;if(drag.pan){const rect=canvas.getBoundingClientRect(),g=plot.geometry,delta=(e.clientX-drag.px)/(g.right-g.left)*(drag.range[1]-drag.range[0]);setRange(drag.range[0]-delta,drag.range[1]-delta);}else schedule();}else if(!state.pinned)choose(nearest(v),false,v);};
  canvas.onpointerup=e=>{if(!drag||drag.plot!==plot)return;const end=eventX(plot,e),previous=drag;drag=null;if(Math.abs(e.clientX-previous.px)<5)choose(nearest(end),true,end);else if(!previous.pan)setRange(Math.min(previous.start,end),Math.max(previous.start,end));schedule();};
  canvas.onpointercancel=()=>{drag=null;schedule();};canvas.ondblclick=()=>setRange(...state.domain);
  canvas.addEventListener('wheel',e=>{e.preventDefault();zoom(e.deltaY>0?1.25:.8,eventX(plot,e));},{passive:false});
  canvas.onkeydown=e=>{if(e.key==='ArrowLeft'||e.key==='ArrowRight'){e.preventDefault();choose(state.index+(e.key==='ArrowLeft'?-1:1),true);}if(e.key==='Escape'){state.pinned=false;schedule();}};
}
function scaleCanvas(canvas){const w=canvas.clientWidth,h=canvas.clientHeight,dpr=devicePixelRatio||1;canvas.width=Math.max(1,Math.round(w*dpr));canvas.height=Math.max(1,Math.round(h*dpr));const ctx=canvas.getContext('2d');ctx.setTransform(dpr,0,0,dpr,0,0);return {ctx,w,h};}
function tick(v){const a=Math.abs(v);return v.toFixed(a>=1000?0:a>=10?1:3);}
function drawBase(plot){
  const {ctx:c,w,h}=scaleCanvas(plot.base);scaleCanvas(plot.overlay);const left=65,right=Math.max(85,w-14),top=18,bottom=h-36;
  let low=Infinity,high=-Infinity;for(const row of state.visible)for(const ch of plot.channels){const v=displayValue(row.values[ch.key],ch.unit);if(finite(v)){low=Math.min(low,v);high=Math.max(high,v);}}
  const hasValues=finite(low);if(!hasValues){low=0;high=1;}if(low===high){const pad=Math.max(Math.abs(low)*.03,.1);low-=pad;high+=pad;}
  if(plot.unit==='W'){low=Math.min(low,0);high=Math.max(high,0);}const pad=(high-low)*.04;
  [low,high]=state.yRanges[plot.id]||[low-pad,high+pad];plot.geometry={left,right,top,bottom,low,high,w,h};
  c.font='11px Consolas, monospace';c.lineWidth=1;
  for(let i=0;i<=4;i++){const py=top+(bottom-top)*i/4;c.strokeStyle='#293744';c.beginPath();c.moveTo(left,py);c.lineTo(right,py);c.stroke();c.fillStyle='#b7c5d3';c.textAlign='right';c.fillText(tick(high-(high-low)*i/4),left-7,py+4);}
  for(let i=0;i<=4;i++){const px=left+(right-left)*i/4,value=state.range[0]+(state.range[1]-state.range[0])*i/4;c.strokeStyle='#23303a';c.beginPath();c.moveTo(px,top);c.lineTo(px,bottom);c.stroke();c.fillStyle='#b7c5d3';c.textAlign=i===0?'left':i===4?'right':'center';c.fillText(value.toFixed(3),px,bottom+17);}
  c.textAlign='left';c.fillStyle='#b7c5d3';c.fillText(displayUnit(plot.unit)||'무차원',4,12);c.textAlign='right';c.fillText('T+ (s)',right,h-2);
  c.save();c.beginPath();c.rect(left,top,right-left,bottom-top);c.clip();
  for(const ch of plot.channels){
    c.strokeStyle=color(ch.key);c.lineWidth=1.4;c.beginPath();let open=false,previous=null;
    // Pixel buckets retain first/min/max/last so narrow power peaks remain visible.
    let bucket=[],pixel=null;
    const flush=()=>{if(!bucket.length)return;let min=bucket[0],max=bucket[0];for(const p of bucket){if(p.v<min.v)min=p;if(p.v>max.v)max=p;}const points=[...new Set([bucket[0],min,max,bucket.at(-1)])].sort((a,b)=>a.order-b.order);for(const p of points){const py=bottom-(p.v-low)/(high-low)*(bottom-top);if(open)c.lineTo(p.px,py);else c.moveTo(p.px,py);if(bucket.length===1)c.fillRect(p.px-.8,py-.8,1.6,1.6);open=true;}bucket=[];};
    c.fillStyle=color(ch.key);
    for(let i=0;i<state.visible.length;i++){const row=state.visible[i],v=displayValue(row.values[ch.key],ch.unit),t=x(row);if(!finite(v)){flush();open=false;previous=null;pixel=null;continue;}if(previous!==null&&t-previous>3){flush();open=false;pixel=null;}previous=t;const px=left+(t-state.range[0])/(state.range[1]-state.range[0])*(right-left),p=Math.floor(px);if(p!==pixel){flush();pixel=p;}bucket.push({px,v,order:i});}flush();c.stroke();
  }
  c.restore();if(!hasValues){c.fillStyle='#ffe178';c.textAlign='center';c.fillText('선택 구간에 유효한 저장값 없음',w/2,h/2);}
}
function drawOverlay(plot){
  const {ctx:c}=scaleCanvas(plot.overlay),g=plot.geometry;if(!g)return;const row=state.rows[state.index];
  if(row){const t=x(row),px=g.left+(t-state.range[0])/(state.range[1]-state.range[0])*(g.right-g.left);if(t>=state.range[0]&&t<=state.range[1]){c.strokeStyle='#fff';c.setLineDash([4,4]);c.beginPath();c.moveTo(px,g.top);c.lineTo(px,g.bottom);c.stroke();c.setLineDash([]);for(const ch of plot.channels){const v=displayValue(row.values[ch.key],ch.unit);if(finite(v)&&v>=g.low&&v<=g.high){const y=g.bottom-(v-g.low)/(g.high-g.low)*(g.bottom-g.top);c.fillStyle=color(ch.key);c.beginPath();c.arc(px,y,3,0,Math.PI*2);c.fill();}}}
    plot.readout.textContent=`X ${fmt(t,3)} s · `+plot.channels.map(ch=>`${ch.label}: ${formatValue(row.values[ch.key],ch.unit)} ${displayUnit(ch.unit)}`).join(' / ');
  }else plot.readout.textContent='표본 없음';
  if(drag&&drag.plot===plot&&!drag.pan){const project=v=>g.left+(v-state.range[0])/(state.range[1]-state.range[0])*(g.right-g.left);c.fillStyle='#00d7ff35';c.fillRect(project(drag.start),g.top,project(drag.last)-project(drag.start),g.bottom-g.top);}
}
function renderCursor(){
  const row=state.rows[state.index];$('pin').classList.toggle('active',state.pinned);$('pin').textContent=state.pinned?'고정 해제':'커서 고정';
  $('scrub').max=Math.max(0,state.rows.length-1);$('scrub').value=state.index;
  $('cursorTime').textContent=row?`X = T+ ${fmt(x(row),3)} s · 표본 #${row.id}${state.pinned?' · 고정':''}${finite(state.requestedX)?' · 포인터 X '+fmt(state.requestedX,3)+' s':''}`:'표본 선택 대기';
  for(const [id,key,unit] of [['pointW','battery_power_w','W'],['pointV','battery_pack_voltage_v','V'],['pointA','battery_current_a','A'],['pointCommand','power_command_total_w','W']])$(id).textContent=`${formatValue(row?.values[key],unit,2)} ${displayUnit(unit)}`;
  const mode={DISCHARGING:'방전',CHARGING:'충전',STANDBY:'대기'}[row?.status.bms_state]||'상태 미수신';
  $('powerDirection').textContent=`전력 크기 ${powerText(finite(row?.values.battery_power_w)?Math.abs(row.values.battery_power_w):null)} · BMS ${mode} · 위 값은 원본 부호 유지`;
  $('pointTime').textContent=row?`표본 시각 ${stamp(row.captured_at_utc)} · 수신 ${stamp(row.received_at_utc||row.captured_at_utc)} · 보드 시각 ${row.source_timestamp_ms??'—'} ms`:'';
  $('detail').textContent=row?JSON.stringify(row,null,2):'표본 선택 대기';
  $('values').replaceChildren();
  for(const [category,title] of Object.entries(CATEGORIES)){const list=chosen().filter(c=>c.category===category);if(!list.length)continue;const group=document.createElement('div');group.className='value-group';const head=document.createElement('h3');head.textContent=title;group.append(head);for(const ch of list){const item=document.createElement('div');item.className='value-row';const label=document.createElement('span');label.textContent=ch.label;const value=document.createElement('b');value.textContent=`${formatValue(row?.values[ch.key],ch.unit,ch.key.startsWith('gnss_')?7:4)} ${displayUnit(ch.unit)}`;item.append(label,value);group.append(item);}$('values').append(group);}
  document.querySelectorAll('#rows tr').forEach(tr=>tr.classList.toggle('selected',Number(tr.dataset.id)===row?.id));showMapPoint(row);
}
function renderTable(){
  const pages=Math.max(1,Math.ceil(state.visible.length/200));state.page=Math.min(state.page,pages-1);const rows=state.visible.slice(state.page*200,(state.page+1)*200);$('rows').replaceChildren();
  for(const row of rows){const tr=document.createElement('tr');tr.dataset.id=row.id;for(const value of [fmt(x(row),3),...['battery_power_w','battery_pack_voltage_v','battery_current_a','desired_yaw_rad_s','yaw_rate_rad_s','speed_kmh'].map(k=>formatValue(row.values[k],k==='battery_power_w'?'W':'',4))]){const td=document.createElement('td');td.textContent=value;tr.append(td);}tr.onclick=()=>choose(state.rows.indexOf(row),true);$('rows').append(tr);}
  $('pageInfo').textContent=`${state.page+1}/${pages}페이지 · 구간 ${state.visible.length.toLocaleString()}개 / 전체 ${state.rows.length.toLocaleString()}개`;$('prev').disabled=state.page===0;$('next').disabled=state.page>=pages-1;
}
function schedule(base=false,table=false,route=false){needBase||=base;needTable||=table;needMap||=route;if(frame)return;frame=requestAnimationFrame(()=>{frame=0;state.visible=state.rows.slice(lower(state.range[0]),upper(state.range[1]));if(needBase)state.plots.forEach(drawBase);if(needTable)renderTable();if(needMap)renderRoute();needBase=needTable=needMap=false;state.plots.forEach(drawOverlay);renderCursor();$('rangeInfo').textContent=`X: ${fmt(state.range[0],3)} ~ ${fmt(state.range[1],3)} s · 폭 ${fmt(state.range[1]-state.range[0],3)} s · ${state.visible.length.toLocaleString()}표본 · ${state.axis==='sample'?'표본 시각':'수신 시각'} 기준`;
  });}

const map=L.map('powerMap',{preferCanvas:true}).setView([35.14358,126.93324],15),routeLayer=L.layerGroup().addTo(map);
const tiles=L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'}).addTo(map);
L.control.scale({imperial:false}).addTo(map);let routeBounds=null,marker=null,accuracy=null,mapInitial=false;
tiles.on('tileerror',()=>{$('mapCredit').textContent='배경 지도 연결 실패 · 저장 좌표와 전력 분석은 계속 사용할 수 있습니다.';});tiles.on('load',()=>{$('mapCredit').textContent='OpenStreetMap · 지도 배경 연결됨';});
const gps=row=>row&&finite(row.values.gnss_latitude_deg)&&finite(row.values.gnss_longitude_deg)?[row.values.gnss_latitude_deg,row.values.gnss_longitude_deg]:null;
function powerColor(v,lo,hi){if(!finite(v))return '#89909b';const r=Math.max(0,Math.min(1,(Math.abs(v)-lo)/Math.max(1,hi-lo))),a=r<.5?[0,201,255]:[255,177,48],b=r<.5?[255,177,48]:[255,60,70],t=r<.5?r*2:(r-.5)*2;return `rgb(${a.map((v,i)=>Math.round(v+(b[i]-v)*t)).join(',')})`;}
function renderRoute(){
  routeLayer.clearLayers();let previous=null,lo=Infinity,hi=-Infinity;const points=[];for(const row of state.visible){const p=row.values.battery_power_w;if(finite(p)){lo=Math.min(lo,Math.abs(p));hi=Math.max(hi,Math.abs(p));}}
  $('mapMin').textContent=`|${state.powerUnit}| ${formatValue(lo,'W')}`;$('mapMax').textContent=`|${state.powerUnit}| ${formatValue(hi,'W')}`;
  for(const row of state.visible){const position=gps(row);if(!position){previous=null;continue;}points.push(position);if(previous&&x(row)-x(previous.row)<=3){if(position[0]!==previous.position[0]||position[1]!==previous.position[1]){const line=L.polyline([previous.position,position],{color:powerColor(row.values.battery_power_w,lo,hi),weight:5,opacity:.85}).addTo(routeLayer);line.on('click',()=>choose(state.rows.indexOf(row),true));}}
    else{const dot=L.circleMarker(position,{radius:4,color:powerColor(row.values.battery_power_w,lo,hi)}).addTo(routeLayer);dot.on('click',()=>choose(state.rows.indexOf(row),true));}previous={row,position};}
  routeBounds=points.length?L.latLngBounds(points):null;if(routeBounds&&!mapInitial){map.fitBounds(routeBounds,{padding:[20,20],maxZoom:18});mapInitial=true;}if(!points.length)$('mapInfo').textContent='선택 구간에 유효한 GPS가 없습니다. 시간별 전력은 그래프에서 확인하세요.';
}
function showMapPoint(row){
  const position=gps(row);if(!position){if(marker)map.removeLayer(marker);if(accuracy)map.removeLayer(accuracy);marker=accuracy=null;$('mapInfo').textContent='선택 표본에 유효한 GPS 없음 · 배터리 전력을 임의의 위치와 연결하지 않습니다.';return;}
  if(!marker){marker=L.circleMarker(position,{radius:7,color:'#fff',weight:2,fillColor:'#00d9ff',fillOpacity:1}).addTo(map);accuracy=L.circle(position,{radius:row.values.gnss_accuracy_m||0,color:'#0ff',weight:1,fillOpacity:.07}).addTo(map);}else{marker.setLatLng(position);accuracy.setLatLng(position).setRadius(row.values.gnss_accuracy_m||0);}
  $('mapInfo').textContent=`선택 지점 ${position.map(v=>v.toFixed(7)).join(', ')} · 배터리 ${powerText(row.values.battery_power_w)} · GPS 정확도 ${fmt(row.values.gnss_accuracy_m,2)} m · GPS 나이 ${fmt(row.values.gnss_age_ms,0)} ms${finite(row.gnss_source_timestamp_s)?' · GPS 측정 '+stamp(row.gnss_source_timestamp_s*1000):''}`;
  if($('followPoint').checked&&!map.getBounds().contains(position))map.panTo(position,{animate:false});
}
async function poll(){try{const s=await api('/api/records/status');RecordingClock.update(s);state.active=s.recording_active;$('record').disabled=!s.can_record;$('record').textContent=state.active?'측정 종료':'측정 시작';$('label').disabled=state.active;const waiting=state.active&&(!s.recording_last_received_at_utc||s.now_ms-Date.parse(s.recording_last_received_at_utc)>3000);$('recordStatus').textContent=s.recording_error||`${state.active?'● 저장 중':s.recording_started_at_utc?'기록 종료':'기록 대기'} · ${(s.recording_sample_count||0).toLocaleString()}개${waiting?' · 수신 대기':''}`;}catch{$('record').disabled=true;$('recordStatus').textContent='게이트웨이 연결 끊김';}finally{setTimeout(poll,1000);}}
$('record').onclick=async()=>{const action=state.active?'stop':'start';$('record').disabled=true;try{const s=await api('/api/records/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,label:$('label').value})});RecordingClock.update(s);state.active=s.recording_active;await sessions();if(s.recording_session_id)await select(s.recording_session_id);else if(state.sid)await select(state.sid);}catch(e){fail(e);}};
$('search').oninput=renderSessions;$('reload').onclick=()=>sessions().catch(fail);$('latest').onclick=()=>select(state.sid);$('cancel').onclick=()=>state.controller?.abort();
$('applyX').onclick=()=>{const a=Number($('xMin').value),b=Number($('xMax').value);if(!$('xMin').value||!$('xMax').value||!finite(a)||!finite(b)||a>=b){fail(Error('X축 시작·끝을 확인하세요. 시작이 끝보다 작아야 합니다.'));return;}$('error').textContent='';setRange(a,b);};
$('zoomIn').onclick=()=>zoom(.5);$('zoomOut').onclick=()=>zoom(2);$('panLeft').onclick=()=>moveRange(-.5);$('panRight').onclick=()=>moveRange(.5);$('resetZoom').onclick=()=>setRange(...state.domain);
$('timeAxis').onchange=()=>{const id=state.rows[state.index]?.id;state.axis=$('timeAxis').value;prepareTime();state.index=Math.max(0,state.rows.findIndex(r=>r.id===id));state.requestedX=null;schedule(true,true,true);};
$('pin').onclick=()=>{state.pinned=!state.pinned;schedule();};$('previousSample').onclick=()=>choose(state.index-1,true);$('nextSample').onclick=()=>choose(state.index+1,true);$('scrub').oninput=()=>choose(Number($('scrub').value),true);
$('peak').onclick=()=>{let best=null;for(const row of state.visible)if(finite(row.values.battery_power_w)&&(!best||Math.abs(row.values.battery_power_w)>Math.abs(best.values.battery_power_w)))best=row;if(best){choose(state.rows.indexOf(best),true);$('error').textContent='';}else fail(Error('이 구간에 유효한 배터리 전력이 없습니다. BMS 미수신을 전력 0으로 계산하지 않습니다.'));};
$('prev').onclick=()=>{state.page--;schedule(false,true);};$('next').onclick=()=>{state.page++;schedule(false,true);};
function preset(keys){state.selected=new Set(keys);persist();renderCategories();renderPanels();schedule(true);}
$('batteryPreset').onclick=()=>preset(DEFAULT.concat(['battery_soc_pct','bms_temp_max_c']));$('tvPreset').onclick=()=>preset(['desired_yaw_rad_s','yaw_rate_rad_s','yaw_error_rad_s','delta_power_w','power_left_w','power_right_w','battery_power_w','sas_deg','tps_pct','rpm_left','rpm_right']);$('hideAll').onclick=()=>preset([]);
$('fitMap').onclick=()=>{if(routeBounds)map.fitBounds(routeBounds,{padding:[20,20],maxZoom:18});};window.addEventListener('resize',()=>{map.invalidateSize();schedule(true);});
$('powerUnit').onchange=()=>setPowerUnit($('powerUnit').value);syncPowerUnit();
sessions().then(()=>{if(state.sessions.length)select(state.sessions[0].id);else{renderPanels();schedule(true,true,true);}}).catch(fail);poll();setInterval(()=>sessions().catch(fail),10000);
