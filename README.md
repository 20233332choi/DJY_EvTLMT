# DJY EV Telemetry

STM32F, STM32R, ESP32-S3, DALY BMS 및 GNSS에서 들어오는 실제 차량 데이터를 표시하는
Windows C++ 피트 대시보드입니다. GUI는 CMake, Dear ImGui, Win32, DirectX 11로 구성됩니다.
가상 주행 신호, 내장 데모 및 Assetto Corsa 데이터 소스는 사용하지 않습니다.

전체 통신 구조와 안전 경계는
[EV 차량/피트 텔레메트리 구조](docs/EV_ARCHITECTURE.md)를 참고하십시오.

## 기본 실행

프로젝트 루트에서 다음 파일을 실행합니다.

```text
run_dashboard.bat
```

대시보드는 기본적으로 Gateway의 실차 JSON을 UDP `9004`에서 기다립니다. 패킷이 없으면
임의값을 만들지 않고 각 항목에 `--` 또는 `수신 대기`를 표시합니다.

실행 파일이 없으면 Visual Studio CMake로 자동 빌드합니다. 강제로 다시 빌드하려면:

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
.\run_bms_pit_dashboard.bat
```

Gateway가 BMS 데이터와 차량 텔레메트리를 병합해 UDP `9004`로 전달합니다. BMS 드라이버는
공개 텔레메트리 ID `0x90~0x98`만 요청하며 설정 변경과 MOS 제어 명령은 구현하지 않습니다.

향후 BMS를 ESP32 UART2에 연결해 무선으로 전달할 때도 `battery_pack_voltage_v`,
`battery_current_a`, `battery_soc_pct`, `bms_cell_voltages_v`, `bms_*` 필드를 그대로 사용합니다.

## 대시보드 창

| 창 | 실제 입력 | 역할 |
|---|---|---|
| 시스템 상태 | Gateway/ESP/STM/BMS/GNSS 상태 | 연결 상태와 실제로 존재하는 창만 선택 |
| 차량 실시간 센서 | STM32F/STM32R | 속도, 모터 RPM, TPS, SAS, IMU, TQV/회생 상태 |
| 배터리 / DALY BMS | BMS | 팩 상태, MOS/알람, 셀별 전압과 평균 대비 편차 |
| GNSS 트랙맵 | 향후 GNSS 수신기 | 실제 위도/경도 기반 주행 궤적과 Fix 품질 |
| TQV / 회생제동 피트 설정 | 안전 게이트가 허용한 명령 경로 | 링크 시험, 제한값 및 임시 설정 |
| 실측 에너지 로그 | FSK-EEM `.log` | 체크섬 검증, 샘플과 전압·전류·전력 파형 |

TQV/회생 설정 창은 기본적으로 닫혀 있습니다. 실행 중인 Gateway가 제어를 명시적으로
허용하고 차량의 물리 안전 조건을 통과해야만 전송 버튼이 활성화됩니다.

## GNSS 입력 계약

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
  "gnss_age_ms": 120
}
```

위도/경도는 `double` 정밀도로 보관하고, 첫 유효 위치를 원점으로 변환해 미터 단위 궤적을
그립니다. 수신 지연이 2초를 넘으면 새 궤적을 추가하지 않습니다.

## 프로젝트 구조

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
