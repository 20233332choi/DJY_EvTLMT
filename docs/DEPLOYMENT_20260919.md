# 2026-09-19 차량 텔레메트리 배포

> 후속 작업에서 ngrok TLS 문제를 해결했다. 아래의 '초기 검증'은 해결 전 기록이다.
> 현재 결과 및 무료 요금제 제약은 문서 마지막 'ngrok 해결 및 실제 양방향 검증'을 참고한다.

## 실행

프로젝트 루트의 `start_telemetry.bat`를 실행한다. Python과 ngrok는
`C:\Users\PARK\telemetry-tools`에서 사용한다. PC와 ESP 모두 인터넷 연결이 필요하다.
현재 ESP Wi-Fi는 HyengmInPhone이며 같은 핫스팟의 PC 주소는 172.20.10.13이다.

- PC 대시보드: http://127.0.0.1:8766/pit
- 인터넷 대시보드: https://hatching-nutty-thicken.ngrok-free.dev/pit
- 차량 HTTPS 교환: 같은 도메인의 `/api/vehicle/exchange`
- 게이트웨이 로그: `C:\Users\PARK\telemetry-tools\gateway.log`, `gateway-error.log`
- ngrok 로그: `C:\Users\PARK\telemetry-tools\ngrok\agent.log`

인터넷 대시보드는 조회용이다. 공개 `/api/control` 요청은 거부하며,
차량 교환 API는 ESP와 PC만 공유하는 별도 Bearer 토큰을 요구한다.
ngrok 계정 인증 토큰과 차량 교환 토큰은 서로 다른 값이다.
토큰/비밀번호가 있는 config.h 및 ngrok.yml, ESP 바이너리/백업은 외부 공유하지 않는다.

## STM 소스 주의

실제 기존 플래시의 실행 코드와 일치한 소스는
`C:\Users\PARK\Documents\TV\stm_back`이다. 이 프로젝트를 수정·빌드·업로드했다.
처음 제공된 DJY_TQV 프로젝트는 DAC/차량 보정 설정이 달라 업로드하지 않았다.
Front STM은 이번 작업에서 수정하거나 업로드하지 않았다.

- PA4/PA5 내부 DAC 및 기존 차량 보정값 유지. vehicle_params.h의 SHA256이 백업과 동일하다.
- 기존 USB 전용 디버그 출력을 통합 확장 텔레메트리로 교체했다.
- USART1 PA9 TX → ESP GPIO18 RX에 115200, 5 Hz, 비동기 송신을 추가했다.
- 같은 문장을 USART2/ST-Link USB에도 출력한다. 송신 길이와 버퍼 수명을 보호한다.
- UART RX 복구가 TX까지 중단하지 않도록 HAL_UART_AbortReceive를 사용한다.
- IMU XYZ 가속도/유효성, RPM 좌우 유효성은 관측용으로 추가했다. 제어 필터는 변경하지 않았다.

## ESP 변경

- HTTPS relay와 응답 명령 수신 활성화. 인증서 검증을 유지한다.
- ngrok URL과 로컬 UDP 목적지 IP를 현재 환경에 맞췄다.
- USB JSON 출력에 TX 버퍼와 비차단 1 Hz 제한을 적용해 STM UART 수신 정체를 방지한다.
- 명령 만료 시간은 늘리지 않았다. 인터넷 통신은 실시간 제어 링크가 아니며 지연 시 명령이 만료될 수 있다.

## 안전 및 현재 전원 상태

사용자는 모터 구동 전원 OFF, 보드만 ON, Front 보드 OFF임을 확인했다.
Front가 꺼져 있으면 CAN 수신 0 및 fault_code=1(CAN timeout), TPS/SAS 미수신은 예상된 결과다.
IMU와 BMS도 실제 데이터가 들어오지 않으면 미수신으로 표시한다.

기존 STM은 ESP 명령이 끊기면 `TV_STRENGTH_NO_ESP=1.0`으로 돌아간다.
이는 기존 TV/ED 강도 설정이지 스로틀 100% 명령이 아니다. 그러나 통신 단절이
모터 정지를 뜻하지도 않는다. 이번 작업에서 해당 차량 제어 정책은 변경하지 않았다.
구동 전원을 켠 주행 시험 또는 출력 변경 시험은 수행하지 않는다.

## 복구 자료

`C:\Users\PARK\telemetry-audit-20260919`:

- `rear-current.bin`: 업데이트 전 STM 전체 512 KiB 플래시
- `stm-back-before-update.elf`: 업데이트 전 STM ELF
- `stm-back-before-update`: 업데이트 전 STM Core 소스
- `observe_serial.py`: 명령을 보내지 않는 USB 관측 도구
- `check_firmware_ca.py`: ESP에 내장된 CA만으로 TLS 1.2 검증
- `verify_relay.py`: 실제 텔레메트리와 모터 출력에 관여하지 않는 relay_ping 검증

## 초기 검증 기록 (아래 후속 수정 전, 당시 인터넷 직접 연결 미해결)

- STM 빌드/업로드/플래시 검증 성공. USB에서 5초 동안 26개의 정상 텔레메트리 문장 확인.
- ESP 첫 1 MiB 부분 백업 성공 (첫 Arduino 2.x 업데이트 기록 범위는 포함):
  `esp-before-update-first1MB.bin`, SHA256
  `77B604CDB73D0951AA8EEF6700A15AFDEF07CEB6893E7E58DCED78D2D6EE3C32`.
  전체 16 MiB 백업이 아니며 최종 Arduino 3.x 기록 범위 전체도 포함하지 않는다.
  이 파일만으로 원래 전체 플래시 복원을 보장할 수 없다.
  16 MiB/2 MiB 읽기는 USB 전송 오류로 실패했고,
  esptool 4.11, 115200 baud, 1 MiB 읽기는 성공했다.
- ESP Arduino 2.0.17 빌드·업로드 및 모든 기록 구간의 해시 검증 성공.
- 실제 STM→ESP UART 수신 오류 0, 정상 프레임 수 증가 확인.
- 같은 핫스팟에서 ESP→PC UDP 약 4.84 Hz 확인. 이후 Front가 켜지며
  TPS 약 890, SAS 약 6700, IMU 유효 데이터를 수신했다.
- Python 게이트웨이 33개, STM 텍스트 9개, 튜닝 8개, BMS 3개 테스트 통과.
- PC에서 공개 `/health` HTTP 200 및 ESP 내장 CA만 사용한 TLS 1.2 인증서 검증 성공.
- 단, ESP 직접 HTTPS 연결은 HTTP -1 / TLS -29312(EOF), 성공 전송 0.
  ALPN http/1.1 추가 후에도 재현된다. `relay_ping`는 릴레이가 오프라인이라 전송하지 않았다.
- PC가 CHOSUN_WiFi로 돌아가면 로컬 UDP는 끊기며 ngrok 공개 주소 접속도 시간 초과된다.
  이 현상은 ESP 자체의 TLS EOF와 분리해서 봐야 한다.
- 최종 Arduino 3.3.11 / pioarduino 55.03.311 빌드·업로드 및 모든 기록 구간 해시 검증 성공.
  RAM 52,816 B, 앱 이미지 1,086,896 B. 앱 기록 주소 0x10000~0x1195AF.
  SHA256: `4B7367B2FD74D78B316AA0A7ECE2B995445FBE8EA75C17662ECEABCD8D50C415`.
- 최종 ESP에서도 TLS -29312 EOF 재현: 부팅 후 30초에 정상 UART 144프레임,
  UART 파싱 오류 0, TLS 실패 4회, HTTPS 전송 성공 0회.
  DNS 13.114.174.247, TCP 443 연결 성공, NTP 시각 설정 완료를 확인했다.
  PC TLS 인증 성공과 ESP TLS 실패만 확인했으며, 서버가 연결을 끊는 근본 원인은 확정하지 못했다.
- 최종 ESP 바이너리: `C:\Users\PARK\telemetry-tools\esp-build-modern\esp32-s3-devkitc-1\firmware.bin`.
- `platformio-modern.ini`의 `.p`/`.c`는 긴 경로 제한, 영문 build_dir는 한글 링커 경로 오류를 피한다.
  시스템 레지스트리나 Windows 긴 경로 정책은 변경하지 않았다.
- 최종 펌웨어에서 LED는 최근 5초 내 HTTPS 성공이 있을 때만 초록색이며, 이후 실패하면 다시 노란 점멸로 돌아간다.

재빌드/업로드 명령 (ESP 프로젝트 폴더에서, 구동 전원 OFF 확인 후):

```powershell
$env:PYTHONUTF8='1'
$env:PYTHONIOENCODING='utf-8'
& "$env:USERPROFILE\telemetry-tools\python\python.exe" -m platformio run -c platformio-modern.ini -t upload --upload-port COM12
```

Arduino 2.x 기본 platformio.ini 대신 최종 배포에 사용한 platformio-modern.ini를 명시한다.
다른 HTTPS 중계 서비스로 전환하려면 별도 사용자 확인이 필요하다. 이번 작업에서는 전환하지 않았다.

노란 LED 점멸은 Wi-Fi 연결 후 HTTPS 릴레이 재시도 상태이다.
모터 출력 변경 명령 또는 구동 시험은 전송하지 않았다.

## ngrok 해결 및 실제 양방향 검증

- 기존 문제: DNS/TCP 및 인증서 검증은 성공하지만, TLS 상태 12
  (`SERVER_CHANGE_CIPHER_SPEC`)에서 EOF(-29312) 발생. 기본 ECDSA 체인의
  인증서/키 연산은 보드에서 약 3초가 걸렸다.
- PC MemoryBIO 비교 시험: TLS 1.2의 두 번째 클라이언트 메시지를 즉시 또는
  0.25/0.5/0.75초 지연하면 성공했고, 1/2/3초 지연하면 EOF가 재현됐다.
  이는 이번 엔드포인트에서 측정한 결과이며 ngrok 전체 서비스의 보장된 제한을 뜻하지 않는다.
- ESP에서 ECDHE-RSA + AES-128-GCM, P-256/P-384 그룹으로 인증서 검증을 유지한
  핸드셰이크 약 0.95초 및 `/health` HTTP 200을 확인했다.
- `include/ngrok_tls_client.h`: Arduino 3.x의 기존 TLS 구현에 강한 RSA 인증서
  기반 ECDHE/AES-GCM 조합을 설정한다. 순방향 비밀성, CA/호스트명 검증 유지.
  짧은 핸드셰이크 동안만 태스크 우선순위 1을 사용하고 원래 값으로 복구한다.
  HTTP 또는 인증서 검증 우회로 폴백하지 않는다.
- `platformio-modern.ini`에 `EV_RELAY_FAST_RSA_TLS=1` 적용.
  일회성 TLS 진단은 운영 빌드에서 비활성화했다. SDK 자체는 수정하지 않았다.
- ESP HTTP 클라이언트와 인증 성공한 게이트웨이 차량 교환 응답만 연결을 재사용한다.
  Wi-Fi/HTTP 실패 시 소켓을 버리고 재접속하며 재시도 간격은 1~5초.
  서버의 유휴 keep-alive는 5초로 제한한다. 거절 응답/다른 API는 연결을 닫는다.
- PC 게이트웨이를 `start_park_telemetry.ps1 -InternetOnly`로 재시작했다.
  이 옵션은 **새로 시작하는** 게이트웨이의 UDP를 127.0.0.1에만 바인딩한다.
  기존 프로세스의 모드를 바꾸지는 않는다. 현재 시험은 USB 입력도 사용하지 않았다.
- 실제 상태: `telemetry_transport=INTERNET_RELAY`, `wifi_udp_link_online=false`,
  `usb_link_online=false`, `stm_online=true`, 수신 약 2.2 Hz, UART 파싱 오류 0.
- `verify_relay.py` 통과: 실제 STM→ESP→ngrok HTTPS→PC 텔레메트리,
  PC→ngrok 응답→ESP의 무해한 `relay_ping` 수신 확인(ACKED),
  공개 대시보드 데이터 확인, 공개 제어 API 403 및 무인증 차량 교환 API 401.
  ping은 STM으로 전달하거나 모터 출력을 변경하지 않는다.
- 게이트웨이 단위 테스트 34개 통과. 인증된 연속 요청의 소켓 재사용과
  무인증 요청의 연결 종료 테스트를 추가했다.
- 터널 프로세스를 의도적으로 중단하고 재시작하는 복구 시험 수행.
  120.7초 관측 중 인터넷 교환 225회 증가, 의도적 중단을 포함해 offline 표본 15개,
  재연결 과정 오류 4회 증가 후 고정, UART 오류 증가 0.
  복구 후 약 2.1~2.2 Hz 수신을 유지했고 양방향 ping/외부 접근 차단 시험도 재통과했다.
  이는 약 2분의 복구 시험이며 하루/장기간 안정성 검증을 대신하지 않는다.
- 운영 ESP 이미지: 1,087,184 bytes, SHA256
  `B29AC2D3DE591EA0E3A0D0230DF669778145D4EF80033508CCC241488A2C5683`.
  업로드 후 모든 기록 구간 해시 검증 통과.
- PC와 ESP는 시험 시 같은 휴대폰 핫스팟에 있었지만 로컬 UDP 수신을 제외하여
  실제 공개 ngrok 경로만 검증했다. 물리적으로 서로 다른 ISP에서의 시험은 별도다.
  chosun_wifi에서는 PC의 ngrok HTTPS 접속 실패를 확인했으므로
  운용 PC도 ngrok에 도달할 수 있는 인터넷을 사용해야 한다.

### 무료 요금제와 아직 완료되지 않은 장기 운용 항목

사용자가 무료 요금제임을 확인했다. 공식 제한은 월 HTTP 20,000회 및 전송 1 GB다.
<https://ngrok.com/docs/pricing-limits/free-plan-limits>
keep-alive는 TLS 비용을 줄일 뿐 HTTP 요청 사용량은 줄이지 않는다.
현재 약 2.2 Hz가 지속되면 ESP 교환만으로도 20,000회는 약 2.5시간 분량이다.
휴대폰 GNSS/공개 대시보드 요청 등은 이 한도를 더 사용한다.
따라서 현재 HTTP 방식으로 무료 무제한 상시 운용을 보장할 수 없다.
장기 운용에는 요청을 줄이는 전송 방식 및 트래픽 예산 또는 적합한 요금제 선택이 필요하다.
요금제 변경/결제는 수행하지 않았다. Windows 자동 시작/서비스 설치도 아직 하지 않았다.
휴대폰 GNSS의 실제 전송과 모터 출력 동작 시험은 이번 ngrok 검증 범위에 포함하지 않았다.
