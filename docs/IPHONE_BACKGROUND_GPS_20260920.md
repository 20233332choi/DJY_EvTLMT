# 아이폰 백그라운드 GPS 연결

## 구현 및 사용

사용자 휴대폰은 아이폰이다. Safari/PWA의 `watchPosition`을 화면 잠금 중 실행한다고 가정하지 않는다. 백그라운드 위치를 지원하는 Traccar Client 앱의 HTTP 전송을 게이트웨이가 직접 받는다. 별도 Traccar 서버는 필요 없다.

1. https://apps.apple.com/app/id843156974 에서 Traccar Client 설치.
2. https://hatching-nutty-thicken.ngrok-free.dev/phone 에서 서버 주소 복사.
3. 앱 Server URL을 `https://hatching-nutty-thicken.ngrok-free.dev/api/gnss/native`로 설정. Device identifier는 앱 기본값 유지.
4. 아이폰 위치 권한은 항상 허용/정확한 위치, 앱 Location accuracy Highest, Interval 1초, Stop detection 끔. 실제 주기는 OS/신호 상태에 따라 달라진다.
5. 앱 서비스를 시작한 뒤 화면 잠금. 강제 종료하지 않는다. 다른 기기에서 /phone의 앱 수신 카운터가 증가하는지 확인한다. 종료는 앱에서 서비스를 끈다.

웹 GPS와 앱 GPS는 동시에 사용하지 않는다. 기존 브라우저 보정은 다른 source_id로 저장되어 앱 GPS에 자동 적용되지 않는다. 기존 랩 측정이 다른 GPS 소스로 진행 중이면 종료한 뒤 앱 GPS로 다시 시작한다.

## 데이터 경로

`Traccar Client → HTTPS OsmAnd POST/GET → native_gnss.decode → GNSSManager → 지도/랩타임/TelemetryDatabase`

- `application/x-www-form-urlencoded` POST 및 기존 GET query를 지원.
- 앱 SDK의 원본 전송 구현을 확인: id/lat/lon/timestamp/accuracy/altitude/speed/bearing. speed는 knots이므로 ×1.852로 km/h 변환. 초·밀리초·시간대 포함 ISO 시각 지원.
- 장치 ID에서 안정적인 해시 source_id를 생성한다. 공개 상태 응답에 원래 장치 ID를 노출하지 않는다.
- 정확도 누락, 무효 좌표, 중복·역순, 오래된 위치를 정상 위치로 위장하지 않는다. 기존 GNSS 품질/랩 판정 유지: 3초 초과 지연은 랩에 사용하지 않으며 30초 초과 오래된 위치는 거부한다.
- 품질 거부에도 HTTP 200과 `accepted:false/reason`으로 응답하여 앱의 오프라인 전송 대기열이 같은 오래된 표본에서 무한 재시도하지 않도록 한다. 따라서 오프라인 장기 기록 전체를 복구하는 기능은 아니다.
- /api/gnss/native/status와 /phone에 수신/채택/제외 건수, 최근 수신 시각 표시.
- 유효한 수신은 기존 텔레메트리 기록 상태를 따른다. 브라우저 열림 여부와 독립적이다. 전화기 앱의 실행/권한 부여는 사용자가 해야 한다.

## 검증 및 적용

- 변환/단위/시간/누락값 검증, 실제 HTTP form POST 및 GET → 지도 모델 → DB 저장 테스트 2개 통과.
- 기존 GNSS 테스트 7개, 지도·전화 페이지 Edge 브라우저 테스트 통과. 웹 GPS 페이지를 닫은 뒤 앱 형식 POST가 수신되는 경로와 설정 주소/모바일 레이아웃도 검증했다.
- 실제 iPhone 화면 잠금 중의 지속 수신은 앱 설치·권한 설정 후 실기기 검증 필요. 서버의 native 상태는 앱 대기 중이었다.
- 운영 DB 백업 `C:/Users/PARK/telemetry-audit-20260920/before-native-gps.db` (기존 1,913표본). 적용 전 데이터 기록 및 랩 측정은 비활성이었고 차량 제어 명령도 비활성임을 확인했다.
- 게이트웨이 재시작 후 ngrok 연결이 잠시 복구 중이었으나 재확인에서 /phone 및 /api/gnss/native/status 모두 HTTP 200. 기존 저장 세션을 임의로 새로 시작하지 않았다.

## 근거

- W3C Geolocation: https://www.w3.org/TR/geolocation/ — 숨김 문서의 위치 업데이트 제약.
- Traccar Client: https://www.traccar.org/client/
- 설정: https://www.traccar.org/client-configuration/
- 프로토콜: https://www.traccar.org/osmand/
- SDK 실제 전송: https://github.com/traccar/traccar-client-sdk/blob/main/core/src/commonMain/kotlin/org/traccar/client/HttpUploader.kt
- Apple 위치 권한: https://support.apple.com/en-us/102515

외부 앱/프로토콜과 연동하는 자체 어댑터이며 Traccar 소스 코드를 복사해 배포하지 않았다.
