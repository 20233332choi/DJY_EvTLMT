# 실차 후방 STM 프로젝트

사용자가 별도로 제공한 `stm_back.zip`의 후방 소스입니다. 저장소의 과거 `firmware/tv_stm_esp/stm_back`과 제어·시각 동기화 코드가 달라 별도로 보관합니다. 기존 과거 프로젝트는 유지합니다.

원본 ZIP SHA-256: `776cd11708efc0933fc13e41c115057ebe2a27901e88491343fb2ab123e7e626`.
원본 반입 커밋에는 소스·CubeIDE/CubeMX 프로젝트만 포함하며 Debug/Release 산출물·개인 IDE 설정·실행 설정은 제외했습니다. 다음 변경 커밋에서 바이너리 송신과 타이밍 진단을 적용합니다.

## 적용과 빌드

후방 제어 주기는 기존 100 Hz를 유지하고 UART1 송신만 238바이트 CRC16 바이너리/460800 baud/최대 100 Hz로 변경했습니다. USART2 USB ASCII 진단은 115200 baud/최대 5 Hz입니다. ESP와 후방을 함께 업데이트해야 합니다.

저장소 루트에서 `./scripts/build_stm32_rear_uart.ps1 -BinaryRear`를 실행합니다. 출력은 `.local/stm32-rear-binary/`입니다. 스크립트의 기존 기본 빌드 대상은 과거 프로젝트로 유지하며, `-LegacyPinned`와 `-BinaryRear`는 동시에 사용할 수 없습니다. 이 프로젝트 자체를 STM32CubeIDE로 열어 빌드할 수도 있습니다.

이 PR의 원본 반입 커밋과 다음 기능 변경 커밋을 나눠 보면, 실차 제공본 대비 바뀐 후방 파일은 `main.c`, `timing_diag.c/.h`, `stm_back.ioc`, 새 `djy_telemetry_protocol.h` 5개입니다. IMU·안전·TV/ED·DAC의 원본 제어 코드는 유지했습니다. 제공본의 SD 디스크 I/O는 미구현 상태이므로 SD 기록 성공을 의미하지 않습니다.

[프로토콜 및 호환성](../../docs/BINARY_UART_KO.md) · [전체 수집 경로와 제한](../../docs/HIGH_RATE_TELEMETRY_KO.md)
