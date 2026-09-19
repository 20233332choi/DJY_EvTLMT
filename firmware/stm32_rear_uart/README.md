# Rear STM32 ↔ ESP32 양방향 UART / RPM 수정

> 이 폴더는 과거 오버레이입니다. 현재 기본 빌드는 [tv_stm_esp](../tv_stm_esp/README.md)를 사용합니다.
> 아래 과거 빌드를 재현할 때는 스크립트에 `-LegacyPinned`를 추가하십시오.

이 폴더는 **보드에서 복구한 C 소스가 아니다.** `DJY_TQV`의 커밋
`ee9d295bae733b27dc93f7dd4871d38d284e9732`를 기준으로 만든 최소 변경 후보이다.
2026-09-13 RPM 수정 전에는 실제 보드의 실행 이미지와 UART 확장 빌드의 동일성을
읽기 전용 덤프로 확인했다. **그 이후 RPM 수정도 구동 전원 차단 확인 후 Rear STM에 적용했고,
읽기 검증 및 유선 수신 확인을 완료했다.** 실제 모터 회전 RPM의 정확도 검증은 별도다.

RPM 수정 내용·시험·백업·적용 전 조건은
[2026-09-13 RPM 수정 기록](../../docs/STM32_RPM_FIX_20260913.md)을 참고한다.

## 변경 범위

- `Core/Src/main.c`: 기존 로그에 IMU와 TQV 내부 상태를 추가하고, 신선한 ESP 강도 명령을 기존 설정 입력 앞단에 연결한다.
- `Core/Src/telemetry_uart.c`, `Core/Inc/telemetry_uart.h`: USART1 PA9/PA10, 115200 8N1, 3.3 V TTL 양방향 통신.
- `Core/Inc/djy_uart_protocol.h`: CRC-8, 0~100%, 5% 단위, enable 비트가 있는 고정 9바이트 명령 형식.
- `Core/Src/control_settings.c`: 기존 강도 램프와 피트 최대값을 유지하면서 ESP 명령 입력만 추가한다.
- `Core/Src/rpm_sensor.c`, `Core/Inc/rpm_sensor.h`: 연속 고속 잡음의 가짜 RPM 방지,
  정상 주기 2회 확인, 좌우 유효성 및 최근 캡처 활동 조회. 로그 끝의 `rv=좌/우`는 RPM 유효성이다.
- 피트 정차 설정 조건에는 잡음을 포함한 최근 RPM 캡처가 없다는 조건을 추가했다.
- 기존 USART2/ST-Link 로그, 센서 입력, 안전 판정, 물리 TV 스위치, `TV_Update()` 계산 및 DAC 출력 경로를 유지한다.
- UART 송신은 인터럽트와 512바이트 정적 버퍼를 쓴다. 바쁘면 샘플을 버리며 ESP를 기다리지 않는다.
- ESP 명령은 500ms 동안만 유효하다. 끊기면 원래 Front/CAN 설정 입력으로 복귀하며, 그것도 오래됐으면 기존 램프로 0%가 된다.
- 다음 `torque_vectoring.h` 상태를 5Hz로 보낸다: `vehicle_speed`, `desired_yaw`, `yaw_error`, `delta_power`, `power_left`, `power_right`, `tv_active`, `ed_active`, `traction_scale`.
- BENCH_TEST_MODE=0, CAN_SNIFF_MODE=0을 유지한다. 가상 RPM을 만들지 않는다.

`torque_vectoring.c`와 `torque_vectoring.h` 자체는 수정하지 않는다. TQV 강도는 기존
`TV_SetStrength()` 입력만 바뀌고 물리 TV 스위치와 Rear 안전 조건을 우회하지 않는다.

## 빌드

저장소 루트에서 실행한다.

```powershell
.\scripts\build_stm32_rear_uart.ps1 -LegacyPinned
```

기본적으로 옆 폴더 `DJY_TQV`에서 위 고정 커밋을 읽는다. 해당 저장소의 미커밋 변경은 사용하거나 수정하지 않는다.
다른 위치라면 `-SourceRepository <경로>`를 지정한다.
ARM 도구는 기존 PlatformIO의 `toolchain-gccarmnoneeabi` 패키지를 사용한다.
HAL/CMSIS/FatFs 및 나머지 기준 소스는 Windows 임시 폴더로 추출한다.
완성된 ELF/BIN/HEX/MAP은 `.local/stm32-rear-uart/`에도 복사한다. 자동 플래시 기능은 없다.

## 후보 적용 시 배선

| STM32 Rear | ESP32-S3 | 기능 |
|---|---|---|
| PA9 / USART1_TX | GPIO18 / RX | 센서·IMU·TQV·출력 상태 |
| PA10 / USART1_RX | GPIO17 / TX | TQV 강도 명령 |
| GND | GND | 공통 신호 기준 |

2026-09-13 수정 전 보드에서 PA9/PA10 UART 활성화를 실제 확인했다. 이번 RPM 후보도
위 UART 설정을 유지한다. STM 내부 송신과 ESP GPIO18까지의 실제 신호 도달은 별도 검증이다.
전원 출력끼리 연결하지 않는다. CAN/RS232/RS485 신호를 이 TTL UART에 직접 연결하지 않는다.

USART1 확장이 없는 팀 원본을 사용할 때는 USART2 PA2 TX를 ESP GPIO18에 연결하는
단방향 대안이 있다. 이 경우 ESP GPIO17은 분리하고 GND만 공통으로 둔다.
PA2의 실제 보드 커넥터 위치와 다른 장치의 점유 여부는 배선 전에 확인해야 한다.

## 적용 전 남은 판단

과거 벤치에서 구동 전원/출력 분리를 확인받았지만, 플래시 직전에는 다시 확인해야 한다.
키 OFF만으로 물리적 분리를 확인했다고 판단하지 않는다. 소스 시험 및 이미지 비교는
실제 RPM 정확도, 배선 건전성 또는 실차 구동 안전성을 입증하지 않는다.

검사 증거와 백업 위치: [STM32/ESP 검사 기록](../../docs/STM32_ESP32_AUDIT_20260910.md).
