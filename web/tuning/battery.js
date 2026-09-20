'use strict';
window.batteryDashboard = {
  groups: {power:'01 · 배터리 전력 / 좌우 출력 요구',voltage:'02 · 팩 전압',current:'03 · 팩 전류',delta:'04 · 토크벡터링 ΔP',yaw:'05 · 목표 / 실제 yaw',throttle:'06 · 스로틀',speed:'07 · 차속',cells:'08 · 최고 / 최저 셀 전압',imbalance:'09 · 셀 전압 편차',temperature:'10 · 온도',soc:'11 · 잔량'},
  render(values,status,view,selected,showingCurrent) {
    const unit=(v,u,d=1)=>finite(v)?format(v,d)+' '+u:'미수신';
    const on=v=>v===true?'ON':v===false?'OFF':'미수신';
    $('bmsContext').textContent=showingCurrent?'현재 수신값':`선택 시점 · ${selected?time(selected.time_ms):'기록 없음'}`;
    $('packWatts').textContent=unit(values.battery_power_w,'W',0);
    $('packKw').textContent=unit(values.battery_power_kw,'kW',3);
    $('packVoltage').textContent=format(values.battery_pack_voltage_v,1);
    $('packCurrent').textContent=format(values.battery_current_a,1);
    $('packSoc').textContent=format(values.battery_soc_pct,1);
    $('commandTotal').textContent=unit(values.power_command_total_kw,'kW',3);
    $('commandSides').textContent=`좌 ${unit(values.power_left_kw,'kW',3)} / 우 ${unit(values.power_right_kw,'kW',3)} · 요구값`;
    $('batterySpeed').textContent=unit(values.speed_kmh,'km/h',2);
    let hasChange=false;
    for(const [key,id,u,d] of [['battery_pack_voltage_v','voltageChange','V',1],['battery_current_a','currentChange','A',1],['battery_power_w','powerChange','W',0]]){
      const first=view.rows.find(r=>r.id&&finite(r.values[key])&&(!selected||r.time_ms<=selected.time_ms));
      const valid=finite(values[key])&&first;
      const change=valid?values[key]-first.values[key]:null;
      $(id).textContent=valid?`${change>0?'+':''}${format(change,d)} ${u}`:'—';
      hasChange=hasChange||Boolean(valid);
    }
    $('changeContext').textContent=hasChange?'표시 구간 기준 · 포인터로 비교 시점 선택':'구간 내 유효값 대기';
    $('bmsState').textContent=status.bms_online?(status.bms_state||'수신 중'):'BMS 미수신';
    $('bmsState').classList.toggle('alarm',status.bms_fault===true);
    $('mos').textContent=`충전 ${on(status.bms_charge_mos_on)} / 방전 ${on(status.bms_discharge_mos_on)}`;
    $('load').textContent=`충전기 ${on(status.bms_charger_present)} / 부하 ${on(status.bms_load_present)}`;
    $('temps').textContent=`${unit(values.bms_temp_min_c,'°C')} / ${unit(values.bms_temp_max_c,'°C')}`;
    $('extra').textContent=`${on(status.bms_balancing)} / ${unit(status.bms_remaining_capacity_ah,'Ah',2)}`;
    $('cycles').textContent=format(status.bms_cycle_count,0);
    $('alarm').textContent=status.bms_online?(status.bms_fault===true?`경고 · ${status.bms_alarm_summary||status.bms_alarm_hex||'BMS 알람'}`:status.bms_fault===false?'보고된 알람 없음':'알람 상태 미수신'):'미수신';
    $('alarm').classList.toggle('alarm',status.bms_fault===true);
    $('bmsAge').textContent=finite(status.bms_age_ms)?`최근 BMS 프레임 ${format(status.bms_age_ms,0)} ms 전 · 전압/전류 개별 시각은 미제공`:'CAN 수신 대기';
    $('bmsSource').textContent=[status.bms_model,status.bms_protocol].filter(Boolean).join(' · ')||'BMS 수신 대기';
    const cells=status.bms_cell_voltages_v||[],validCells=cells.filter(finite);
    $('cellCount').textContent=status.bms_online?`${validCells.length}개 수신 / ${finite(status.bms_cell_count)?status.bms_cell_count:'?'}셀`:'미수신';
    $('cellSummary').textContent=`최저 ${unit(values.bms_min_cell_voltage_v,'V',3)} / 최고 ${unit(values.bms_max_cell_voltage_v,'V',3)} · 편차 ${unit(values.bms_cell_delta_mv,'mV',0)}`;
    $('cells').replaceChildren();
    const min=Math.min(...validCells),max=Math.max(...validCells);
    cells.forEach((v,i)=>{const cell=document.createElement('div');cell.className='cell';cell.classList.toggle('low',finite(v)&&v===min);cell.classList.toggle('high',finite(v)&&v===max&&min!==max);cell.append(`셀 ${i+1}`);const value=document.createElement('b');value.textContent=unit(v,'V',3);cell.append(value);$('cells').append(cell);});
  }
};
