# tv_stm_esp 통합 소스 (2026-09-18)

사용자가 지정한 `tv_stm_esp.zip`의 Front/Rear 소스를 이 폴더에 추가했다.
원본 압축파일은 수정하지 않았다. Debug/Release 결과물과 IDE 개인 설정 폴더는 제외했다.
압축파일 SHA256: `5ff46c099e05cf41284a1442c6cd0621dd7599425b7150225c22c5a0afe17a8c`.

## 핀맵

| 보드 / 기능 | MCU 핀 | 연결 / 설정 |
|---|---|---|
| Front TPS | PA0 | ADC1_IN0 |
| Front SAS | PB3 / PB4 / PB5 / PB6 | SPI1 SCK / MISO / MOSI / CS |
| Front TV 스위치 | PC13 | 풀업, GND로 닫히면 ON, CAN 0x100 byte4 bit0 |
| Front ↔ Rear CAN | 각 보드 PA12 TX / PA11 RX | 차량 CAN 트랜시버 경유, 500 kbit/s |
| Rear IMU | PA0 TX / PA1 RX | UART4, 115200 8N1, DMA1 Stream2 Ch4 |
| Rear RPM 좌 / 우 | PB4 / PB5 | TIM3 CH1 / CH2 |
| Rear 현재 DAC 좌 / 우 | PA4 / PA5 | `DAC_USE_INTERNAL=1` |
| Rear 외장 DAC 예약 배선 | PC10 / PC11 / PC12 | SPI3 SCK / CS / MOSI, 현재 출력 백엔드 아님 |
| Rear SD | PB13 / PB14 / PB15 / PB1 | SPI2 SCK / MISO / MOSI / CS |
| Rear → ESP | PA9 TX → GPIO18 RX | USART1, 115200 8N1, 5 Hz |
| ESP → Rear | GPIO17 TX → PA10 RX | 핀 초기화만 존재, 명령 수신·제어 없음 |
| Rear → PC | PA2 TX / PA3 RX | USART2 / ST-Link VCP, 동일한 확장 텔레메트리 |
| ESP BMS CAN | GPIO9 TX / GPIO8 RX | 별도 3.3 V CAN 트랜시버 경유, DALY 250 kbit/s |
| UART 기준 | Rear GND ↔ ESP GND | 3.3 V TTL |

`board_config.h`의 PA4/PA5 미사용 주석과 달리 실제 활성 설정은 내부 DAC이다.
핀맵은 헤더 주석뿐 아니라 `main.c`와 MSP 초기화로 확인했다.
원본 `.ioc`에는 새 USART1/IRQ 설정이 반영되지 않았으므로 CubeMX 재생성 시 추가 부분을 보존해야 한다.

## 유지한 원본 제어값

| 항목 | 압축본 기준 |
|---|---|
| TPS MIN / MAX / 진단 마진 | 885 / 2820 / 200 (유효 범위 685~3020) |
| TPS 데드밴드 / 끝단 마진 | 110 / 80, 부팅 idle 학습 유지 |
| SAS 중앙값 / 조향비 | **1116 / -0.5** |
| PID P / I / D | **20 / 1 / 0** |
| 감속비 / 타이어 반경 | 4.0 / 0.2286 m |
| RPM 펄스 설정 / 보정 gain | 11 / 17.45 |
| BENCH / CAN_SNIFF / DAC_SELFTEST | 모두 0 |

과거 문서의 SAS 6897, PID 10/2/0을 이 소스에 덮어쓰지 않았다.
1116이 현재 실차의 직진 위치인지 이번 작업에서는 측정하지 않았다.
Front 원본, Rear의 `vehicle_params.h`, TV/ED/PID/안전 판정과 DAC 구동 로직을 유지한다.

## 텔레메트리 추가

- 기존 PA2의 RPM/TV 두 줄을 하나의 확장 문장으로 합쳐 PA9와 PA2에 함께 전송한다.
  `L/R`, `cap/glt`, `tps/pct/idle`, IMU, SAS, TQV 내부값, DAC, fault, `rv`, `steer/sv/sc`를 담는다.
  기존 `dsy`, `can` 진단 카운터도 유지하며, IMU 진단 tuple은 센서 유효성 `imu`와 구분해 `idg`로 보낸다.
- USART1/USART2 인터럽트 송신을 사용한다. 어느 UART든 이전 버퍼를 쓰고 있으면 이번 샘플은 건너뛴다.
  문자열이 768바이트 버퍼를 넘으면 송신하지 않는다. ESP 수신 버퍼도 768바이트이다.
  제어 ISR에서는 UART를 기다리지 않는다.
- RPM 좌우 freshness 조회와 IMU 가속도 원본 조회를 추가했다. 기존 제어 필터/유효 판정은 변경하지 않았다.
- `steer`는 원본 `SAS_to_SteeringAngle()`로 raw값을 변환한 센서 표시값이다.
  TV 제어 내부의 필터 적용 각도와는 구분한다. `sv`는 Front 센서와 heartbeat freshness 및 SAS 오류를 확인한다.
- STOP 시 `tv`가 0으로 지워져도 DAC 필드는 실제 적용한 off 지령 코드(내부 DAC 기준 1116)를 보고한다.
  이는 출력 전압의 실측값이 아니다.
- `ctl/req/lim/app/urx/uerr=0`: 원격 피트 제어는 지원하지 않는다.
  BMS 수신과 ESP 읽기 전용 설정은 유지한다.
- ESP는 STM의 `vs`를 km/h로 변환한다. 이전 3.8 감속비로 재계산하지 않는다.
  ESP/USB의 TPS 진단 범위와 USB의 `steer/sv/sc` 해석도 맞췄다.
- `kp/ki/kd`는 `TV_Init`에 전달한 PID 상수를 1,000배 한 정수로 전송한다.
  현재 펌웨어에는 런타임 gain 변경 경로가 없다. USB/ESP는 `pid_kp/ki/kd`, `pid_online`으로 전달한다.
  PID 숫자 자체와 차량 제어 로직은 변경하지 않았다. 구버전 보드는 PID 미수신으로 표시된다.

## 빌드와 검사

저장소 루트:

```powershell
.\scripts\build_stm32_rear_uart.ps1
python -m unittest discover -s tests -v
```

기본 빌드는 이제 이 폴더의 Front와 Rear를 사용한다.
결과는 `.local/tv-stm-esp/stm_front.*`, `stm_rear.*` (ELF/BIN/HEX/MAP)이다.
과거 고정 커밋 오버레이가 필요할 때만 `-LegacyPinned`를 지정한다.

ESP 폴더 `firmware/esp32_ev_gateway`:

```powershell
$env:PLATFORMIO_CORE_DIR='C:\p'
pio run -c platformio-modern.ini
```

ESP 결과: `.pio/build/esp32-s3-devkitc-1/firmware.bin`.
설치된 로컬 `config.h`의 Wi-Fi 설정을 사용하며 비밀값은 이 문서에 기록하지 않는다.

새 검사 `tests/test_tv_stm_esp.py`는 실제 STM 송신 함수를 호스트에서 실행해
ESP 파서와 USB → TelemetryStore → 화면 값 변환에 연결한다.
UART busy 버퍼 보존, 조향 중앙값, TPS 경계, STM 차속, CAN/SAS 미수신 및 데이터 만료를 검사한다.
기존 BMS JSON/파서 검사도 함께 실행한다.

이번 작업은 소스 통합·호스트 검사·빌드까지이며, 보드 기록 및 실차 수신/전압/회전 검증은 하지 않았다.
