/* Server elapsed time + monotonic browser interpolation; browser clock skew is irrelevant. */
window.RecordingClock = {
  state: {}, anchor: performance.now(),
  update(data) { this.state = data; this.anchor = performance.now(); this.draw(); },
  format(seconds) {
    const n = Math.max(0, Math.floor(seconds || 0));
    return [Math.floor(n/3600), Math.floor(n/60)%60, n%60].map(v=>String(v).padStart(2,'0')).join(':');
  },
  draw() {
    const s = this.state, el = document.getElementById('recordClock');
    if (!el) return;
    const stale = performance.now()-this.anchor > 5000;
    const elapsed = (s.recording_elapsed_s||0) + (s.recording_active ? Math.min(5000,performance.now()-this.anchor)/1000 : 0);
    el.textContent = `${this.format(elapsed)}${stale?' · 연결 확인 필요':''}`;
    const start = document.getElementById('recordStart');
    if(start) start.textContent = s.recording_started_at_utc ? `저장 시작 ${new Date(s.recording_started_at_utc).toLocaleString('ko-KR')}` : '저장 시작 대기';
  }
};
setInterval(()=>RecordingClock.draw(),100);
