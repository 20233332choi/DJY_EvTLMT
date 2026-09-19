# STM → ESP → 핫스팟 무선 수신 검증 (2026-09-13)

## 현재 결과

사용자가 컨트롤러 구동 전원을 차단한 상태에서 수신만 시험했다. 이 후속 작업에서는
STM/Front 펌웨어를 다시 쓰거나 모터 구동·TV 활성화 명령을 보내지 않았다.

```text
Front ─ CAN ─ Rear PA9(TX) → ESP GPIO18(RX)
              PA10(RX) ← ESP GPIO17(TX): 현재 명령 차단
              GND ────── GND
                            │ Wi-Fi UDP 9003
                            ▼
               핫스팟 내부 PC → HTML /pit
```

- 사용자 재배선 직후, ESP 업데이트 전부터 UART 패킷 카운터가 564 → 588로 증가했다.
  배선 복구와 아래 무선 소프트웨어 수정을 구분한다.
- 당시 ESP IP `172.20.10.9`, PC Wi-Fi IP `172.20.10.7`. 이 주소는 재접속 시 바뀔 수 있다.
- 수정 후 8초 수신: ESP 주소에서 UDP 39개, 각 2,360~2,362바이트,
  정상 JSON 39개 / 해석 실패 0개.
- 대시보드 입력을 `STM_USB`에서 `WIFI_UDP`로 전환했다. USB는 전원용으로 유지했지만
  게이트웨이의 `usb_link_online=false`, `wifi_udp_link_online=true`로 무선 경로를 확인했다.
- 약 10초 관찰: 서로 다른 sequence 20개, 수신율 4.76~4.84Hz,
  `rear_uart_rx` 1,142 → 1,193, `rear_uart_bytes` 303,772 → 317,338,
  `rear_uart_errors=0`. 모든 관측에서 `stm_online=true`.

## 무선 미표시 원인과 수정

기존 설정은 로컬 UDP 비활성이었다. 이를 켠 뒤에도 Arduino NetworkUDP가 1,460바이트에서
송신 버퍼를 내보내면서 약 2.3KB JSON이 **서로 다른 UDP 메시지 두 개**로 나뉘었다.
실제 PC에서 1,460 + 906바이트를 관찰했다. 패킷은 도착했지만 둘 다 불완전한 JSON이었다.

`firmware/esp32_ev_gateway/src/main.cpp`에서 lwIP 소켓 `sendto()` 한 번으로 JSON 전체를
전송하도록 변경했다. IP 계층의 분할/재조립과 애플리케이션 UDP 메시지 분할은 다르다.
장시간·주행 환경의 손실률 검증은 아직 하지 않았다.

함께 반영한 수신 보완:

- Rear UART RX 버퍼 1,024바이트, 실제 초기화 결과 및 원시 바이트 카운터 노출.
- 약 5Hz STM 로그에 맞춰 텔레메트리 발행 간격 200ms.
  기존 20ms 발행은 115200 USB 출력 대역폭을 초과해 UART 처리 지연을 유발할 수 있었다.
- STM의 `rv=L/R`을 해석해 유효 RPM과 단순 보고 숫자를 분리. 누락된 `rv`는 유효로 추정하지 않음.
- `EV_RECEIVE_ONLY=1`: ESP의 USB/UDP/릴레이 제어 전달 차단.
- 무시되는 로컬 `config.h`: 로컬 UDP 켬, 인터넷 릴레이 및 릴레이 명령 끔.
  자격증명과 TLS 검증 설정은 완화하지 않음.

방화벽은 변경하지 않았다. 초기 숫자 열거형을 잘못 해석했지만 실제 Python 규칙은 Allow였다.
방화벽 변경 스크립트는 보호 검사에서 중단되어 규칙을 추가/수정하지 않았다.

## 센서 및 기록의 경계

| 항목 | 현재 관찰 |
|---|---|
| TPS | 원시값 895 → 893, 개도량 0%, 수신 상태 참 |
| 좌 RPM | 보고값 0, 유효성 거짓, NO_PULSES |
| 우 RPM | 보고값 0, 유효성 거짓, NOISY; 전기적 잡음 원인 해결은 별도 |
| 실제 RPM 그래프 | 양쪽 null. 보고값 0을 검증된 실제 0RPM으로 취급하지 않음 |
| IMU / 조향각 | 유효 수신 없음. 무선 복구가 센서 수신까지 보장하지 않음 |
| 제어 | gateway_control_enabled=false, live_control_allowed=false |
| DB | recording_active=false. 측정 시작 버튼을 눌러야 측정 세션 기록 시작 |

기존 Scope 버퍼를 `.local/esp-prewireless-20260913-scope-*.json`에 보존한 뒤 입력을 바꿨다.
이는 수신 중 페이지별 스냅샷이며 DB 측정 세션이 아니다.
최종 상태와 Scope 증거는 `.local/esp-wireless-20260913-telemetry.json`,
`.local/esp-wireless-20260913-scope.json`에 저장한다.

## 실행 / 적용 정보

- 같은 핫스팟에서 `scripts/run_ev_stack.ps1 -SkipDashboard`로 읽기 전용 무선 게이트웨이 실행.
- PC 대시보드: `http://127.0.0.1:8766/pit`.
- ESP 목적지 `EV_PIT_HOST`는 실제 PC Wi-Fi IPv4와 일치해야 한다. IP가 바뀌면 재설정 필요.
- **ngrok은 이번 경로에 사용하지 않았고 복구 완료로 판단하지 않는다.**
- ESP COM8에 `platformio-modern.ini`로 빌드/업로드, 기록 해시 검증 성공.
- 적용 firmware.bin SHA256:
  `7992AFB9C6D58DF40981E552289A1EDB3663F20A7D04F2333B2DBDC234B7E480`.
- 수정 전 백업 `.local/esp-before-uart-20260913.bin`은 주소 0부터 0x140000바이트만 읽은 것.
  전체 플래시 백업이 아니며 비밀 설정이 포함될 수 있으므로 공개하지 않는다.
- 호스트 테스트 56개 통과. 최초 샌드박스 실행의 로컬 HTTP WinError 10053은 권한 재실행에서
  재현되지 않았다. 호스트 테스트와 위 실제 수신 검증을 구분한다.
