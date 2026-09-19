# TQV·회생제동 STM–ESP–피트 연동

## 실제 데이터 흐름

```text
스티어링 휠
  ├─ TQV 다이얼 ─PC0(A5)─┐
  └─ REGEN 다이얼 ─PC1(A4)─┤
                            v
Front STM ── 0x100/0x110 ── CAN 500 kbit/s ──> Rear STM
   │                                               │
   │                                               ├─ 0x310 적용 상태
   │                                               ├─ 0x311 RPM/DAC
   │                                               └─ 0x320 피트 설정 ACK
                                                   UART │ 115200 8N1
                                                        v
                                                    ESP32-S3
                                                        │
                              Wi-Fi UDP 9003 / USB JSON │
                                                        v
                              Python gateway ── UDP 9004 ──> C++ 피트 화면
                                      └─ HTTP 8766/steering ──> 스티어링 표시 장치
```

모터 컨트롤러의 CAN은 프로토콜과 비트레이트가 확인되기 전까지 이 차량 CAN과 직접
합치지 않는다. Front/Rear STM32에는 각각 3.3 V CAN 트랜시버가 필요하며 CANH/CANL,
GND를 공통으로 연결한다. 버스 양 끝에만 120 Ω 종단저항을 둔다. 현재 ESP32는 차량
CAN에 직접 붙지 않고 Rear STM32의 USART1에 연결한다.

## 화면에서 구분하는 값

- `WHEEL DIAL REQUEST`: Front STM의 물리 다이얼 요청값(0x110)
- `REAR STM APPLIED`: Rear STM이 통신 상태, 피트 상한, 램프를 반영한 값(0x310)
- `PIT LIMITS`: 노트북에서 설정한 최대 허용값과 변화율
- `LIMIT REASON`: 요청과 적용값이 다른 이유
- `REGEN READY/APPLIED`: 컨트롤러·브레이크·BMS 검증이 끝난 실제 회생 상태

따라서 다이얼이 80%라도 피트 상한이 50%면 요청은 80%, 적용은 최대 50%로 따로
표시된다. 통신이 끊기면 Rear STM의 TQV 적용값은 램프다운되어 0%로 돌아간다.

## 물리 다이얼 배선

필터, 퀵 릴리즈 커넥터, 표시 장치 전원과 CAN 트랜시버를 포함한 1차 회로도는
`DJY_TQV/docs/STEERING_WHEEL_CIRCUIT.md`를 기준으로 한다.

10 kΩ 선형 포텐셔미터 두 개를 사용한다.

| 기능 | Front STM32F446RE | Nucleo 핀 | 배선 |
|---|---|---|---|
| TQV | PC0 / ADC1_IN10 | A5 | 양 끝 3V3·GND, 가운데 wiper PC0 |
| REGEN | PC1 / ADC1_IN11 | A4 | 양 끝 3V3·GND, 가운데 wiper PC1 |

펌웨어는 100 Hz로 읽고 1차 필터를 거친 뒤 5% 단위로 양자화한다. 0% 위치에서는 해당
기능의 요청 플래그도 꺼진다. TPS와 두 다이얼이 ADC1을 공유하므로 TPS 함수가 매번 PA0
채널을 다시 선택하도록 수정되어 있다.

## ESP32-S3 배선과 설정

현재 PlatformIO 빌드는 `EV_REAR_UART_MODE=1`이다. 핀별 그림과 전원 주의사항은
[ESP32-S3 ↔ Rear STM32 배선](ESP32_STM32_WIRING.md)을 기준으로 한다.

| Rear STM32 | ESP32-S3 | 연결 |
|---|---|---|
| PA9 / USART1_TX | GPIO18 / UART1_RX | Rear 텔레메트리 |
| PA10 / USART1_RX | GPIO17 / UART1_TX | ESP 피트 명령 |
| GND | GND | 공통 신호 기준 |
| - | GPIO4 | PIT ENABLE 스위치, 스위치 반대쪽은 GND |

`config.example.h`를 `config.h`로 복사하고 핫스팟 SSID, 비밀번호, 피트 노트북 IP를
입력한다. 비밀번호가 든 `config.h`는 Git에서 제외된다.

```powershell
cd firmware\esp32_ev_gateway
pio run
pio run -t upload --upload-port COM8
```

Rear STM32가 차량 CAN에서 모은 값과 자체 출력 상태를 USART1로 보내면 ESP 펌웨어가
이를 JSON으로 변환한다. Wi-Fi 절전은 꺼서 지연을 줄였고 같은 JSON을 USB serial
115200에도 출력한다. ESP의 TWAI/CAN 코드와 `0x121 ESP_STATUS`는 대체 CAN 모드에
남아 있지만 현재 UART 빌드에서는 사용하지 않는다.

## 실차 프로그램 실행

읽기 전용으로 먼저 확인한다.

```powershell
python .\gateway\ev_gateway.py
.\run_dashboard.bat ev
```

정차 피트 설정까지 사용할 때만 명시적으로 제어 브리지를 연다.

```powershell
python .\gateway\ev_gateway.py --enable-control
.\run_dashboard.bat ev
```

스티어링 표시 장치는 같은 핫스팟에서 `http://<피트노트북IP>:8766/steering`을 연다.
Windows 방화벽에서 TCP 8766과 UDP 9003을 허용해야 한다.

## 피트 조정의 안전 조건

피트 화면은 실제 구동 요청을 직접 덮어쓰지 않는다. 조정 가능한 것은 다음의 휘발성
상한과 램프뿐이며 전원을 다시 켜면 펌웨어 기본값으로 돌아간다.

- TQV 최대 허용률
- 회생 요청 최대 허용률
- TQV 변화율
- 회생 변화율(실제 출력 연결 전 예약값)
- TQV/회생 허용 플래그

명령은 아래 조건을 모두 만족해야 Rear STM에서 받아들인다.

1. 게이트웨이를 `--enable-control`로 실행
2. ESP32 GPIO4의 물리 PIT ENABLE 스위치를 GND로 연결
3. TPS 2% 이하
4. 좌·우 모터 RPM 각각 30 이하
5. Front/Rear CAN 상태가 최신

Rear STM이 적용한 뒤 0x320 ACK를 보내야 피트 화면이 `ACKED`로 바뀐다. ACK가 없으면
설정이 적용되었다고 판단하지 않는다.

## 회생제동의 현재 제한

회생 다이얼 요청, 피트 상한, 통신, 화면 표시는 동작하지만 `regen_applied_pct`는 현재
의도적으로 0%다. ND72680B 회생 명령 입력 방식, 이중 브레이크 센서 plausibility,
BMS 충전 허용 전류·전압·온도 제한이 실차에서 검증된 뒤에만 실제 출력 단계를 추가한다.
이 제한은 화면이나 ESP에서 우회하지 않는다.
