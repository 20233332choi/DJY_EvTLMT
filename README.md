# DJY EV Telemetry

2026-09-18 `tv_stm_esp.zip` 기준 Front/Rear 소스와 ESP 연동을 추가했습니다.
현재 핀맵·제어값·빌드 방법은 [tv_stm_esp 통합 안내](firmware/tv_stm_esp/README.md)를 참고하십시오.

STM32F, STM32R, ESP32-S3, DALY BMS 및 GNSS에서 들어오는 실제 차량 데이터를 표시하는
HTML 피트 대시보드입니다. 기본 화면은 `http://127.0.0.1:8766/pit`이며,
ngrok의 같은 `/pit` 주소에서 팀원이 실시간 신호와 저장된 주행을 함께 조회합니다.
기존 Windows C++ 화면은 `run_dashboard.bat native`로 별도 실행할 수 있습니다.
가상 주행 신호, 내장 데모 및 Assetto Corsa 데이터 소스는 사용하지 않습니다.

전체 통신 구조와 안전 경계는
[EV 차량/피트 텔레메트리 구조](docs/EV_ARCHITECTURE.md)를 참고하십시오.

## 기본 실행

프로젝트 루트에서 다음 파일을 실행합니다.

```text
run_dashboard.bat
```

BAT 파일은 이 하나만 사용합니다. 더블클릭하면 STM USB 직접 수신, 인터넷 읽기 전용, 로컬 Gateway,
BMS USB 벤치, ESP 직접 수신, 재빌드 및 인터넷 제어 모드를 고르는 통합 메뉴가 열립니다.
현재는 무선을 보류하고 `1. STM Rear USB direct telemetry`를 선택합니다.
Rear의 ST-Link USB를 PC에 연결하면 ESP 없이 표시/기록됩니다.
포트가 여러 개이면 `run_dashboard.bat wired COM13`처럼 실제 Rear 포트를 지정합니다.
**측정 시작**부터 원문 및 TQV 내부값을 기존 DB에 저장합니다.
[USB 기록과 유효성 표시 기준](docs/HTML_PIT_DASHBOARD.md#rear-stm-usb-직접-기록-무선-보류)을 참고하십시오.
기존 인터넷 읽기 전용 모드는 메뉴 7번 또는 `run_dashboard.bat internet`입니다.

HTML 대시보드는 Gateway의 수신 버퍼와 SQLite 기록을 조회합니다. 패킷이 없으면
임의값을 만들지 않고 각 항목에 `--` 또는 `수신 대기`를 표시합니다.

HTML은 별도 빌드가 필요 없습니다. 기존 C++ 화면을 다시 빌드하려면:

```powershell
.\run_dashboard.bat rebuild
```

Gateway 없이 ESP32의 로컬 UDP `9003`을 직접 받을 때만 다음 모드를 사용합니다.

```powershell
.\run_dashboard.bat ev-direct
```

## BMS 벤치 연결

현재 DALY R24TS를 USB-UART로 PC에 직접 연결하여 확인할 때는 다음 파일을 실행합니다.

```powershell
.\run_dashboard.bat bms COM5
```

Gateway가 BMS 데이터와 차량 텔레메트리를 병합해 UDP `9004`로 전달합니다. BMS 드라이버는
공개 텔레메트리 ID `0x90~0x98`만 요청하며 설정 변경과 MOS 제어 명령은 구현하지 않습니다.

향후 BMS를 ESP32 UART2에 연결해 무선으로 전달할 때도 `battery_pack_voltage_v`,
`battery_current_a`, `battery_soc_pct`, `bms_cell_voltages_v`, `bms_*` 필드를 그대로 사용합니다.

## HTML 그래프와 팀 공유

피트 화면은 완전 검정 배경, 밝은 원색, 굵은 그래프 선을 사용합니다.
Yaw 제어, 전력 배분, RPM, 차속, 조향, 스로틀, 트랙션 배율, TV/ED 개입, 모터 전압을
9개 패널로 나누며, 같은 목적/단위의 신호만 합칩니다. 시간 탐색과 포인터는 모든 패널에 동기화됩니다.
상단에는 TQV 내부 9개 값을 변수명으로 표시하고 PID Kp/Ki/Kd는 보드 보고값만 표시합니다.
PID 수신에는 새 Rear 펌웨어가 필요하고, ESP 경유 시 ESP도 업데이트해야 합니다.
기어비는 4:1이며 타이어 폭 42는 속도 환산에 쓰지 않습니다. 바깥지름/둘레 미확인 상태로,
STM 차속 원본(m/s)과 RPM 품질을 통과한 차속(km/h)을 구분합니다. 제어의 타이어 반경 설정은 변경하지 않았습니다.
측정 시작부터 수신한 전체 패킷은 기존 SQLite DB에 계속 저장됩니다.
과거 세션 선택, 1,000개 단위 이전/다음 페이지, 시간 탐색, 화면 정지 및 페이지 CSV 내보내기를 제공합니다.

모터 전압 입력 필드는 `motor_left_voltage_v`, `motor_right_voltage_v`입니다.
현재 차량 송신부에 이 측정값이 없으면 미수신으로 표시합니다. 배터리 팩 전압이나
DAC 스로틀 출력 전압으로 대체하지 않습니다.

공유 주소의 그래프/세션 API는 읽기 전용입니다. 측정 시작/종료와 원본 JSON은 피트 PC에서만
접근 가능합니다. 기존 TQV 설정 및 FSK-EEM 로그 분석은 C++ 화면에 보존합니다.
사용법과 수신 주기 제한은 [HTML 피트 가이드](docs/HTML_PIT_DASHBOARD.md)를 참고하십시오.

## 기존 C++ 대시보드 창 (`native`)

| 창 | 실제 입력 | 역할 |
|---|---|---|
| 시스템 상태 | Gateway/ESP/STM/BMS/GNSS 상태 | 연결 상태와 실제로 존재하는 창만 선택 |
| 차량 실시간 센서 | STM32F/STM32R | 속도, 모터 RPM, TPS, SAS, IMU, TQV/회생 상태 |
| 배터리 / DALY BMS | BMS | 팩 상태, MOS/알람, 셀별 전압과 평균 대비 편차 |
| GNSS 트랙맵 | 휴대폰 GPS 또는 외장 GNSS | 실제 위도/경도 기반 주행 궤적과 Fix 품질 |
| TQV / 회생제동 피트 설정 | 안전 게이트가 허용한 명령 경로 | 링크 시험, 제한값 및 임시 설정 |
| 실측 에너지 로그 | FSK-EEM `.log` | 체크섬 검증, 샘플과 전압·전류·전력 파형 |

TQV/회생 설정 창은 기본적으로 닫혀 있습니다. 실행 중인 Gateway가 제어를 명시적으로
허용하고 차량의 물리 안전 조건을 통과해야만 전송 버튼이 활성화됩니다.

## 측정 기록과 DB 조회

C 대시보드의 `측정 시작`을 누른 시점부터 Gateway가 전달하는 각 텔레메트리 스냅샷을
`data/ev_telemetry.db` SQLite 파일에 저장합니다. `측정 종료` 전에는 시료를 계속 추가하며,
시작 전 대기 데이터는 저장하지 않습니다. 주요 차량·IMU·BMS·GNSS 값은 개별 DB 열로
저장하고, 각 시점의 전체 패킷도 `payload_json`에 함께 보존합니다.

피트 PC에서 다음 주소를 열면 별도 프로그램 없이 기록을 확인할 수 있습니다.

```text
http://127.0.0.1:8766/records
```

세션별 시료 표에는 `vehicle_speed`, `desired_yaw`, `yaw_error`, `delta_power`,
`power_left`, `power_right`, `tv_active`, `ed_active`, `traction_scale`이 표시됩니다.
행을 누르면 그 시점에 저장된 전체 JSON을 볼 수 있습니다. 기록 조회 페이지와 API는
피트 PC의 로컬 접속만 허용하며 ngrok 전달 요청은 거부합니다.

## GNSS 입력 계약

### 휴대폰 GPS 사용

인터넷 피트 실행 후 콘솔에 표시되는 `https://<ngrok-domain>/phone`을 차량의 휴대폰에서
열고 `GPS 전송 시작`을 누릅니다. 위치 권한은 `정확한 위치`로 허용해야 합니다. 휴대폰은
핫스팟을 제공하면서 자체 GPS를 읽어 HTTPS `/api/gnss`로 보내고, Gateway는 좌표를 기존
차량 텔레메트리에 병합해 C 대시보드의 GNSS 트랙맵으로 전달합니다. HTML 페이지는 휴대폰
위치 권한과 전송 버튼만 담당하며 피트 대시보드는 계속 C/ImGui입니다.

휴대폰 정확도가 100 m보다 나쁜 샘플은 지도에 넣지 않습니다. 실차 기록 전에는 야외에서
정확도가 안정될 때까지 기다린 뒤 `주행 궤적 초기화`로 출발점을 다시 잡으십시오.

### JSON 입력

ESP32 또는 Gateway가 다음 JSON 필드를 UDP 패킷에 포함하면 트랙맵이 자동으로 실제 궤적을
누적합니다. 좌표가 없을 때는 빈 지도와 연결 대기 상태만 표시합니다.

```json
{
  "gnss_online": true,
  "gnss_fix_type": 3,
  "gnss_satellites": 12,
  "gnss_latitude_deg": 37.1234567,
  "gnss_longitude_deg": 127.1234567,
  "gnss_altitude_m": 42.5,
  "gnss_heading_deg": 181.2,
  "gnss_hdop": 0.8,
  "gnss_accuracy_m": 4.2,
  "gnss_source": "PHONE GPS",
  "gnss_age_ms": 120
}
```

위도/경도는 `double` 정밀도로 보관하고, 첫 유효 위치를 원점으로 변환해 미터 단위 궤적을
그립니다. 수신 지연이 2초를 넘으면 새 궤적을 추가하지 않습니다.

## 프로젝트 구조

최신 카카오톡 `TorqueVectoring` 소스와의 배선·송신 차이는
[최신 STM·ESP 핀맵 대조표](docs/TORQUEVECTORING_PINMAP.md)와
[편집 가능한 배선도](docs/TORQUEVECTORING_PINMAP.svg)를 참고하십시오.
이 원본은 PA9/PA10 USART1 텔레메트리가 미구현이며, 기존 UART 배선표와 구분해야 합니다.

```text
DJY_EvTLMT/
├─ CMakeLists.txt
├─ run_dashboard.bat
├─ src/
│  ├─ main.cpp
│  ├─ ui/                   # 실차 전용 ImGui 화면
│  └─ telemetry/            # EV UDP 수신, BMS/GNSS 스키마, 실측 로그
├─ gateway/                 # 유선/무선 차량 패킷 병합 및 UDP 9004 전달
├─ firmware/
│  └─ esp32_ev_gateway/     # STM32/BMS → Wi-Fi/USB Gateway
├─ scripts/
├─ docs/
└─ third_party/
```

## 안전 경계

- ESP32 또는 대시보드가 고장 나도 STM32F/STM32R의 차량 제어가 독립적으로 유지되어야 합니다.
- BMS 연동은 읽기 전용이며 대시보드에서 MOS 설정 명령을 보내지 않습니다.
- 회생 적용값은 BMS 제한, 브레이크 입력 타당성 및 모터 컨트롤러 연동 검증 전까지 0입니다.
- 피트 제어는 읽기 전용이 기본이며 PIT ENABLE, 정차, TPS/RPM 및 오류 조건을 우회하지 않습니다.

인터넷 릴레이는 [ESP32 차량 인터넷 릴레이](docs/INTERNET_RELAY.md), TQV/회생 상세는
[TQV·회생제동 STM–ESP–피트 연동](docs/TQV_REGEN_INTEGRATION.md), CAN 계측기 연동은
[CAN 연동 문서](docs/CAN_INTEGRATION.md)를 참고하십시오.
