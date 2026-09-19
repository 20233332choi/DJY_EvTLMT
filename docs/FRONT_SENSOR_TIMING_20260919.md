# 전방 센서 취득 지연 실측 — 2026-09-19

전방 STM32에 준비된 시각 동기화 코드와 센서 취득 계측을 적용했다. ST-Link `066EFF495150785187181947`, VCP COM3, STM32F446RE, 공급 전압 3.25 V를 확인했고 Flash 쓰기/검증에 성공했다. 후방 STM/ESP는 사용자에 의해 분리된 상태다.

## 실측 결과

60초 동안 각 100 Hz 취득 주기의 원시 기록 6,111개를 수집했다. 첫 수집 시각 이후 1초를 제외한 정상 구간 6,011개를 분석했다. 수집 구간 내 sequence 누락 0개, 보드 출력 큐 overflow 0개. 포트를 여는 순간의 잘린 첫 줄 하나는 제외했다. 모든 단위는 전방 TIM5의 **명목 마이크로초**이다.

| 항목 | 평균 | P95 | 최대 |
|---|---:|---:|---:|
| 취득 시작 주기 | 10,000 µs | 10,000 µs | 10,000 µs |
| 조향 AS5147 읽기: SPI 명령/응답 및 검사 | 38 µs | 38 µs | 38 µs |
| TPS 읽기: ADC 8회 취득 및 평균 | 352 µs | 352 µs | 352 µs |
| 조향 시작 → TPS 완료 | 390 µs | 390 µs | 390 µs |
| 센서 ISR 작업 구간 | 398.10 µs | 399 µs | 399 µs |

P95는 개별 원시 표본의 nearest-rank 분위수다. 보드 시간 해상도는 1 µs여서 같은 값으로 양자화된 결과이며 서브마이크로초 jitter가 없다는 뜻은 아니다. `work_us`는 마지막 진단 큐 복사와 ISR 반환 비용을 제외한다. 측정 구간이 10 ms를 넘은 경우는 0개다.

조향 protocol/parity 오류 0, 조향 SPI HAL 오류 0, TPS ADC HAL 오류 0, TPS 범위 오류 0. 원시값 범위는 SAS 8842~8885, TPS 835~904였다. 이 값만으로 실제 물리 회전량이나 페달 움직임을 검증한 것은 아니다.

CAN pair enqueue 성공 0개, 실패 6,011개였다. 마지막 CAN ESR은 `0x00B00033`: ACK error, error-passive 상태다. 현재 후방/ESP를 분리한 구성에서 전방의 기존 자동 재전송과 미완료 메일박스가 유지되는 결과와 부합한다. **센서 읽기 실패가 아니며 CAN 전달 성공/지연은 측정하지 못했다.** 후방 연결 상태의 CAN 송신 시간이나 두 보드 시계 동기화 검증으로 해석하면 안 된다.

## 코드 근거

프로젝트: `C:\Users\PARK\Documents\TV\stm_front`.

- `Core/Src/main.c`, `HAL_TIM_PeriodElapsedCallback`: `SAS_ReadAngle()` 전후, `TPS_ReadRaw()` 완료 시각을 TIM5로 기록. 센서 읽기 순서는 SAS → TPS로 유지했다. 두 센서는 동시 취득이 아니다.
- `Core/Src/sas_sensor.c`: AS5147 ANGLECOM 0x3FFF를 두 번 전송하여 파이프라인 응답을 읽고 parity/error를 검사한다. SPI는 16비트, APB2/64 = 명목 1.3125 MHz. 두 프레임의 순수 32클록은 약 24.38 µs이며, 측정 38 µs에는 CS 처리와 HAL/검사 비용이 포함된다.
- `Core/Src/tps_sensor.c`: 단일 변환을 Start → Poll → GetValue → Stop 순으로 8회 실행한다. 기존 주석의 약 37 µs는 `(84+12)/21 MHz × 8`의 순수 변환 시간이다. 실제 352 µs를 대신하지 않는다.
- `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc.c`, `HAL_ADC_Start`: ADC가 꺼져 있으면 ADON을 설정하고 안정화 busy loop를 실행한다. `HAL_ADC_Stop`은 매번 ADC를 끈다. 그러므로 8회 모두 안정화 대기와 HAL 비용이 들어간다. 현재 Debug `-O0` 빌드의 결과다. 각 내부 단계별 소요 시간은 이번 계측에서 별도로 분해하지 않았다.
- `Core/Src/front_timing.c`: ISR에서 256개 ring buffer에 기록하고 메인 루프에서 USART2 interrupt 송신. 문자열 생성이나 USB 송신 완료 대기를 센서 ISR에서 수행하지 않는다. HAL 상태 getter를 추가했으며 센서 값 계산/판정, 100 Hz 주기, 기존 CAN 자동 재전송은 변경하지 않았다.

## 지연 해석과 한계

전방 취득 계산 비용 0.390 ms는 10 ms 주기의 약 3.9%다. 이번 관측 범위에서 이 취득 작업은 주기를 넘지 않으며, 이전 후방의 최대 약 32 ms IMU 파싱 지연보다 작다. 다만 이는 서로 다른 센서·서로 다른 연결 조건의 시험이다.

**물리적 조향/페달 변화 → 전방 보드 값 갱신 전체 지연을 38/352 µs라고 하면 안 된다.** 두 값은 센서 읽기 함수 실행 시간이다. 외부 변화가 폴링 직후 발생하면 다음 100 Hz 취득까지 최대 약 10 ms의 대기가 추가될 수 있다. 이를 고려한 단순 소프트웨어 대기+취득 예산은 약 10.39 ms이며 실측된 물리 응답 최대값이 아니다. TPS는 8개 시점의 평균값이므로 단일 시점 샘플도 아니다.

AS5147 내부 각도 계산/보정과 아날로그 TPS 센서 자체 응답 지연은 측정하지 않았다. 물리적 입력의 기준 신호 또는 기준 엔코더와 SPI/ADC 측정 신호를 동시에 관측해야 이를 분리할 수 있다. 현재 USB 장치만으로는 그 기준 시각을 알 수 없다. 센서 제조사 [AS5147 제품 정보](https://www.infineon.com/part/AS5147)의 내부 보정 사양도 이번 물리 실측값으로 대체하지 않았다.

전방은 HSI 기반이다. 원시 로그 양 끝에서 전방 시간 61.100 s 동안 PC 수신 시각은 약 59.989 s 경과해 약 1.85% 차이가 있다. 이는 USB 수신 시각을 이용한 장기 비교이며 오실로스코프 기반 절대 교정은 아니다. 향후 두 보드 동기화의 skew 추정은 상대 시계 차이를 다루지만 절대 SI 시간 교정과는 별개다.

후방 STM/ESP가 분리되어 있으므로 대시보드에 이 전방 진단이 전달되지는 않는다. 이번 원본은 USB로 직접 수집했다.

## 검증 및 재현

- GCC 빌드 성공: text 27456, data 100, bss 14176 bytes.
- Flash verify 성공. 적용 ELF SHA256: `F2C2D91AFDAA30AC2C3542444793CD997CDC85D602E085970C12D517B594A91A`.
- 출력 큐 가득 참/순서/wrap/비동기 송신 버퍼 수명 테스트 통과.
- 시계 모델·프레임 pairing·실제 CAN 프로토콜 mock 테스트 3개 통과. 프로토콜 harness는 NDEBUG를 해제해 assert가 활성화되도록 보완했다.
- 적용 전 Flash 512 KiB 및 Core/Debug 백업: `C:\Users\PARK\telemetry-audit-20260919\before-front-sensor-timing`.
- 원본 계측: `C:\Users\PARK\telemetry-audit-20260919\timing-front-sensors.json`.

```powershell
& C:\Users\PARK\telemetry-tools\python\python.exe -B scripts\measure_front_timing.py --port COM3 --seconds 60 --output C:\Users\PARK\telemetry-audit-20260919\timing-front-sensors.json
```

F1 CSV 필드: seq, started_us, period_us, sas_us, tps_us, acquire_us, enqueue_end_us, work_us, sas_raw, tps_raw, flags, can_esr(hex), queue_dropped. `started_us`는 32비트이므로 약 71.6분에 wrap한다. flags 비트 0~4는 각각 SAS protocol, TPS 범위, SAS HAL, TPS HAL, CAN pair enqueue 실패를 나타낸다. 최초 포트 열기 전의 부팅 표본은 이 파일에 포함되지 않는다.
