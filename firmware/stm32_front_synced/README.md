# 실차 전방 STM 프로젝트

별도로 제공한 `stm_front.zip`의 소스·CubeIDE/CubeMX 프로젝트를 반입했다.
ZIP SHA-256: `8f757d674ca464773b3d0092eeb78aadbffa753250087a1d601985094c300d57`.
Debug/Release 산출물, 개인 `.settings`, `.launch` 파일은 제외했다.

`firmware/stm32_rear_binary`와 함께 사용한다. 두 보드의 `vehicle_clock`,
`board_time_sync`, `time_sync_model` 공통 소스 5개가 일치하는지 호스트 검사로 확인한다.
전방은 취득 구간 중간 시각과 sequence를 CAN 센서 프레임 쌍에 담고,
후방은 이를 짝지어 시간 진단에 사용한다. F1 USB 진단도 제공본대로 포함한다.

저장소 루트에서 `./scripts/build_stm32_rear_uart.ps1 -BinaryRear`를 실행하면
이 전방과 바이너리 후방을 함께 빌드한다. 출력은 `.local/stm32-rear-binary/`다.
과거 전방 `firmware/tv_stm_esp/stm_front`는 과거 후방용으로 유지한다.

이번 반입에서 SAS/TPS 읽기·스위치·CAN 주기·안전 판단 알고리즘은 변경하지 않았다.
센서 I/O 오류 처리 등 기존 미해결 항목이 해결됐다는 의미는 아니다.
