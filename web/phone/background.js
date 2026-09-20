'use strict';
const nativeUrl = document.getElementById('nativeUrl');
nativeUrl.value = location.origin + '/api/gnss/native';
document.getElementById('copyNative').onclick = async function () {
  try { await navigator.clipboard.writeText(nativeUrl.value); this.textContent = '복사됨 · 앱의 Server URL에 붙여넣기'; }
  catch { nativeUrl.focus(); nativeUrl.select(); this.textContent = '주소를 길게 눌러 복사하세요'; }
};
async function nativeStatus() {
  const el = document.getElementById('nativeStatus');
  try {
    const response = await fetch('/api/gnss/native/status');
    if (!response.ok) throw Error('HTTP ' + response.status);
    const s = await response.json();
    const age = s.receive_age_s == null ? '아직 없음' : s.receive_age_s.toFixed(1) + '초 전';
    const reasons = {WAITING_APP:'앱에서 서비스를 시작하세요.', READY:'위치 수신 정상',
      DELAYED_FIX:'전송 지연 · 랩타임에서 제외', CLOCK_OR_OLD_FIX:'오래된 위치 또는 휴대폰 시간 확인 필요',
      OUT_OF_ORDER:'중복 또는 순서가 뒤바뀐 위치', LOW_ACCURACY:'위치 정확도 부족',
      OTHER_SOURCE:'다른 GPS로 랩 측정 중 · 측정을 종료하고 앱 GPS로 다시 시작하세요.',
      SAMPLE_GAP:'수신 간격이 벌어져 랩 연결을 끊었습니다.'};
    el.textContent = `${s.live?'● 앱 GPS 수신 중':'○ 앱 GPS 대기'} · 마지막 전송 ${age}\n수신 ${s.received} / 채택 ${s.accepted} / 제외 ${s.rejected}\n${reasons[s.reason] || s.reason}`;
  } catch { el.textContent = '게이트웨이 연결 확인 필요 · 앱 수신 상태를 불러오지 못했습니다.'; }
  finally { setTimeout(nativeStatus, 2000); }
}
nativeStatus();
