# EV 차량/피트 텔레메트리 구조

## 확정 목표 구조 (2026-09-07)

이 프로젝트에서 사용하는 이름은 다음과 같다.

- `STM32F`: Front STM32
- `STM32R`: Rear STM32
- `DISP`: 피트의 C++/Dear ImGui 대시보드

텔레메트리의 논리 흐름은 아래 두 경로로 고정한다.

```text
STM32F ──> STM32R ──> ESP32 ──> DISP
BMS ─────────────────> ESP32 ──> DISP
```

실제 물리 인터페이스와 속도를 포함하면 다음과 같다.

```text
STM32F
   │  차량 CAN 500 kbit/s
   v
STM32R
   │  UART 115200 8N1
   │  ESP UART1: RX GPIO18 / TX GPIO17
   v
ESP32-S3 ── Wi-Fi(local UDP 또는 HTTPS relay) ──> EV gateway ── UDP 9004 ──> DISP
   ^
   │  ESP UART2: RX / TX / DE(RE), GPIO 미정
절연형 3.3 V RS485 트랜시버
   ^
   │  RS485 9600 8N1, A/B/(BMS측 GND)
DALY R24TS BMS
```

화살표는 기본 텔레메트리 방향이다. 피트 명령의 역방향 경로는 기존 안전 게이트를
통과할 때만 별도로 허용하며, BMS에는 설정 변경이나 MOS 제어 명령을 보내지 않는다.

### 현재 상태와 목표

| 구간 | 인터페이스 | 상태 |
|---|---|---|
| STM32F → STM32R | 차량 CAN 500 kbit/s | 기존 차량 계약 유지, 실물 검증 별도 |
| STM32R → ESP32 | UART 115200 8N1 | 현재 ESP 빌드 설정과 일치 |
| BMS → ESP32 | 절연형 RS485, 기본 9600 8N1 | 목표 구성, RS485 응답 형식과 핀맵 검증 필요 |
| ESP32 → DISP | Wi-Fi → gateway → UDP 9004 | 기존 로컬/인터넷 경로 사용 |
| BMS → PC | UART-to-USB | 현재 벤치 검증용, 최종 차량 경로 아님 |

### ESP32 자원 배정과 충돌 판정

ESP32-S3에는 UART 컨트롤러가 세 개 있으므로 다음처럼 분리하면 주변장치 충돌은 없다.

| ESP 자원 | 용도 | 설정 |
|---|---|---|
| UART0 / `Serial` | USB 디버그 로그 | 115200 |
| UART1 / `rearUart` | STM32R 통신 | 115200, RX GPIO18 / TX GPIO17 |
| UART2 | BMS RS485 트랜시버 | 기본 9600, RX/TX/DE(RE) GPIO 미정 |
| TWAI(CAN) | 예비 | 이 구조에서는 사용하지 않음 |
| Wi-Fi | DISP 텔레메트리 전송 | local UDP 또는 HTTPS relay |

ESP32의 처리량은 STM32R UART, 저속 BMS 폴링, Wi-Fi 전송을 동시에 수행하기에 충분하다.
단, 아래 조건을 구현할 때만 `충돌 없음`으로 판정한다.

- GPIO17/18은 STM32R 전용으로 유지하고 BMS에 재사용하지 않는다.
- BMS용 UART2 RX/TX/DE(RE)는 보드의 부트 스트랩, USB, 상태 LED, PIT ENABLE 핀과
  겹치지 않는 GPIO로 확정한다.
- RS485 A/B는 ESP GPIO에 직접 연결하지 않고 절연형 3.3 V 트랜시버를 통한다.
- STM32R 수신을 최우선으로 non-blocking 처리하고, BMS는 1~2 Hz로 폴링한다.
- UART별 수신 버퍼와 프레임 파서를 분리하여 한 장치의 깨진 프레임이 다른 장치 상태를
  변경하지 못하게 한다.
- BMS 데이터에는 age/counter/checksum 검증을 적용하고, 시간 초과 시 마지막 값을
  유지해 정상처럼 표시하지 않고 `STALE` 또는 `OFFLINE`으로 보낸다.
- 현재 ESP JSON/relay 버퍼 크기 1536 byte는 셀 전압 배열을 추가한 뒤 다시 계산한다.
- USB-RS485 어댑터와 ESP가 같은 BMS RS485 선에서 동시에 master로 송신하지 않는다.

### 처리 우선순위

1. STM32R 차량 텔레메트리 수신과 유효성 검사
2. 로컬 freshness/fault 상태 갱신
3. BMS 읽기 전용 폴링과 응답 검사
4. DISP용 JSON 조립 및 Wi-Fi 전송

ESP32 또는 무선 링크가 고장 나면 DISP 텔레메트리는 끊길 수 있지만, STM32F와 STM32R의
차량 제어 및 안전 동작은 계속 독립적으로 유지되어야 한다. ESP32와 BMS 통신값을 토크나
회생제동의 단독 허가 조건으로 사용하지 않는다. 회생 적용은 별도의 BMS 제한값, 브레이크
입력 타당성 및 모터 컨트롤러 인터페이스 검증이 끝날 때까지 0으로 유지한다.

## 화면 분리

```text
STM32F ──CAN 500k──> STM32R ──UART──┐
BMS ──RS485/isolated transceiver────┴─> ESP32 ─┬─USB serial──────────────┐
                                               ├─local UDP 9003──────────┤
                                               └─hotspot/LTE HTTPS/ngrok─┤
                                                                         v
                                                                    EV gateway
                                                                      ├─ HTTP 8766
                                                                      └─ UDP 9004: CMake pit dashboard
```

### 스티어링 휠 HTML

운전 중 즉시 필요한 정보만 큰 글자로 표시한다.

- 현재/마지막 랩타임
- 좌·우 모터의 평균 RPM
- 배터리 SOC

평상시에는 위 세 값만 보인다. GREEN/YELLOW/RED/STOP/CHECKERED/BOX 플래그,
차량 fault 또는 피트 조정 메시지가 들어오면 전체 화면 점멸 경고가 우선 표시된다.

휴대폰, 태블릿, 라즈베리파이 키오스크 브라우저에서 같은 화면을 사용할 수 있다.
표시 장치를 변경해도 별도 C++ 재빌드가 필요 없다.

개발 단계에서는 피트/개발 PC의 게이트웨이가 HTML을 제공한다. 실차 최종 구성에서는
같은 HTML을 ESP32가 차량 내부에서 직접 제공해 스티어링 화면이 LTE 왕복 경로를
거치지 않도록 한다. 피트 전송이 끊겨도 차량 로컬 화면은 계속 동작해야 한다.

### 피트 CMake/ImGui

기존 DJY_EvTLMT 화면 골조를 유지하면서 정비·분석 정보를 표시한다.

- 스티어링 화면의 모든 값
- SAS, yaw rate, 횡가속도
- 좌·우 DAC와 출력 추정값
- TV delta power
- pulse capture 진단값
- fault code, 패킷 손실, 수신률과 지연

## 실행

표시 전용 테스트 창은 `http://127.0.0.1:8766/test`에서 연다. RPM, 배터리,
랩 시작/종료, 플래그와 조정 메시지를 시험할 수 있다. 이 API는 브라우저 표시 상태만
변경하며 STM, 인버터 또는 TV 제어값으로 전송하지 않는다. 테스트 값은 피트 C++용
UDP `9004`에도 합쳐지지 않는다. `모든 테스트 해제`를 누르면 실제 수신 데이터 표시로 돌아온다.

실제 ESP32 연결 시:

```powershell
.\run_dashboard.bat local
```

ESP32는 피트 노트북의 UDP 9003으로 EV JSON을 전송한다. 게이트웨이가 CMake 전용
UDP 9004로 전달하므로 두 프로그램이 같은 UDP 포트를 동시에 점유하지 않는다.

초기 STM 배선 시험에서 ESP32 USB COM8을 사용할 때:

```powershell
python .\gateway\ev_gateway.py --serial COM8 --baud 115200
.\run_dashboard.bat ev
```

ESP32가 출력하는 JSON 한 줄을 게이트웨이가 읽어 스티어링 HTML과 피트 CMake 양쪽에
동시에 전달한다. 다른 시리얼 모니터가 COM8을 열고 있으면 먼저 종료해야 한다.

차량 핫스팟과 인터넷을 사용하는 경우에도 게이트웨이는 수신 데이터를 동일한 UDP 9004
JSON으로 정규화한다. CMake 대시보드는 전송 경로와 무관하게 그대로 사용하며, USB가
신선하면 인터넷 릴레이보다 우선한다. 설정과 인증은
[ESP32 차량 인터넷 릴레이](INTERNET_RELAY.md)를 따른다.

게이트웨이를 사용하지 않고 CMake만 직접 시험할 때는 다음 모드를 사용한다.

```powershell
.\run_dashboard.bat ev-direct
```

이 경우 CMake가 UDP 9003을 직접 열며 스티어링 HTML은 사용할 수 없다.

## TQV/회생 상세 연동

물리 다이얼, 요청/적용값 분리, 피트 설정과 배선은
[TQV·회생제동 연동 문서](TQV_REGEN_INTEGRATION.md)를 따른다.

## 안전 경계

기본 실행은 STM32에서 화면 방향으로만 흐르는 읽기 전용이다. `--enable-control`을 명시한
경우에도 피트에서는 정차 중 상한·램프만 바꿀 수 있고 DAC나 실제 구동 명령을 직접 쓰지
않는다. ESP 물리 PIT ENABLE, TPS, RPM 조건과 Rear STM의 재검증을 모두 통과해야 한다.
