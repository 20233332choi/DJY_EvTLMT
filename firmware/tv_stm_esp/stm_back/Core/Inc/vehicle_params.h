#ifndef VEHICLE_PARAMS_H
#define VEHICLE_PARAMS_H

/* ── 차량 물리 파라미터 (실측 후 수정) ───────────────────────── */
#define WHEELBASE           1.55f    /* 휠베이스 [m] */
#define TRACK_WIDTH         1.08f    /* 트랙폭 [m] */
#define TIRE_RADIUS         0.2286f  /* 타이어 반경 [m] */
#define GEAR_RATIO_DEFAULT  4.0f   /* 체인 감속비 */

/* ── 출력 예산 (kW 도메인) ── */
#define MOTOR_MAX_KW        7.0f     /* 모터/컨트롤러 풀스케일 캡 (★컨트롤러 재설정 전제)
                                      * 이 값 자체는 "한쪽이 몰아 받을 수 있는 상한"일 뿐이고
                                      * 합계는 P_SUM_MAX_KW가 잡는다. 7kW는 대부분의 모터에서
                                      * 연속정격이 아니라 피크값이므로 열관리 확인 필요. */
#define P_SUM_MAX_KW        9.5f     /* 양 모터 합 상한 — 불변 (10kW 대비 5% 마진)
                                      * ★첫 주행(셰이크다운)에는 5~6kW로 낮춰서 시작할 것 */
#define P_OFF_EPS_KW        0.05f

/* ── 좌우 차동(ΔP) 한계 ────────────────────────────────────────────
 * ★ΔP를 kW로만 제한하면 저속에서 위험하다. 좌우 구동력 차이는
 *   ΔF = ΔP / v 라서 v가 작을수록 폭발적으로 커진다.
 *     ΔP=5kW @ v=3m/s → ΔF=1667N  (구동륜 타이어 한계를 훨씬 초과)
 *     ΔP=5kW @ v=10m/s → ΔF=500N   (정상 범위)
 *   그래서 kW 상한과 별개로 "구동력 차 상한"을 두고 둘 중 작은 쪽을 쓴다.
 *     ΔP_max(v) = min(DELTA_POWER_MAX_KW, DELTA_FORCE_MAX_N · v / 1000)
 *
 * 첫 주행일에는 DELTA_FORCE_MAX_N=200, DELTA_POWER_MAX_KW=1.5 로 시작해서
 * 거동 확인하며 올릴 것. */
#define DELTA_POWER_MAX_KW    4.0f    /* 고속 구간 ΔP 천장 [kW] (기존 5.0 → 3.0) */
#define DELTA_FORCE_MAX_N     400.0f  /* 좌우 구동력 차 상한 [N] — 저속 보호의 핵심 */
#define DELTA_POWER_SLEW_KW_S 40.0f   /* ΔP 변화율 제한 [kW/s]. TV↔ED 전환 계단 제거 +
                                       * 센서 노이즈가 출력으로 새는 걸 막는다.
                                       * 100Hz에서 틱당 0.4kW → 3kW 스윙에 75ms */

/* ── 컨트롤러 throttle 전압 매핑 ──
 *  ★ND72680B 사양: V_CTRL_0KW → 0kW, V_CTRL_FULL → CTRL_FULL_KW (선형)
 *  V_PER_KW는 컨트롤러 풀스케일(전압/출력)로 정의 — MOTOR_MAX_KW와 분리해야 함 */
/* 전압은 mV 정수로 정의한다 — 아래 컴파일 타임 검사(#if)에서 부동소수점
 * 상수는 전처리기가 비교할 수 없기 때문. float 값은 여기서 파생시킨다. */
#define V_CTRL_0KW_MV       950      /* 0kW 시작 전압 */
#define V_CTRL_FULL_MV      3000     /* 풀스케일 전압 (3200 → 3000, 아래 ★★ 참고)
                                      * ★★ 이 값은 Fardriver 소프트웨어의 스로틀
                                      * 풀스케일 설정과 반드시 같아야 한다. 컨트롤러를
                                      * 3.00V로 안 바꾸고 여기만 바꾸면 매핑이 어긋나
                                      * 명령보다 적은 출력이 나간다(3.20V 설정에 2.286V를
                                      * 주면 4.56kW 요청이 4.16kW로 나감). */
#define V_THROTTLE_OFF_MV   900      /* 0kW 시작점보다 아래 → 확실한 off */

#define V_CTRL_0KW          (V_CTRL_0KW_MV     * 0.001f)
#define V_CTRL_FULL         (V_CTRL_FULL_MV    * 0.001f)
#define V_THROTTLE_OFF      (V_THROTTLE_OFF_MV * 0.001f)
#define CTRL_FULL_KW        7.0f     /* 그 전압에서의 출력[kW] = 컨트롤러 설정과 반드시 일치 */
#define V_PER_KW            ((V_CTRL_FULL - V_CTRL_0KW) / CTRL_FULL_KW)  /* 2.05/7 ≈ 0.293 */

/* ★외부 MCP4822로 바꾸면서 예전 STM32 내부 DAC의 출력버퍼 한계(0.2V~3.1V)
 * 제약이 사라졌다. MCP4822는 레일투레일 출력이고 VDD=5V 구동이라 4.095V까지
 * 선형으로 낸다. 이제 전 구간이 여유롭게 들어온다:
 *      off  → 0.90V → 코드 900
 *      0kW  → 0.95V → 코드 950
 *      7kW  → 3.00V → 코드 3000
 *
 * 아래 상한은 하드웨어 한계가 아니라 "안전 천장"이다. 스케일링 버그로 4V가
 * 컨트롤러 스로틀에 나가는 사고를 막기 위해 정상 최대치 바로 위에서 자른다. */
#define V_DAC_MAX_MV        (V_CTRL_FULL_MV + 200)   /* 3.20V */
#define V_DAC_MAX_V         (V_DAC_MAX_MV * 0.001f)
#if (V_DAC_MAX_MV > 4095)
#error "V_DAC_MAX_MV가 MCP4822 풀스케일(4.095V)을 초과함"
#endif

/* ── TPS 파라미터 ────────────────────────────────────────────────────
 * ★두 가지 범위를 명확히 구분한다. 예전에는 하나로 뭉쳐 있어서 문제가 됐다.
 *
 *  (1) 진단 범위 [TPS_ADC_MIN-MARGIN, TPS_ADC_MAX+MARGIN]
 *      이걸 벗어나면 단선/단락으로 보고 STOP. 넉넉하게 잡아야 한다.
 *  (2) 사용 범위 [idle+DEADBAND, FULL-MARGIN]
 *      이 구간만 0~100%로 매핑. 페달 유격과 끝단 여유를 여기서 흡수한다.
 *
 * ★실측(2026-08-06, 벤치): 페달 idle~풀프레스 전압 0.7V~2.5V
 *   0.7V → 0.7/3.3*4095 ≈ 868, 2.5V → 2.5/3.3*4095 ≈ 3102
 *
 * ★★실차 장착 후 재실측(2026-09): idle 880~890, 풀프레스 2790~2820.
 *   벤치의 3102는 센서를 손으로 끝까지 돌렸을 때 값이고, 차에 달면 페달
 *   스토퍼가 센서 끝보다 먼저 닿아서 2820에서 멈춘다. 3102를 그대로 두면
 *   hi=2922라 페달을 끝까지 밟아도 94%까지밖에 안 올라갔다.
 *   → 실측 상단(2820)을 기준으로 바꾸고 MARGIN을 줄여 100%를 보장한다. */
#define TPS_ADC_MIN         885      /* ★실차 실측 idle (880~890) */
#define TPS_ADC_MAX         2820     /* ★실차 실측 풀프레스 (2790~2820) */

/* 단선/단락 진단 마진. 마진 없이 MIN/MAX를 그대로 쓰면 idle에서 ADC가 1LSB만
 * 아래로 튀어도 즉시 STOP이 걸린다(실차에서 확실히 터질 문제).
 * ★200으로 크게 잡은 이유: 아래 idle 자동학습이 움직일 수 있는 범위
 * (±TPS_IDLE_LEARN_WINDOW)를 진단 밴드가 완전히 포함해야 하기 때문. 안 그러면
 * "학습된 idle은 정상인데 안전감시는 폴트"인 모순 상태가 생긴다.
 * 진짜 고장(단선→0 부근, 단락→4095 부근)과는 여전히 한참 떨어져 있어서
 * 진단 성능에는 영향 없다. 밴드 = [668, 3302] */
#define TPS_ADC_MARGIN      200

/* 사용 범위 기준점 — ★차에 장착한 뒤 반드시 재실측할 것 (아래 절차 참고) */
#define TPS_PEDAL_IDLE      TPS_ADC_MIN   /* 페달 완전히 뗐을 때 raw */
#define TPS_PEDAL_FULL      TPS_ADC_MAX   /* 페달 끝까지 밟았을 때 raw */

/* ★유격(dead zone) — 이 문제의 핵심.
 * 페달 링키지에는 반드시 놀음이 있고, 밟았다 뗄 때마다 복귀 위치가 수십 카운트씩
 * 달라진다(히스테리시스). 데드밴드가 없으면 "발 뗐는데 출력이 조금 나가는" 상태가
 * 되고, 심하면 idle에서 차가 스르륵 움직인다.
 * 110카운트 = 전체 스트로크의 약 4.9% = 0.089V. 양산차 APP 센서의 데드존이
 * 보통 3~8%라 이 범위 안이다. 실차에서 발 뗐을 때 tps_pct가 0이 아니면 늘릴 것. */
#define TPS_DEADBAND_RAW    110

/* ★끝단 여유 — 위쪽에서 미리 잘라 "끝까지 밟으면 확실히 100%"를 보장한다.
 * ★180 → 80으로 줄였다. TPS_ADC_MAX가 이제 벤치값(3102)이 아니라 실차 실측
 *   상단(2820)이라, 예전만큼 큰 여유가 필요 없다. 필요한 건 밟을 때마다
 *   생기는 편차(실측 2790~2820 = 30카운트)를 덮을 만큼이면 된다.
 *     hi = 2820 − 80 = 2740  →  최악(2790)에도 hi를 넘으므로 100% 보장
 *   80카운트 = 전체 스트로크(1935)의 약 4%. */
#define TPS_FULL_MARGIN_RAW 80

/* ★부팅 시 idle 위치 자동 학습 (양산차의 APP idle learn과 같은 개념).
 * 장착 후 유격이 자리를 잡거나 온도로 드리프트하면 idle raw가 벤치값에서
 * 옮겨간다. 매번 펌웨어를 다시 굽는 대신 부팅할 때 실제 정지 위치를 읽어 쓴다.
 *  - WINDOW : TPS_PEDAL_IDLE ± 이 범위를 벗어난 값은 거부(기본값 유지).
 *             페달을 밟은 채 부팅해도 학습이 거부되도록 하는 안전장치.
 *  - SPREAD : 학습 중 샘플 최대-최소가 이보다 크면 페달이 움직인 것으로 보고 거부.
 * ★실패 방향이 안전한 설계다: idle을 실제보다 높게 잡으면 출력이 덜 나갈 뿐이고,
 *   낮게 잡히는 경우(=출력이 새는 위험한 방향)는 WINDOW가 막는다. */
#define TPS_IDLE_LEARN_MS      300u
#define TPS_IDLE_LEARN_WINDOW  150
#define TPS_IDLE_LEARN_SPREAD  30

/* ── SAS 파라미터 ──────────────────────────────────────────── */
#define SAS_TO_STEERING_RATIO   -0.5f  /* 조향비 조절인듯 ★실측★ SAS raw→조향각[rad]. 부호 음수인 이유:
                                        * 센서는 우회전 시 raw 증가하지만, Ackermann 공식/
                                        * IMU(+좌회전) 관례상 delta는 좌회전이 양수여야 함 */
#define SAS_CENTER_RAW          1116   /* ★실측★ 직진(0도) 시 raw값 */
#define SAS_RAW_TO_RAD          (2.0f * 3.14159265f / 16384.0f) /* raw 1LSB당 rad(센서 1회전 기준, 실측 보정) */
#define MAX_STEERING_ANGLE_RAD  0.63f  /* 약 40도 — ★실측★ 타이어 최대 조향각.
                                        * 이 값보다 실제 조향각이 크면 그 구간이
                                        * 통째로 클램프돼서, 많이 꺾을수록 개입이
                                        * 커져야 할 때 오히려 평평해진다.
                                        * (30도로 잘려서 "급조향 시 안 느껴진다"는
                                        *  피드백이 나왔음 — 2026-09 실주행) */

/* ── 제어 파라미터 ─────────────────────────────────────────── */
#define CONTROL_FREQ_HZ     100
#define CONTROL_DT          (1.0f / CONTROL_FREQ_HZ)
#define MIN_SPEED_FOR_TV    1.0f      /* TV 활성 최소 차속 [m/s] */
#define MAX_LATERAL_ACCEL   8.0f      /* 목표 yaw 제한용 [m/s^2] */

/* ── 전자식 디퍼런셜(ED) — TV가 꺼져 있을 때만 동작하는 최소 차동 ────
 * IMU를 전혀 쓰지 않는 개루프(feedforward) 방식이다. 애커만 기구학만으로
 * 좌우 목표 속도비를 구하고, 오픈 디퍼렌셜과 동일하게 "좌우 토크 동일 →
 * 전력은 속도에 비례" 원리로 전력을 나눈다.
 *
 *   회전반경 R = L/tanδ, 좌우 궤적 반경 차 = T
 *     v_in = v(1-k), v_out = v(1+k),  k = (T/2L)·tanδ
 *     P_in/P_out = v_in/v_out  →  ΔP = P_demand · k   (제로섬)
 *
 * 특징:
 *  - IMU/요레이트/PID 불필요 → IMU 고장(SAFE_ACTION_DISABLE_TV) 시에도 살아있음
 *  - 속도항이 소거돼서 v=0에서도 성립 → MIN_SPEED_FOR_TV 아래 구간을 메워줌
 *  - ΔP가 P_demand에 비례 → 스로틀 안 밟으면 자동으로 0 (본질적으로 안전)
 *  - 폐루프가 아니므로 "차를 돌리는" 게 아니라 "안쪽 바퀴를 안 끌리게" 하는 정도
 *
 * TRACK/2L = 1.08/3.10 = 0.348 → 30도 풀스티어에서 k=0.201,
 * P_demand=9.5kW면 ΔP=1.9kW (4.75±0.95 → 3.8kW/5.7kW). 기구학적으로 타당한 값. */
#define ED_GAIN             1.0f    /* 전체 개입량 배율. 언더스티어 느낌이면 ↑, 불안하면 ↓ */
#define ED_K_MAX            0.35f   /* k 상한 (조향각 이상치 대비 안전망) */
#define ED_DELTA_MAX_KW     2.5f    /* ED가 낼 수 있는 ΔP 절대 상한 [kW] */
#define ED_LPF_FC_HZ        5.0f    /* ΔP 출력 평활 [Hz] — 조향은 저주파라 이 정도면 충분 */
/* 고속에서는 타이어 슬립각 때문에 순수 기구학보다 차동이 작아야 안정적이다.
 * FADE_START 이하는 100%, FADE_END 이상은 FADE_MIN_GAIN으로 선형 감소. */
#define ED_FADE_START_MPS   12.0f
#define ED_FADE_END_MPS     25.0f
#define ED_FADE_MIN_GAIN    0.5f

/* ── 언더스티어 그래디언트 (2자유도 자전거모델 목표요레이트 보정) ────
 * DYC 논문(KSAE 2022 555p, 식 12) 기준: γ_des = v·tanδ / (L + Kus·v²)
 * Kus=0이면 기존 순수 기구학 모델(v·tanδ/L)과 완전히 동일 — 실측 전까지 안전한
 * 기본값. 실측 방법: 정상원선회 시 여러 속도에서 (steering_angle, 실제 정상상태
 * 요레이트, v)를 기록해서 Kus = (v·tanδ/γ_steady - L) / v² 로 역산해 튜닝.
 * 단위 [s^2/m^2](=1/(m/s^2) 아님, v^2 항과 곱해지는 형태). 언더스티어 성향이면
 * 양수, 오버스티어 성향이면 음수. */
/* ★★ 미실측 — 이게 0으로 남아 있는 게 "코너를 계속 돌면 토크가 붙는다"의
 * 근본 원인이다. Kus=0이면 목표가 "언더스티어가 전혀 없는 이상적인 차"의
 * 요레이트가 되는데, 실제 차는 반드시 언더스티어하므로 정상 선회 중에도
 * 오차가 0으로 안 떨어진다 → 적분이 계속 쌓여 토크가 서서히 붙는다.
 *
 * ★로그에서 바로 계산할 수 있다. 일정한 코너를 정상상태로 돌 때(조향·속도가
 * 안정된 구간) 한 샘플만 집어서:
 *
 *     Kus = ( v·tan(δ) / ψ_실측  −  WHEELBASE ) / v²
 *
 *   v = vehicle_speed, δ = steering_angle_rad, ψ_실측 = imu_yaw_rate.
 *   여러 코너에서 뽑아 평균낼 것. FS 차량은 보통 0.002~0.010 범위.
 *   제대로 넣으면 정상 선회 시 오차가 0으로 수렴해서 적분이 안 쌓인다. */
#define UNDERSTEER_GRADIENT 0.0f

/* ── 횡가속도 기반 트랙션 안전장치 ────────────────────────────────
 * 예측 횡가속도(a_lat = v·실제요레이트)와 IMU 실측 횡가속도가 크게 다르면
 * (=타이어가 미끄러지는 중이라 자전거모델 자체가 안 맞는 상황) TV 개입을
 * 줄인다. MISMATCH_OK 이하는 정상(센서노이즈/모델오차) 범위로 무시,
 * MISMATCH_MAX 이상이면 TV 개입을 완전히 0으로 줄임. 그 사이는 선형 보간. */
#define LAT_ACC_MISMATCH_OK   2.0f    /* [m/s^2] */
#define LAT_ACC_MISMATCH_MAX  6.0f    /* [m/s^2] */
/* IMU를 뒤집어 달거나 X축을 뒤로 향하게 달면 횡가속 부호가 반대가 된다.
 * 그러면 mismatch가 항상 커져서 TV가 계속 죽는다. 실차에서 좌선회 시
 * lat 로그가 음수로 나오면 이 값을 -1.0f로. */
#define IMU_LAT_ACC_SIGN      1.0f

/* ── PID 게인 (실차 튜닝 필요, 출력 단위 = kW) ──────────────────
 * ★2026-09 실주행 피드백: "개입이 약하고 늦다. 급조향 때는 안 느껴지고
 *   코너를 계속 돌고 있으면 그제서야 토크가 붙는다."
 *   → 전형적인 "P 부족 + I 의존" 증상이다. 체감되는 토크를 느린 적분항이
 *     만들고 있어서, 즉각 반응이 없고 수 초에 걸쳐 쌓이는 느낌이 난다.
 *   기존 KI=2.0으로 오차 0.1rad/s에서 1kW를 쌓는 데 약 5초가 걸렸다.
 *
 *   KP ↑ : 조향 즉시 반응 (체감 응답의 대부분을 여기서 만든다)
 *   KI ↓ : 느리게 쌓이는 느낌 제거. ★단 UNDERSTEER_GRADIENT가 0으로 남아
 *          있는 한 정상 선회 중 오차가 0으로 안 떨어져서 적분이 계속 쌓인다 —
 *          근본 해결은 Kus 실측이다(아래 주석 참고).
 *   KD ↑ : 급조향(=목표 요레이트 계단 변화)에 즉각 반응. 노이즈에 민감하니
 *          떨림이 생기면 제일 먼저 되돌릴 것. */
#define PID_KP              20.0f    /* 10.0 → 18.0 */
#define PID_KI              1.0f     /*  2.0 →  1.0 */
#define PID_KD              0.0f     /*  0.5 →  1.5 */
#define PID_INTEGRAL_MAX    DELTA_POWER_MAX_KW
#define PID_OUTPUT_MAX      DELTA_POWER_MAX_KW

/* ── 독립 워치독 (IWDG) ──────────────────────────────────────────────
 * ★왜 필요한가: 펌웨어가 멎어도 DAC는 마지막 값을 그대로 유지한다. 그 값이
 *  높았다면 양쪽 모터가 계속 고출력으로 남는다 — 안전 문제이자, 컨트롤러
 *  MaxLineCurr를 121A(7kW)로 올린 뒤에는 총 10kW 규정 위반이 된다.
 *  IWDG가 걸리면 MCU가 리셋되고 DAC는 0으로 떨어진다.
 *
 * ★타임아웃 선정: 메인 루프에서 제일 오래 걸리는 건 USART2 디버그 출력이다
 *  (블로킹, 타임아웃 20ms × 2줄 = 최악 40ms). SD를 안 쓰므로 FatFs의
 *  f_write/f_sync는 s_open=false라 즉시 리턴해서 블로킹이 없다.
 *  → SD를 쓰게 되면 카드에 따라 수백 ms 블로킹하므로 이 값을 다시 늘릴 것.
 *
 *  LSI는 개체차가 커서(17~47kHz) 실제 타임아웃이 크게 흔들린다:
 *    타임아웃[s] = RELOAD × PRESCALER / LSI
 *      공칭(32kHz): 250 × 64 / 32000 = 0.50s
 *      최악(47kHz): 0.34s   ← 최악 루프 40ms 대비 약 8배 여유
 *      최선(17kHz): 0.94s
 *  주행 중 오작동 리셋이 행보다 위험하므로 여유를 충분히 두고 잡았다. */
#define IWDG_PRESCALER_DIV  IWDG_PRESCALER_64
#define IWDG_RELOAD_COUNT   250u

/* ── DAC 백엔드 선택 ─────────────────────────────────────────────────
 * 0 = 외부 MCP4822 (SPI3) ← 정식 구성
 * 1 = STM32 내부 DAC (PA4/PA5) ← ★임시/진단 전용
 *
 * ★내부 DAC은 PA4/PA5가 TTa(3.3V 톨러런트) 핀이라 컨트롤러 신호 환경을 못
 *  버틴다. 실제로 보드 여러 장의 DAC 채널이 이 경로로 손상됐다. 부품 도착 전
 *  임시 단일모터 시험이나 "이 보드의 어느 채널이 살아있나" 확인 용도로만 쓰고,
 *  쓸 때도 PA4/PA5에 직렬 1kΩ을 반드시 넣을 것.
 *
 * 두 백엔드는 핀이 겹치지 않아(PA4/PA5 vs PB3/PB4/PB5) 공존 가능하다.
 * 이 스위치 하나만 바꾸면 되고, 제어 로직은 어느 쪽이든 동일하게 동작한다. */
#define DAC_USE_INTERNAL    1

#define DAC_RESOLUTION      4095     /* 두 백엔드 모두 12비트 */

/* 1V당 코드 수 — 백엔드에 따라 환산이 다르다.
 *   MCP4822 : 게인2배 + 내장 2.048V → 풀스케일 4.096V → 1 LSB = 1mV
 *   내부 DAC: VREF 3.3V 풀스케일 → 1 LSB = 0.806mV */
#if DAC_USE_INTERNAL
  #define DAC_CODE_PER_V    (4095.0f / 3.3f)   /* ≈ 1240.9 */
#else
  #define DAC_CODE_PER_V    1000.0f
#endif

/* ── 안전 타임아웃 [ms] ────────────────────────────────────── */
#define CAN_TIMEOUT_MS      100
#define IMU_TIMEOUT_MS      100
#define RPM_TIMEOUT_MS      200
#define HB_TIMEOUT_MS       500       /* Heartbeat(10Hz) 유효 판정 */
#define IMU_DMA_RESTART_MS  300u      /* 이 시간 동안 자이로 패킷 없으면 UART DMA 재시작 */
#define IMU_YAW_RATE_MAX    3.0f      /* 이상치 판정 [rad/s] */

/* ── 폴트 디바운스 (100Hz 기준 샘플 수) ──────────────────────────
 * 실차 노이즈로 한 틱 튀었다고 바로 STOP/TV-off 하면 주행 중 덜컥거린다.
 * N틱 연속으로 이상해야 폴트로 확정한다(= N×10ms 지연). 반대로 정상 복귀는
 * 즉시(1틱) — 안전 방향으로만 빠르게 움직이지 않고, 폴트 진입만 늦춘다. */
#define FAULT_DEBOUNCE_TPS    3       /* 30ms */
#define FAULT_DEBOUNCE_IMU    5       /* 50ms */

/* ── 입력 필터 (filters.h) ────────────────────────────────────────
 * 실차 배선 노이즈 대응. 컷오프는 "센서의 물리적 대역폭"보다 충분히 높게 잡아
 * 제어 지연이 생기지 않도록 했다. 괄호 안은 1차 IIR 군지연 = 1/(2π·fc). */
#define TPS_LPF_FC_HZ       15.0f     /* (10.6ms) 페달 대역폭은 5Hz 이하 */
#define SAS_LPF_FC_HZ       25.0f     /* (6.4ms)  조향 대역폭도 낮지만 응답 우선 */
#define IMU_YAW_LPF_FC_HZ   25.0f     /* (6.4ms)  차체 진동 알리아싱 억제 */
#define IMU_ACC_LPF_FC_HZ   10.0f     /* (15.9ms) 트랙션 판정용이라 느려도 무방 */
#define RPM_LPF_FC_HZ       5.0f      /* (31.8ms) 차속은 느린 물리량 */

/* Deglitch 임계 = "1틱(10ms) 동안 물리적으로 가능한 최대 변화량"보다 약간 크게.
 * 이 값을 넘는 점프는 노이즈로 보고 최대 2틱까지 무시한다(정상 구간 지연 0). */
#define TPS_MAX_STEP_RAW    400.0f    /* 페달 전구간(2234)을 56ms에 밟는 속도 */
/* SAS는 raw 카운트 도메인에서 판정한다. 1LSB = SAS_RAW_TO_RAD = 3.83e-4 rad
 * (조향휠 기준)이므로 800카운트/틱 ≈ 30.6 rad/s ≈ 1750°/s — 사람이 낼 수 있는
 * 조향속도보다 충분히 위라 정상 조작은 절대 안 걸리고, 통신 비트에러로 상위
 * 바이트가 깨져 수천 카운트 튀는 경우만 잡는다. */
#define SAS_MAX_STEP_RAW    800.0f
#define IMU_YAW_MAX_STEP    0.30f     /* 요각가속도 30rad/s^2 상당 */
#define RPM_MAX_STEP        300.0f    /* 모터 30000RPM/s 상당 */
#define DEGLITCH_MAX_REJECT 2u        /* 진짜 급변은 최대 20ms 늦게 반영 */

/* 토글 스위치 디바운스 — 기계식 접점 채터링 + 배선 노이즈 대응 */
#define SWITCH_DEBOUNCE_TICKS 5u      /* 50ms 연속 같은 값이어야 반영 */

/* ── RPM 펄스 입력 (TIM3 Input Capture, 컨트롤러 SPD 핀) ──────── */
#define RPM_PULSES_PER_REV  11u       /* Fardriver "Speed Pulse" 설정값 (실제 물리적 회전당 펄스수는 아님 — 아래 참고) */
#define RPM_TICK_US         10u       /* 타이머 1카운트 = 10us (PSC=839 @ 84MHz) */
/* ★STALE 시간을 200ms에서 400ms로 늘렸다. 아래 CAL_GAIN 보정 때문에 실효
 * 펄스율이 낮아서(8000RPM에서도 펄스 주기 11.9ms), 200ms 컷이면 약 475RPM
 * (≈차속 3m/s) 아래가 전부 "정지"로 잡혀서 저속에서 TV가 아예 안 붙었다.
 * 400ms면 약 238RPM(≈1.5m/s)까지 측정 가능. */
#define RPM_STALE_MS        400u

/* ★실측 보정값: "Speed Pulse" 설정을 그대로 회전당 펄스수로 나눴더니 Fardriver
 * 앱 표시 RPM보다 항상 작게 나옴(선형 비례, 매뉴얼상 이 설정은 "표시속도용
 * 스케일" 값이지 물리적 펄스수가 아닌 듯). 두 지점 실측:
 *   우리값 91  ↔ Fardriver 1604 RPM  → 비율 17.63
 *   우리값 150.5 ↔ Fardriver 2609 RPM → 비율 17.33
 * 평균 ≈17.45로 보정. Speed Pulse 값을 바꾸면 이 상수도 재보정 필요. */
#define RPM_CAL_GAIN        17.45f

/* ★SPD 라인은 컨트롤러에서 길게 끌려오는 데다 대전류 옆을 지나서 이 시스템에서
 * 가장 노이즈가 심한 신호다. "물리적으로 불가능하게 빠른 펄스 간격"을 캡처
 * 인터럽트에서 즉시 버리는 게 가장 효과적인(그리고 지연 0인) 필터다.
 *   period_us = 60e6·GAIN / (RPM·PPR)
 * RPM_MAX_PLAUSIBLE=8000 → 약 11898us. 즉 11.9ms보다 짧은 간격의 에지는
 * 전부 노이즈로 폐기. 전기적 노이즈는 us 단위라 100% 걸러진다. */
#define RPM_MAX_PLAUSIBLE   8000.0f
#define RPM_MIN_PERIOD_US   ((uint32_t)(60000000.0f * RPM_CAL_GAIN / \
                             (RPM_MAX_PLAUSIBLE * (float)RPM_PULSES_PER_REV)))

/* ★16비트 랩어라운드 교차검증 허용오차 [ms].
 * TIM3는 16비트라 10us/tick에서 655ms를 넘는 주기는 카운터가 한 바퀴 돌아
 * 짧은 값으로 앨리어싱된다. 캡처 주기와 HAL_GetTick() 경과시간이 이 값보다
 * 크게 어긋나면 랩어라운드(또는 펄스 누락)로 보고 버린다.
 * 정상이면 둘이 1~2ms 안에서 일치하므로 5ms면 충분한 여유다. */
#define RPM_WRAP_TOL_MS     5u

#endif /* VEHICLE_PARAMS_H */
