# DJY EV Telemetry

STM·ESP·BMS·휴대폰 GPS 데이터를 수신하고 저장하는 웹 텔레메트리입니다.

## 실행

`start_telemetry.bat`을 실행하면 텔레메트리 서버와 ngrok을 시작하고 브라우저를 엽니다.
현재 PC에 설정된 Python·ngrok 및 ESP 릴레이 설정을 사용합니다.
실행 BAT는 `start_telemetry.bat` 하나만 사용합니다. 실행 시 프로젝트 경로를 표시하며, 다른 폴더의 서버가 실행 중이면 혼용하지 않고 중단합니다.
USB 직접 수신은 PowerShell에서 `./scripts/run_ev_stack.ps1 -Wired -RearPort COM13`, BMS 벤치 연결은 `./scripts/run_bms_pit_dashboard.ps1 -Port COM5`를 사용합니다. 포트는 실제 연결에 맞춥니다.

| 화면 | 주소 |
|---|---|
| 실시간 피트 대시보드 | `http://127.0.0.1:8766/pit` |
| BMS | `http://127.0.0.1:8766/battery` |
| 저장 데이터 분석·CSV | `http://127.0.0.1:8766/records` |
| 지도·출발선·랩타임 | `http://127.0.0.1:8766/map` |
| 휴대폰 GPS 연결 안내 | `http://127.0.0.1:8766/phone` |
| 운전자 화면 | `http://127.0.0.1:8766/driver` |

인터넷에서는 ngrok 도메인 뒤에 같은 경로를 사용합니다.
기록 시작·종료와 출발선 설정·랩 측정은 인터넷에서도 가능합니다.
차량 제어 명령과 GPS 기준점 보정에는 기존 로컬 접근 제한이 적용됩니다.

## 저장·분석

- 측정 시작부터 수신 데이터를 `data/ev_telemetry.db`에 저장합니다.
- 분석창은 세션 전체를 불러오며, 카테고리 체크박스·구간 확대·정확한 표본값·CSV 내보내기를 제공합니다.
- 전력은 W/kW로 전환할 수 있으며 저장 원본은 유지합니다.
- BMS 팩 전력은 전압×전류입니다. 모터 출력 요구값은 실측 출력과 구분합니다.
- GPS가 함께 저장된 표본은 지도에서 위치와 전력을 확인할 수 있습니다.
- 아이폰 백그라운드 GPS는 Traccar Client와 `/api/gnss/native`를 사용합니다.
- 출발선의 첫 정방향 통과에서 타이머가 시작되고 다음 정상 통과에서 랩을 완료합니다.

## 프로젝트 구조

```text
start_telemetry.bat   서버·ngrok 시작
gateway/              Python 수신·저장·웹 서버
web/                  현재 사용하는 웹 화면과 지도 라이브러리
firmware/             STM·ESP 펌웨어 및 통신 정의
bin/                  보존된 ESP 펌웨어 바이너리
data/                 실제 주행 기록 DB
scripts/              실행·계측·검증 도구
tests/                서버·통신·펌웨어 검증
docs/                 기능 안내와 과거 진단 기록
_archive/             이전 작업물 보관 (Git 제외)
```

C++/ImGui 대시보드와 구형 기록 HTML은 작업 경로에서 제외했습니다.
`native`, `ev-direct`, `rebuild` 실행 모드는 더 이상 제공하지 않습니다.
STM·ESP 펌웨어의 C/C++ 소스는 차량용이므로 유지합니다.
과거 진단 문서에는 당시 구성과 경로가 남아 있을 수 있습니다.

## 상세 안내

- [기록 분석](docs/RECORD_ANALYSIS_20260920.md)
- [BMS 전력 화면](docs/BMS_POWER_DASHBOARD_20260920.md)
- [아이폰 GPS](docs/IPHONE_BACKGROUND_GPS_20260920.md)
- [STM·ESP 통합 안내](firmware/tv_stm_esp/README.md)
- [배선도](docs/TORQUEVECTORING_PINMAP.svg)
