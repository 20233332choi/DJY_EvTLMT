# STM 텔레메트리 소프트웨어 검증 — 2026-09-16

현재 소스에는 **쓰로틀 외에도 조향각 누락, 펌웨어 형식에 따른 RPM/IMU/TQV 미표시, 일부 상태값 미전달**이 있다. 아래 결과는 호스트 모의 입력 및 소스 대조이며 오늘 연결된 보드의 펌웨어나 실시간 입력을 확인한 결과는 아니다. 펌웨어·게이트웨이 동작 코드는 변경하지 않았다.

## 검증 범위와 실행

- `scripts/build_stm32_rear_uart.ps1`이 사용하는 DJY_TQV 고정 커밋 `ee9d295...` + `firmware/stm32_rear_uart/Core` 오버레이.
- STM 송신 텍스트 → ESP 실제 C++ 파서/JSON 생성부 → Python TelemetryStore → tuning.project.
- STM USB 실제 Python 파서 → 같은 저장/그래프 경로. C++ 대시보드 표시 조건도 소스 확인.
- 별도 `../TV/stm_back` 원본의 송신 형식과 비교. 원본은 확장 UART 펌웨어와 다르다.

```powershell
python -m unittest discover -s tests -v
python scripts/audit_telemetry_contract.py
```

기존 57개 테스트 모두 통과. 추가 감사 스크립트는 현재 STM 포맷의 필드 순서를 읽고 합성 센서값을 넣어 ESP 실제 파서/JSON 생성부를 g++로 실행한다. 센서 함수, MCU ABI, UART 전기 신호, 무선망 자체를 재현하지 않는다. 출력은 결함을 포함한 현재 동작의 관찰 결과이고, 완료 문구는 정상성 승인 의미가 아니다. 외부 네트워크나 보드에 쓰지 않는다.

## 결과

| 항목 | 확장 STM → ESP | STM USB / 구형 형식 | 원인 및 의미 |
|---|---|---|---|
| TPS 개도량 | 정상 범위의 확장 문장은 전달됨 | 기본 문장만 오면 값이 있어도 그래프에서 null | ESP는 rearOutputFresh 및 fault!=1, USB는 fault 필드 존재 및 fault!=1을 요구 |
| TPS 범위 | 818..3152만 정상 | USB 및 고정 STM 진단 범위는 668..3302 | raw=800, fault=0 입력이 ESP에서 오류, USB에서 정상으로 갈림 |
| 조향각 | raw/각도=0, online/ok=false 고정 출력 | raw는 읽지만 송신하지 않는 sas_ok=1을 요구 | 현재 확장 송신만으로는 양쪽 모두 유효 조향각 표시 불가 |
| 좌우 RPM | rv=1/1일 때 유효 표시 | ESP에서 rv가 없으면 미수신; USB는 카운터 변화 기반 품질 판정도 사용 | 보고 숫자와 검증된 RPM을 구분. 펄스 없음/잡음도 그래프 결측 사유 |
| 속도 | 유효한 양쪽 RPM 필요, ESP에서 RPM으로 계산 | USB는 vs 필드와 양쪽 RPM 품질 필요 | 속도 결측이 반드시 무선 패킷 손실은 아님 |
| yaw·가속도 | imu=1 및 yaw/lat/... 확장 묶음이 있으면 전달 | 구형 imu=a/b/c/d는 진단 카운터여서 측정치 없음 | 구형 문장을 IMU 실제값으로 쓸 수 없음 |
| 목표 yaw·yaw 오차·차동 전력·좌우 전력 명령·traction | vs/dy/ye/dp/pl/pr/tva/eda/tr 묶음으로 전달 | 기본 문장에는 없어 미수신 | 전력은 제어 계산/명령값이며 실측 전력 아님 |
| DAC·TV 적용률·fault | ctl 확장 묶음에서 전달 | 기본 문장에는 없음 | 필드 미송신과 물리적 출력 정상 여부는 별개 |
| 좌우 모터 전압·목표 RPM | 현재 STM 문장에 없음 | USB가 목표 RPM 선택 필드를 지원하지만 현재 STM은 안 보냄 | 이 화면 채널들은 연결만으로 채워지지 않음 |
| REGEN 요청/제한·모드·램프 | 현재 UART 송수신에 해당 값 전달 없음 | USB에도 없음 | ESP JSON의 0/1/200 등의 기본값을 실시간 STM 상태로 해석하면 안 됨 |

BMS 전압/전류/SOC 및 GNSS는 별도 입력 경로다. STM 연결만으로 이 값들이 채워질 것으로 기대하면 안 된다.

재현에서 확장 문장 `tps=2000 pct=50`, `sas=9000`, `rv=1/1`, `imu=1`은 TPS 50%, RPM 1200/1300, yaw 0.125, 목표 yaw 0.456, 전력 명령 4.2/5.1로 전달됐다. 조향각은 두 경로 모두 null이었다. 기본 문장의 같은 TPS 50%는 수신 객체에는 남지만 두 경로의 그래프에서 null이었다. 확장 문장에서 rv만 제거하면 ESP 실제 RPM 그래프는 null, 원본 보고 RPM은 보존됐다. 301ms 후 ESP의 STM online 해제도 확인했다.

## 소스 근거와 추가 상태 누락

- `firmware/stm32_rear_uart/Core/Src/main.c:728`: 실제 송신 포맷. sas_ok, 모터 전압, 목표 RPM, REGEN/모드/램프 필드 없음.
- `firmware/esp32_ev_gateway/src/main.cpp:858`: 조향각 값과 유효성을 0/false로 고정.
- 같은 파일 `:819`, `:820`, `:887`: TPS 범위 및 Front freshness 조건.
- `gateway/stm_serial.py:88`: fault 없는 문장에서는 TPS 미수신; `:95`: sas_ok=1 필요.
- `gateway/tuning.py:13`, `:14`, `:43`: 조향각/TPS의 유효성 조건으로 결측 처리.
- `src/ui/DashboardUI.cpp:297`: C++ 화면도 TPS online/ok가 모두 참이어야 개도량을 표시.
- STM `main.c:762`의 req는 ESP 명령의 strength다. 제어는 `:828`에서 명령 만료 시 Front/CAN으로 복귀하지만, 송신 req는 그 실제 입력원을 따로 반영하지 않는다. 휠 요청값과 화면 요청값이 달라질 수 있다.
- ESP driverFresh는 200ms 미만인데 STM 명령 유효 시간은 500ms다. `driver_control_fresh`를 전체 Front/휠 상태나 STM의 전체 명령 유효성과 동일시하면 안 된다.
- STM의 ipk/bad/rs 진단은 USB 파서에서 매핑하지만 ESP 파서는 해당 필드를 매핑하지 않는다. ESP의 stm_imu_diag_*는 구형 imu=a/b/c/d용이다.
- `../TV/stm_back/Core/Src/main.c:779`의 원본은 기본 RPM/TPS와 진단 카운터만 USART2로 보낸다. 해당 원본에는 PA9 USART1 텔레메트리 구현이 없다. 어느 버전이 실제 보드에 있는지는 오늘 확인하지 않았다.

## 이전 무선 검증과 이번 진단의 관계

[9월 13일 기록](ESP_UART_UDP_20260913.md)에는 STM→ESP→동일 핫스팟 PC의 실제 UDP 수신 검증이 있다. 8초간 39/39개 JSON 정상, 약 5Hz, UART 오류 0이었다. 이번에 저장된 `.local/esp-wireless-20260913-telemetry.json`도 직접 열어 WIFI_UDP, usb_link_online=false, wifi_udp_link_online=true, stm_online=true, 수신율 4.76Hz를 확인했다. 그 스냅샷의 TPS는 raw=890, 0%, online=true이고 IMU/조향각은 false였다.

따라서 **쓰로틀 미표시가 항상 발생하는 것은 아니다.** 현재 증상을 확정하려면 실제 수신 문장/JSON이 확장 형식인지, tps_raw/pct/fault 및 tps_online/tps_ok가 무엇인지 대조해야 한다. 이번 구형 문장 재현만으로 사용자의 현재 보드가 구형이라고 단정하지 않는다. ngrok 인터넷 릴레이, 장시간/주행 통신, 실제 센서 정확도는 이 과거 무선 검증의 범위 밖이다.

## 수정 시 우선순위

1. STM에서 실제 CAN 신선도와 TPS/SAS 유효성을 명시적으로 송신하고 수신기·화면의 의미를 맞춘다. 유효성 검사를 단순 삭제하지 않는다.
2. 조향각 0/false 고정을 제거할 때 실제 유효성 필드와 연결하고 TPS 진단 범위도 맞춘다.
3. 실제 제어 입력원에 따른 TV 요청값과 REGEN/모드/램프를 필요 필드로 송수신한다. 목표 RPM·실측 모터 전압은 생성원이 있어야 추가할 수 있다.

이번 변경은 이 보고서와 재현 스크립트뿐이다. 보드 플래시, 리셋, 모터/TV 명령, 게이트웨이 재시작은 수행하지 않았다.
