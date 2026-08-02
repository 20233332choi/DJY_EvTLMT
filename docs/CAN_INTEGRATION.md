# FSK Energy Meter CAN Integration

## 필요한 하드웨어

- CAN 컨트롤러와 ISO 11898-2 고속 CAN 트랜시버
- 차량 규정과 접지 구조에 맞는 절연
- CANH/CANL ESD·EFT·서지 보호
- 버스 양 끝의 120 Ω 종단저항
- 차량용 잠금 커넥터
- PC용 USB-CAN 인터페이스

기본 `fsk-energymeter` PCB에는 차량용 CAN 트랜시버가 없습니다. 외부 UART-CAN/SPI-CAN
보드 또는 CAN을 지원하는 중앙 로거가 필요합니다. 초기 시험은 `UART/SWD` 헤더와
절연 USB-UART로 수행할 수 있습니다.

## 권장 설정

| 항목 | 권장값 |
|---|---|
| 물리 계층 | Classical CAN 2.0B, 11-bit ID |
| 비트레이트 | 500 kbit/s |
| 측정 송신 주기 | 10 ms (100 Hz) |
| 바이트 순서 | Little-endian |
| 전압 | 0.1 V/bit |
| 전류 | signed 0.1 A/bit |
| LV 전압 | 0.01 V/bit |
| 온도 | signed 0.01 °C/bit |

차량의 기존 CAN 속도, ID 및 DBC 정책이 있으면 차량 규격을 우선합니다.

## 메시지 예시

### `0x520 EEM_MEASUREMENT` — 100 Hz, DLC 8

| Byte | 타입 | 신호 | 변환 |
|---:|---|---|---|
| 0–1 | `uint16` | HV bus voltage | raw × 0.1 V |
| 2–3 | `int16` | HV bus current | raw × 0.1 A |
| 4–5 | `uint16` | LV supply voltage | raw × 0.01 V |
| 6–7 | `int16` | MCU temperature | raw × 0.01 °C |

전류 양수는 배터리 방전/구동, 음수는 충전/회생으로 통일합니다.

### `0x521 EEM_STATUS` — 10 Hz, DLC 8

| Byte | 신호 | 설명 |
|---:|---|---|
| 0 | protocol version | 수신 호환성 |
| 1 | rolling counter | 0～255 순환 |
| 2 | status flags | calibration, SD, sensor |
| 3 | error flags | ADC, SD, brownout, internal |
| 4–5 | dropped samples | 누적 송신 누락 수 |
| 6–7 | CRC/checksum | 차량 규칙 적용 |

ID는 예시이므로 차량 전체 ID 할당과 충돌하지 않게 변경해야 합니다.

## 펌웨어 요구사항

- 기존 100 Hz ADC 측정과 SD 기록 유지
- CAN 송신은 non-blocking으로 구현
- 송신 큐가 가득 차도 측정과 SD 기록을 지연시키지 않음
- 동일한 측정 레코드를 SD와 CAN 양쪽에 사용
- 보정 완료 전 `calibration valid=0`
- 송신 실패 시 무한 재시도 대신 누락 카운터 증가
- rolling counter/CRC와 bus-off 복구 정책 적용

## 대시보드 상태 판정

| 상태 | 조건 |
|---|---|
| `ONLINE` | 정상 ID/DLC/counter/CRC 패킷 수신 |
| `DELAYED` | 100～500 ms 미수신 |
| `STALE` | 500～1500 ms 미수신, 마지막 값 회색 표시 |
| `OFFLINE` | 1500 ms 이상 미수신, 값 `--` 표시 |
| `INVALID` | counter/CRC 오류, 해당 패킷 폐기 |

CAN 어댑터가 열렸다는 사실만으로 `ONLINE`으로 판정하지 않습니다. 실제 패킷의 유효성과
갱신 주기를 기준으로 판단하며 실제 패킷은 `REAL HARDWARE SIGNALS` 창에만 표시합니다.

## 배선

```text
Energy meter / central logger
    CANH ───────────────────────── CANH ── USB-CAN ── Dashboard PC
    CANL ───────────────────────── CANL
    GND  ── 차량 절연·접지 정책에 따라 연결

120 Ω ── 버스 한쪽 끝                         반대쪽 끝 ── 120 Ω
```

- CANH/CANL은 꼬임선으로 배선합니다.
- HV 버스바와 인버터 상선에서 이격합니다.
- 스타 배선과 긴 스텁을 피합니다.
- 종단저항은 전체 버스 양 끝 두 곳에만 설치합니다.

## 검증 순서

1. 절연 USB-UART로 100 Hz 패킷과 실제 창을 검증합니다.
2. CAN 송신 보드와 500 kbit/s 통신을 벤치에서 시험합니다.
3. rolling counter 누락과 CRC 오류를 주입합니다.
4. SD 로그와 CAN 값이 동일한지 비교합니다.
5. bus-off, 케이블 분리, 재연결 및 재부팅을 시험합니다.
6. 검증 후 DBC와 CAN ID 할당표를 동결합니다.
