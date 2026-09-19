# TV 원본 배선·제어·통신 점검 — 2026-09-14

## 결론

**이번 TV 원본과 기존 EV 적용용 펌웨어는 리어 핀 배치가 다른 버전이다. 서로의 배선표를 섞으면 IMU/RPM/ESP가 안 들어올 수 있다. 원본에도 RPM 잡음 오인식, 조향각 실패 오판정, 기록 미구현 문제가 있다.**

이번 작업은 진단만 수행했다. TV 원본 파일, 기존 적용용 소스, 보드 펌웨어와 구동 설정은 변경하지 않았다. 아래 배선은 **소스가 요구하는 연결**이며 현재 실물 배선을 확인했다는 뜻이 아니다. 지금 바로 재배선하라는 지시도 아니다.

## 범위와 검증 수준

- 원본: `C:\Users\user\Desktop\OHTERS\TV\stm_front`, `stm_back`.
- 비교: EV 빌드 스크립트가 고정한 `DJY_TQV` 커밋 `ee9d295bae733b27dc93f7dd4871d38d284e9732` + `DJY_EvTLMT/firmware/stm32_rear_uart/Core` 오버레이. 오버레이에 없는 파일은 고정 커밋에서 가져온다.
- 양쪽 Core 애플리케이션·초기화·인터럽트, 헤더, IOC, CAN 계약, SD 드라이버, ESP/PC 입력 형식을 검토했다. 원본 폴더에서 별도 회로도 PDF/SVG/이미지는 발견하지 못했다. 코드 주석의 핀 표와 IOC/MSP, ST 공식 핀 표를 대조했다.
- Core C 24개(Front 9, Rear 15): ARM GCC `-fsyntax-only -Wall -Wextra` 통과. Rear에 기존 부호/범위 비교 경고 4건. **전체 링크·실보드 검증은 하지 않았다.**
- 원본 C를 그대로 포함한 PC 검사 3개로 RPM, SAS, 안전/차동 동작을 재현했다. HAL 입력만 가정했다. 물리적 잡음 파형을 측정한 것은 아니다.
- 검사 파일: `../.local/tv-original-audit-20260914/probe.c`. 이 파일의 assert는 현재 결함 재현을 확인하는 검사이지, 안전성 통과 검사가 아니다.
- STM/ESP 포트 열기, 리셋, 업로드, CAN/스로틀 명령 전송은 하지 않았다. 따라서 오늘 보드에 어떤 이미지가 실제 탑재돼 있는지는 이번 검사에서 재확인하지 않았다.
- 기존 로컬 `stm_rear.bin` SHA256은 `783F298374E45136AAAE900C29561634985F13D12ECA7205EDA88E9E9D8DE853`으로 확인했다. 로컬 파일 해시와 현재 보드 탑재 여부는 별개다. 앞서 작성한 IMU 자동 baud 탐색 후보는 적용 보류 상태이며, 이를 원본 또는 적용 완료 코드로 취급하지 않는다.

## 1. 버전별 리어 배선 — 혼용 금지

아래 TX/RX는 반드시 **센서/ESP 쪽 방향**까지 함께 읽는다.

| 기능 | 이번 TV 원본이 요구하는 핀 | 기존 EV 적용용 소스가 요구하는 핀 |
|---|---|---|
| IMU TX → STM RX | **PA1 / CN7-30, UART4 RX** | **PC11 / CN7-2, USART3 RX** |
| IMU RX ← STM TX | **PA0 / CN7-28, UART4 TX** | **PC10 / CN7-1, USART3 TX** |
| IMU 통신 | 115200, 8N1, DMA1 Stream2 Ch4 circular | 기존 고정 115200, DMA1 Stream1 Ch4 circular; 별도 탐색 후보 미적용 |
| 왼쪽 RPM 입력 | **PB4 / CN10-27, TIM3 CH1** | **PA0 / CN7-28, TIM2 CH1** |
| 오른쪽 RPM 입력 | **PB5 / CN10-29, TIM3 CH2** | **PA1 / CN7-30, TIM2 CH2** |
| 왼쪽 스로틀 출력 | 실제 설정 `DAC_USE_INTERNAL=1`: **PA4 / CN7-32** | PA4 / CN7-32 |
| 오른쪽 스로틀 출력 | 실제 설정 `DAC_USE_INTERNAL=1`: **PA5 / CN10-11** | PA5 / CN10-11 |
| 외부 DAC SPI | PC10 SCK / PC11 CS / PC12 MOSI 초기화됨. 현재 내부 DAC 선택이므로 외부 DAC 출력 경로는 비활성 | 없음; PC10/11은 IMU |
| STM → ESP GPIO18 | **PA9 USART1 텔레메트리 구현 없음** | PA9 / CN10-21 → GPIO18 |
| ESP GPIO17 → STM | **PA10 명령 수신 구현 없음** | GPIO17 → PA10 / CN10-33 |
| PC 유선 디버그 | USART2 PA2/PA3, ST-Link VCP, 115200 | 동일 UART, 확장 텔레메트리 형식 |
| Front↔Rear CAN | PA12 TX / CN10-12, PA11 RX / CN10-14 | 동일 |
| SD SPI | PB13 SCK / CN10-30, PB14 MISO / CN10-28, PB15 MOSI / CN10-26, PB1 CS / CN10-24 | 동일 핀 계열 |

**센서가 꺼져 있는 것 같은 증상은 UART 핀 변경만으로 설명되지 않는다.** 센서 단자에서 실제 공급 전압을 별도로 확인해야 한다. CN7-18은 +5V이며 CN7-8/20은 GND다. 'CN8' 자체는 GND의 별칭이 아니다.

커넥터 번호는 [ST UM1724 Rev17, Table 29, p59](https://www.st.com/resource/en/user_manual/um1724-stm32-nucleo64-boards-mb1136-stmicroelectronics.pdf#page=59)로 확인했다. 센서 전원 규격과 실제 모델/하네스 확인은 별도이며 MCU의 UART 신호 전압과 센서 공급 전압을 혼동하면 안 된다.

### 사용자가 마지막에 제공한 배선 기록과의 충돌

| 원문 | 점검 결과 |
|---|---|
| `VCC CN7 18 STM 보라색 -> 초록색 -> 노랑` | CN7-18은 +5V. 센서 끝단의 실제 전압/접촉은 미확인 |
| `TX CN7 PA10 STM 파랑색 -> 노란색 -> 주황색` | PA10은 CN7이 아니라 CN10-33. 혹시 PC10을 뜻했는지 미확인. 이번 원본의 IMU 핀은 어느 쪽도 아님 |
| `RX CN7 PA11 STM 초록색 -> 흰색->흰색` | PA11은 CAN RX/CN10-14. PC11을 뜻했다면 이번 원본에서는 DAC CS 출력이므로 IMU TX와 연결하면 출력끼리 충돌 가능 |
| `GND CN 8 파랑색->파랑색->보라색` | CN7-8을 뜻하는지 CN8 커넥터인지 미확인 |

색상은 사용자 제공 기록일 뿐 전기적 기능을 증명하지 않는다. PA/PC와 TX/RX 방향을 추측해서 수정하지 않았다.

### 프론트 원본 배선

| 기능 | STM 핀 | 연결 대상 |
|---|---|---|
| TPS | PA0 / CN7-28 / A0 | 센서 아날로그 출력, ADC1 IN0 |
| SAS SCK | PB3 / CN10-31 | AS5147 SCK |
| SAS MISO | PB4 / CN10-27 | AS5147 데이터 출력 |
| SAS MOSI | PB5 / CN10-29 | AS5147 데이터 입력 |
| SAS CS | PB6 / CN10-17 | AS5147 CS |
| CAN TX/RX | PA12 / PA11 | CAN 트랜시버의 TXD/RXD, 버스 H/L 직결 아님 |
| USB 디버그 | PA2/PA3 | ST-Link VCP. 원본 Front main에는 주기적 센서 UART 출력 없음 |

Front와 Rear는 별도 MCU라 양쪽에서 PB4/PB5를 서로 다른 용도로 쓰는 것 자체는 충돌이 아니다. Front SPI1은 Mode1/16bit, 실제 SCK는 84MHz/64 = 약 1.3125MHz. TPS는 868~3102, 진단 margin 200, 8회 평균이다. 현재 센서 공급/출력 전압은 실측하지 않았다.

## 2. 우선 수정해야 할 확정 문제

### P1 — RPM 잡음을 정상 회전으로 채택

근거: `TV/stm_back/Core/Src/rpm_sensor.c:105`의 `capture_event()`, 특히 113~115행. 너무 빠른 에지를 버릴 때 기준점을 유지하므로 반복 잡음이 누적되어 약 11.9ms가 되면 정상 주기로 채택된다.

원본 C에 80µs 간격 에지를 2초 동안 입력한 PC 재현:

```text
noise_80us: left=7975 right=7975 fresh=1
```

정지/잡음 상태를 유효 회전으로 만들 수 있다. 차속과 TV 진입 판단에도 영향을 준다. 이것은 기존 EV 오버레이에서 잡음 필터를 수정했던 종류의 문제이며 이번 TV 원본에는 남아 있다. TIM2용 수정 파일을 TIM3 원본에 그대로 덮어쓰면 안 된다.

### P1 — 조향각 SPI 실패를 정상 0으로 판정

근거: `TV/stm_front/Core/Src/sas_sensor.c:35`, `:49`. `HAL_SPI_TransmitReceive()` 반환값을 무시한다. `rx=0`은 parity 및 error-bit 검사를 통과한다.

```text
spi_timeout_no_data: angle=0 sas_error=0
```

MISO가 Low 고정인 경우에도 0 프레임이 통과할 수 있다. 센서 미장착/미수신이 곧 정상 중심값이라는 뜻이 아니다. 이번 원본의 중심 7220, 조향비 -1을 적용하면 raw 0은 +0.70rad 제한값까지 올라가므로 잘못된 ED 차동의 원인이 될 수 있다. HAL 반환값·센서 진단·유효 프레임 여부를 함께 판정해야 한다. 단순히 raw 0을 금지하는 방식은 실제 유효 각도까지 버릴 수 있다.

### P1 — 조향각 하트비트가 오래되면 차동 차단을 해제

근거: `TV/stm_back/Core/Src/safety_monitor.c:55`. 센서 프레임은 계속 오고 SAS 고장 하트비트만 끊긴 경우, 500ms 후 SAS 검사를 건너뛴다. 다른 입력을 정상으로 둔 재현 결과 `action=0 fault=0`이었다. 상태를 모르는 경우도 차동을 허용하지 않도록 다뤄야 한다.

### P1 — 차동 비활성 직후에도 필터 잔여 차동 출력

근거: `torque_vectoring.c:142` 이후, `electronic_diff.c:21`. TV/ED가 모두 꺼져도 ED LPF와 슬루 상태를 서서히 0으로 줄인다. `SAFE_ACTION_DISABLE_DIFF` 주석의 즉시 50:50과 실제 출력이 다르다.

50% 스로틀/0.70rad 조향, TV OFF·ED ON으로 수렴시킨 뒤 ED까지 OFF한 재현:

```text
tv_off_ed_on: left=1.6781 right=3.0719 dp=1.3939
diff_disabled_first_tick: left=1.8447 right=2.9053 dp=1.0606 active=0/0
```

숫자는 코드의 kW **지령 모델값**이며 실측 전력이 아니다. 안전 차단 시에는 이전 차동 상태를 별도로 제거해야 한다. 일반적인 TV↔ED 전환 평활과 고장 차단을 구분할 필요가 있다.

### P1 — CAN 프레임 형식 검증 없이 센서값 갱신

근거: `TV/stm_back/Core/Src/can_comm.c:102`, `:113`. 전수락 필터인데 수신 후 `IDE/RTR/DLC`를 검증하지 않고 `switch(h.StdId)` 및 `d[0..3]` 해석을 수행한다. 확장 프레임에서는 HAL이 `ExtId`만 채우므로 초기화되지 않은 `StdId`를 사용할 수 있다. 짧은 프레임/remote frame도 센서 신선도를 갱신할 수 있다. 표준 data frame 및 ID별 DLC 검사가 필요하다. 이는 소스/HAL 대조 결과이며 이번에 버스에 시험 프레임을 송신하지 않았다.

## 3. 배선/버전 불일치와 중요한 동작 차이

### 내부/외부 DAC 설명이 섞여 있다

- `board_config.h:31`은 PA4/PA5 미사용이라고 설명하지만 `vehicle_params.h:231`은 **DAC_USE_INTERNAL=1**이다.
- 실제 컴파일 분기는 `dac_output.c:28` 내부 DAC, `main.c:178` 내부 DAC 초기화다. 현재 원본 설정으로는 MCP4822에 모터 지령을 쓰지 않는다.
- 그런데 SPI3 PC10/12와 CS PC11 초기화는 내부 DAC 모드에서도 수행한다. 따라서 PC11을 이전 IMU RX처럼 쓰면 안 된다.
- `dac_output.c:9`, `vehicle_params.h:229` 등에 남은 외부 DAC PB3/PB4/PB5 주석도 실제 PC10/11/12와 다르다.
- IOC에는 PA4/PA5 내부 DAC가 빠져 있어 현재 수동 C 수정과 일치하지 않는다. CubeMX 재생성이 사용자 영역 밖의 수동 초기화 등을 훼손할 위험이 있다.
- 외부 DAC 모드에서 SPI 실패는 카운터만 올리고 안전 차단과 연결되지 않는다. 모듈 미연결도 SPI 송신 성공으로 보일 수 있어 카운터 0이 아날로그 출력 정상의 증거가 아니다.

### PA0/PA1 전기 규격에 관한 원본 주석 오류

원본은 PA0/PA1을 ADC가 있으므로 TTa라고 설명하지만 **STM32F446 공식 핀 표는 PA0/PA1을 FT, PA4/PA5를 TTa로 구분한다.** ADC 기능 유무만으로 5V 내성을 판단할 수 없다. [ST DS10693 Rev11, Table 10 p47–48](https://www.st.com/resource/en/datasheet/stm32f446re.pdf#page=47)

이 정정은 차량 SPD 직결을 승인한다는 뜻이 아니다. 핀 모드, 전원 유무, 내부 pull, 전압 최대치·음전압·과도파형까지 실제 조건에 맞게 검토해야 한다. 단순 직렬 저항만으로 모든 과전압이 안전해지는 것도 아니다. 원본 주석의 사고 경위와 컨트롤러 18번/SPD 설정은 팀의 기록이지 이번 실측/컨트롤러 매뉴얼 검증 결과가 아니다.

### 조향 보정과 PID도 같은 버전이 아니다

| 상수 | 기존 EV 고정 베이스 | 이번 TV 원본 |
|---|---:|---:|
| SAS_CENTER_RAW | 8192 | 7220 |
| SAS_TO_STEERING_RATIO | -0.2 | -1.0 |
| GEAR_RATIO_DEFAULT | 3.8 | 4.0 |
| PID Kp / Ki / Kd | 5 / 2 / 0.5 | 18 / 1 / 1.5 |
| RPM_PULSES_PER_REV / RPM_CAL_GAIN | 11 / 17.45 | 11 / 17.45 |

같은 센서 raw라도 조향각, 목표 yaw, 차속, PID 출력이 달라진다. 어느 보정값이 실차에 맞는지는 실제 중심/휠 조향비/기어비 확인 전에는 확정 불가다. 기존 값을 원본으로 일괄 덮어쓰는 것은 통신 수정이 아니라 제어 특성 변경이다.

### TV OFF ≠ 항상 동일한 좌우 출력

원본 `main.c:862`는 TV가 OFF이거나 IMU/RPM에 문제가 있어도 SAS가 정상이라고 판단하면 ED를 켠다. 따라서 TV 차단만으로 50:50이 되는 구조가 아니다. 위 PC 재현에서도 TV OFF에 좌우 전력 지령이 달랐다. 이것만으로 실제 한쪽 모터 미동/미회전의 원인이 확정되지는 않으며 DAC 핀 전압과 컨트롤러 상태 검증이 필요하다.

또한 `power_left/right`는 선형 스로틀→전력 가정의 계산값이다. 실제 모터 전압/전류/전력 피드백은 원본에 없으며, 실제/목표 RPM 폐루프 제어도 없다. `desired_yaw`는 TV 실제 활성 구간에만 계산하고 비활성 구간에는 0으로 설정한다.

## 4. IMU/ESP/기록 문제

### IMU 수신 경로 자체는 UART4로 일치하지만 유효성 보강 필요

- 원본 main/MSP/IRQ/IOC는 UART4 PA0/PA1, RX DMA1 Stream2 Ch4 circular로 일치한다. 기존 PC10/11 배선 안내는 이 원본에 적용되지 않는다.
- 115200 고정이다. 실제 센서 baud/전원/출력 패킷 설정은 이번에 읽지 않았다.
- 자이로 0x52의 wz, 가속도 0x51의 ay 위치와 checksum 처리는 소스상 일관된다. 별도 ax/az 출력은 이 원본에 없다.
- `IMU_IsValid()`는 자이로 시간만 보며 가속도 freshness를 검사하지 않는다. 자이로만 계속 오면 오래된 횡가속도를 traction 계산에 쓸 수 있다.
- 부팅 직후에는 수신 0회도 timestamp=0 때문에 timeout 범위에서 valid가 될 수 있다. 정상 main은 1초 보정 후 제어를 시작하므로 이를 곧바로 현재 주행 원인으로 단정하지 않는다.
- 설정 송신/DMA 시작 반환값과 센서 설정 적용 여부를 검증하지 않는다. UART4 IRQ는 없고 main watchdog으로 오류를 복구한다.
- 보정 성공 여부는 출력하지만 안전 판정의 필수 조건은 아니다. 움직이는 중 부팅한 경우 별도 정책이 필요하다.

### ESP에 보내는 코드가 없다

원본 Rear에는 USART1/PA9·PA10 텔레메트리 구현이 없다. PA9→GPIO18 기존 배선만 유지한 채 원본으로 교체하면 ESP에 차량 데이터가 안 들어온다.

USART2 디버그 줄은 RPM/TPS/진단 카운터만 포함한다. `imu=gyro_ok/bad/resync/restart`는 네 개의 카운터이고 yaw 측정값이나 boolean valid가 아니다. 현재 EV PC parser는 `imu=1`과 별도의 `yaw=`가 있어야 IMU 수신으로 표시하므로 **원본 디버그를 받는 것과 대시보드에 IMU가 표시되는 것은 다르다.** PA2로 옮기는 것만으로 모든 센서/TV 값이 전달되지 않는다.

`main.c:767`의 160바이트 디버그 버퍼에도 길이 문제가 있다. `snprintf()` 반환값은 필요한 전체 길이인데 `:797`에서 그 값을 그대로 전송 길이로 사용한다. 누적 카운터가 커져 160을 넘으면 문자열이 잘리고 버퍼 뒤 메모리까지 전송할 수 있다. 실제 송신 길이를 버퍼 안으로 제한해야 한다.

### 원본 SD 기록은 현재 동작하지 않는다

- `FATFS/Target/user_diskio.c:84/99`는 항상 `STA_NOINIT`를 반환하는 미구현 드라이버다. read/write도 실제 SPI 전송 없이 성공값만 반환한다.
- 따라서 `sd_logger.c:26`의 mount에서 빠져나와 정상적으로 로그 파일을 열 수 없다. SD 꽂힘이나 logger 함수 존재는 기록 증거가 아니다.
- 드라이버를 구현한 뒤에도 `FA_CREATE_ALWAYS`로 부팅마다 `tvlog.csv`를 덮어쓴다.
- SAS 열은 0 상수이고 `ed_active` 열이 없다. `fault`에는 상세 fault code가 아니라 action이 기록된다.
- write/sync 결과·실제 기록 바이트 수를 확인하지 않고 버퍼를 비운다. close도 active 부분 버퍼를 저장하지 않는다.
- 제공된 Debug makefile은 nano printf의 `_printf_float` 링크 옵션이 없는데 CSV는 `%f`를 사용한다. 기록 드라이버를 완성해도 포맷/빌드 설정까지 확인해야 한다.
- PC의 SQLite/측정 세션 저장과 STM SD 저장은 별개다. 이번 점검에서는 PC DB 기록 시작/중지 상태를 바꾸지 않았다.

## 5. CAN·주기·RPM 산식 점검

- 양쪽 HSI16MHz → PLL168MHz, APB1 42MHz. CAN은 `42MHz/(6×(1+10+3)) = 500kbit/s`로 일치한다.
- Front 0x100/DLC4: SAS u16 + TPS u16 little-endian, 100Hz. 0x300/DLC1: 상태, 10Hz. Rear의 해당 해석과 일치한다.
- Rear CAN의 0x200/0x201 저장 경로는 실제 제어 RPM 입력이 아니다. 제어는 TIM3의 PB4/PB5를 사용한다. 컨트롤러 CAN RPM 규격은 확인되지 않았다.
- TIM6은 84MHz/(840×1000)=100Hz. TIM3은 10µs/tick, 16bit wrap=655.36ms다.
- RPM = `60,000,000 / (period_us × 11) × 17.45`. 차량 속도는 좌우 평균 모터 RPM을 기어비 4로 나눠 타이어 반경 0.2286m로 환산한다. GPS 속도가 아니다.
- 400ms stale 및 현재 보정계수 때문에 약 238RPM 아래에서는 연속적인 유효 RPM을 기대하기 어렵다. 극저속 미수신과 정지·센서 단절을 단일 zero로 구분할 수 없다.
- Front `CAN_TEST_MODE=0`, Rear `BENCH_TEST_MODE=0`, `CAN_SNIFF_MODE=0`, `DAC_SELFTEST_MODE=0`. 합성 값 테스트 코드는 남아 있지만 현재 원본 설정으로 활성화되어 있지는 않다.
- Front TIM6 ISR 안에서 blocking SPI/ADC를 사용하고 SysTick/TIM6이 같은 선점 우선순위다(Front는 priority grouping 0). 하드웨어 flag가 멈춘 오류에서는 HAL의 tick 기반 timeout도 진행하지 못할 위험이 있다. Rear 외부 SPI DAC 경로도 TIM6 내부 blocking 송신이므로 같은 주의가 필요하다. 실제 정지 재현은 하지 않았다.
- Rear Error_Handler/하드 fault에 대한 독립 출력 차단이나 활성 하드웨어 watchdog은 확인되지 않았다. MCU 정지 시 마지막 DAC값 유지 위험은 별도 하드웨어 안전 설계로 다뤄야 한다.

## 6. 다음 조치 순서 — 아직 시행하지 않음

1. **보드에 올릴 기준 버전과 실제 하네스부터 확정.** 지금 PC10/11 기반 EV를 유지할지, UART4/PB4/PB5 원본으로 이관할지 섞지 않는다.
2. IMU 전원은 센서 끝단 전압으로 확인하고, 핀 글자 PA/PC 및 GND 커넥터 번호를 확정한다. 전원 없는 센서는 소프트웨어로 살릴 수 없다. 재배선은 전원을 끈 뒤 수행한다.
3. 확정한 버전에 맞춰 RPM 잡음 필터, SAS 실패/하트비트 판정, 안전 차동 차단, CAN 프레임 검증부터 최소 변경으로 수정한다. 기존 TV 수식/PID를 통신 작업과 섞어서 변경하지 않는다.
4. 그 다음 USART1 텔레메트리와 각 값의 valid/freshness를 연결한다. 측정값과 지령값, 무신호와 실제 0을 별도로 표시한다.
5. 구동 전원/출력 물리 차단을 새로 확인한 뒤 수신 시험 및 DAC 전압 비교를 진행한다. PC 검사 통과를 실차 안전 확인으로 대체하지 않는다.
6. 기록은 PC DB 세션부터 실제 행 증가/재열람을 검증한다. STM SD가 필요하면 드라이버와 유실/재부팅 보존 정책을 별도 구현한다.

핵심: **IMU 고장 하나로 단정할 상황이 아니다. 원본·적용용 소스·배선의 버전 불일치와 재현되는 소프트웨어 결함이 함께 있다.**
