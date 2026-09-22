# STM–ESP 바이너리 UART 변경 (2026-09-21)

**후속 변경:** 아래 패킷 형식·460800 baud는 유지하면서 후방 전송 요청을 100 Hz로 올리고, ESP·서버·웹의 묶음 처리도 함께 수정했다. 현재 동작과 보관 한계는 [고속 수집·전체 기록 설명](HIGH_RATE_TELEMETRY_KO.md)을 기준으로 한다. 아래 검증 이력과 최초 5 Hz 운용 설명은 바이너리 전환 시점의 기록이다.

후방 STM → ESP 주행 데이터를 ASCII 문장에서 **238바이트 고정 바이너리 패킷**으로 변경했다.
양쪽 UART는 **460800 baud, 8N1**로 설정했다. 센서와 진단값의 기존 x1000 정밀도를 유지하고,
중복 값만 통합했다. CAN이나 모터 제어 설정은 이번 통신 변경 대상이 아니다.

## 수정 대상과 호환성

- 실제 후방 프로젝트: `firmware/stm32_rear_binary/`. `firmware/tv_stm_esp/stm_back/`는 별도의 과거 소스이며 수정하지 않았다.
- ESP 프로젝트: `firmware/esp32_ev_gateway/`.
- 전방 STM과 CAN 500 kbit/s는 그대로다.
- PA9 TX → ESP GPIO18 RX, ESP GPIO17 TX → PA10 RX 양방향 링크가 460800 baud다.
- ESP → STM 제어 명령은 기존 9바이트/CRC8 형식을 유지한다. 같은 UART이므로 이 방향도 460800이다.
- STM USART2 USB 진단 출력과 UART4 IMU는 각각 기존 115200 설정을 유지한다.
- STM과 ESP를 **함께 업데이트**해야 한다. 새 ESP에서 과거 ASCII STM을 쓰는 경우에만
  로컬 `config.h`에 `EV_REAR_UART_BINARY=0`을 정의하면 115200 ASCII 모드가 된다.
- STM의 `.ioc`에도 USART1 460800 설정을 반영했다.
- 공유 프로토콜 헤더는 STM Core/Inc와 ESP include에 각각 있으며, 테스트로 내용 일치를 검사한다.
  C 구조체 메모리를 그대로 전송하지 않고 바이트별로 직렬화하므로 구조체 패딩에 의존하지 않는다.

## 패킷 정의

정수는 little-endian, 부호 있는 정수는 2의 보수다. 16진수는 바이트를 읽기 위한 표기이며,
`"D5 4A ..."` 같은 문자열을 전송하는 것이 아니다.

| 위치 | 내용 |
|---|---|
| 0–1 | 시작 바이트 D5 4A |
| 2 | 버전 1 |
| 3 | 메시지 종류 2 (주행 데이터) |
| 4–5 | payload 길이 230 = E6 00 |
| 6–159 | 아래 주행·상태·진단 필드 |
| 160–235 | 기존 ts=의 19개 32비트 필드 |
| 236–237 | CRC16 little-endian |

CRC는 CRC-16/CCITT-FALSE(poly=0x1021, init=0xffff, refin/refout=false, xorout=0)이며
0–235 바이트를 검사한다. 표준 확인 문자열 `123456789`의 CRC는 0x29B1이다.

| 바이트 위치 | 필드 | 형식 |
|---|---|---|
| 6–9 | `sequence` | u32 |
| 10–17 | `snapshot_us` | u64 |
| 18–21 | `tx_skipped` | u32 |
| 22–23 | `rpm_left` | u16 |
| 24–25 | `rpm_right` | u16 |
| 26–27 | `tps_raw` | u16 |
| 28–29 | `tps_idle` | u16 |
| 30–31 | `sas_raw` | u16 |
| 32–33 | `sas_center` | u16 |
| 34–35 | `dac_left` | u16 |
| 36–37 | `dac_right` | u16 |
| 38–39 | `traction_milli` | u16 |
| 40 | `tps_pct` | u8 |
| 41 | `flags` | u8 |
| 42 | `fault` | u8 |
| 43 | `command_sequence` | u8 |
| 44 | `requested` | u8 |
| 45 | `limit` | u8 |
| 46 | `applied` | u8 |
| 47 | `timing_valid` | u8 |
| 48–51 | `yaw_milli` | i32 |
| 52–55 | `lat_milli` | i32 |
| 56–59 | `ax_milli` | i32 |
| 60–63 | `ay_milli` | i32 |
| 64–67 | `az_milli` | i32 |
| 68–71 | `speed_milli` | i32 |
| 72–75 | `desired_yaw_milli` | i32 |
| 76–79 | `yaw_error_milli` | i32 |
| 80–83 | `delta_power_milli` | i32 |
| 84–87 | `power_left_milli` | i32 |
| 88–91 | `power_right_milli` | i32 |
| 92–95 | `steer_milli` | i32 |
| 96–99 | `kp_milli` | i32 |
| 100–103 | `ki_milli` | i32 |
| 104–107 | `kd_milli` | i32 |
| 108–111 | `capture_left` | u32 |
| 112–115 | `capture_right` | u32 |
| 116–119 | `glitch_left` | u32 |
| 120–123 | `glitch_right` | u32 |
| 124–127 | `command_rx` | u32 |
| 128–131 | `command_errors` | u32 |
| 132–135 | `can_rx` | u32 |
| 136–139 | `can_errors` | u32 |
| 140–143 | `can_status` | u32 |
| 144–147 | `imu_gyro_ok` | u32 |
| 148–151 | `imu_pkt_bad` | u32 |
| 152–155 | `imu_resync` | u32 |
| 156–159 | `imu_dma_restart` | u32 |

`*_milli` 값은 기존 단위의 1000배 정수다. yaw/desired_yaw/yaw_error는 rad/s,
lat/ax/ay/az는 m/s², speed는 m/s, delta_power/power_left/power_right는 kW,
**steer는 rad**다. PID와 traction도 기존 x1000 배율을 유지한다.

flags: bit0 IMU 유효, bit1 SAS 유효, bit2 좌 RPM 유효, bit3 우 RPM 유효,
bit4 ESP 명령 fresh, bit5 TV 작동, bit6 ED 작동. bit7은 예약이며 수신 시 거부한다.
`timing_valid=0`이면 timing 배열은 유효하지 않다.

timing[0..18]은 기존 순서 그대로:
version, control_seq, metric, count, min, mean, p95_upper, max,
sync_valid, rtt_us, bound_us, drift_ppm, front_timestamped, front_span_us,
front_unmapped, front_negative, overruns, timing_clock_hi, timing_clock_lo.
**drift_ppm(11)만 int32**, 나머지는 uint32다. 13개 통계 중 하나를 순환해서 보낸다.

## 시간·누락의 의미

- `sequence`: 현재 100 Hz 전송 요청마다 증가한다(최초 바이너리 전환 시에는 5 Hz). UART busy나 메인 루프 지연으로 건너뛴 요청도 포함한다.
- `snapshot_us`: 후방 STM 메인 루프가 상태를 복사한 64비트 시각이다.
  **센서 자체 측정 시각이나 100 Hz 제어 틱 시각을 뜻하지 않는다.**
- `tx_skipped`: 그 패킷 이전의 전송 요청 중 UART 송신 시작에 성공하지 못한 누적 횟수다.
  전송 성공은 HAL이 인터럽트 송신을 수락했다는 뜻이며, ESP의 수신 확인을 뜻하지 않는다.
- ESP는 수신 순번 공백, 같은 순번/시각의 중복, 후방 시계가 뒤로 이동한 횟수를 별도로 센다.
  순번 공백은 STM 건너뛰기와 통신 손실을 함께 포함한다.
  시계 역행 횟수는 관측 가능한 재시작 단서이며, 모든 재시작을 식별하는 고유 boot ID는 아니다.
- CRC·필드 범위·버전·길이를 통과한 패킷만 상태를 갱신한다. 중복 패킷은 freshness를 갱신하지 않는다.
- 바이트 누락/삽입/CRC 오류 후 다음 정상 프레임을 재탐색한다. 수신 중간 상태가 50 ms 이상
  진행되지 않으면 버린다. `rear_uart_errors`에는 거부 후보, 유효성 오류, 중복 및 중간 수신 타임아웃이 포함된다.

ESP는 기존 JSON 필드 이름과 단위로 변환하므로 현재 웹 화면은 그대로 해석한다.
새 JSON의 `rear_sample`은 다음 배열이다.

```text
[1, STM순번, snapshot_us상위32비트, snapshot_us하위32비트,
 STM송신건너뛰기, ESP수신순번공백, 중복수신, STM시계역행]
```

64비트 시각은 상·하위 워드로 전달하여 자바스크립트 숫자의 정밀도 문제를 피한다.
서버 `TelemetryStore`가 추가 필드를 보존하므로 DB의 `payload_json`에도 남는다.
기존 CSV의 고정 열에 이 배열을 펼치는 작업은 이번 변경에 포함하지 않았다.
기존 `timestamp_ms`는 HTTPS 배치가 사용하는 **ESP 시계**로 유지한다.
새 배열이 남아 있는 heartbeat도 새로운 STM 샘플로 해석하면 안 된다.

## Baudrate 선정

8N1에서는 데이터 1바이트에 시작/정지 비트를 포함해 10비트가 필요하다.

```text
100 Hz 필요량 = 238 B × 100/s × 10 bit/B = 238000 bit/s
460800에서 프레임 전송 시간 = 238 × 10 / 460800 = 5.165 ms
```

| Baudrate | 5 Hz 사용률 | 100 Hz 사용률 |
|---|---:|---:|
| 115200 | 10.33% | 206.60% (불가) |
| 230400 | 5.16% | 103.30% (불가) |
| **460800** | **2.58%** | **51.65%** |
| 921600 | 1.29% | 25.82% |

460800은 이 후보들 중 100 Hz 전송이 가능하면서 약 48%의 회선 여유를 남기는 가장 낮은 속도다.
표는 후방→ESP 방향 기준이다. 역방향의 9바이트 제어 명령은 UART의 별도 TX/RX 선을 사용한다.
이 계산은 회선 용량 검토이며 실제 배선의 신호 품질이나 CPU 처리시간을 측정한 결과는 아니다.

**최초 바이너리 전환 때에는 5 Hz를 유지했고, 후속 묶음 전송 수정에서 100 Hz로 변경했다.** ESP 수신 버퍼는 2048바이트(완전한 패킷 8개 이상)다.
향후 100 Hz 기록을 적용하려면 각 틱의 상태 확보·큐 관리 및 서버 전달 처리량도 함께 검증해야 한다.
현재 메인 루프는 지연 중의 모든 제어 틱을 저장하는 큐가 아니며, 건너뛴 전송 요청을 계수한다.

## 최초 바이너리 전환 시점의 웹 전송 제한 (현재는 해소됨)

최초 바이너리 전환 시점에는 다음 두 곳에서 전송량이 제한됐다. 현재 설정은 위 후속 변경 문서를 따른다.

- ESP `src/main.cpp`: HTTPS 한 요청에 최대 4샘플, 성공 후 250 ms 대기.
- 서버 `gateway/relay_samples.py`: `MAX_BATCH_SAMPLES=4`, 요청 크기 제한도 존재.

따라서 지연을 무시해도 현 HTTPS 경로는 약 16샘플/초가 상한이다.
이 두 곳과 큐 처리량을 함께 바꿔야 100 Hz 기록을 전달할 수 있다.
이번 변경에서는 UART 송수신 형식과 baudrate를 변경했고, 웹·서버 전송 정책은 유지했다.

## 검증

- 새 C/C++ 호스트 검사: 실제 STM 송신 함수 → 실제 ESP 수신 함수로 값 복원,
  부호·배율·64비트 시각·CRC, UART busy 버퍼 수명, TX 실패/건너뛰기, 중복/순번 wrap,
  수신 타임아웃, 연속 패킷, 제어 명령 9바이트 호환성.
- 238바이트 전체의 **1904개 단일 비트 손상**을 각각 검사했다.
  각 바이트 위치의 삭제/삽입과 잘린 패킷 뒤에 정상 패킷을 붙여 복구를 확인했다.
  C/C++ 테스트에 undefined-behavior sanitizer를 사용했다.
- ESP JSON에 최대 48셀 BMS 및 최대값 시간/순번 메타데이터를 넣어 JSON 유효성과
  게이트웨이 보존을 검사했다.
- 관련 UART·ESP·게이트웨이·릴레이·기록 회귀 **67개 통과**.
  전체 discovery에 포함된 별도 과거 STM 검사 10개는 Documents/TV 경로,
  미포함 DJY_TQV 고정 git 리비전 또는 과거 소스의 GCC 경고에 의존하여 이 67개에서 제외했다.
  현재 workspace STM 송신은 새 테스트와 실제 ARM 빌드로 검증했다.
- 후방 STM32F446: ARM GCC 14.2.1, 기존 Debug 대상의 48개 소스 전체 빌드/링크 통과, 경고 0.
  text=72576, data=180, bss=11104 bytes.
- ESP32-S3: PlatformIO espressif32 7.0.1 실제 빌드/링크 통과.
  예제 설정과 HTTPS/제어 명령을 활성화한 테스트 설정을 각각 검증했다.
  테스트 빌드는 실제 Wi-Fi 비밀값을 사용하지 않았다.
- 보드 플래시와 모터 구동 중 실측 통신 검증은 수행하지 않았다.

핵심 호스트 검사는 `DJY_EvTLMT-main/`에서 재실행할 수 있다.

```sh
python3 -m unittest discover -s tests -p 'test_binary_telemetry.py' -v
python3 -m unittest discover -s tests -p 'test_esp_bms_packet.py' -v
```
