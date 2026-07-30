# EV_ErgMt

Assetto Corsa telemetry를 Formula E 스타일의 EV 전력 신호로 변환하고,
기존 `fsk-energymeter` Data Viewer에서 읽을 수 있는 바이너리 `.log` 파일을 생성합니다.

## Assetto Corsa에서 직접 기록

1. Assetto Corsa를 실행합니다.
2. 아래 캡처 프로그램을 실행합니다. 게임보다 먼저 실행해도 자동으로 기다립니다.

```powershell
cd F:\PROJECT\EV_ErgMt
.\start_ac_capture.ps1
```

3. 차량과 트랙을 선택하고 주행을 시작합니다.
4. 주행 세션을 종료하면 3초 후 `sessions\AC_EV_날짜_시간.log`로 자동 저장됩니다.
5. `.\start_viewer.ps1`을 실행하고 저장된 로그를 선택합니다.

Docker로 뷰어를 실행하려면 다음을 사용합니다.

```powershell
.\start_docker_viewer.ps1
```

캡처 프로그램은 `ASC_TLMTSYS`와 같은 Assetto Corsa 공유 메모리를 사용합니다.
속도, 스로틀, 브레이크뿐 아니라 ERS/KERS 회수 상태를 회생제동 계산에 반영하며
FSK 에너지미터와 같은 100 Hz로 기록합니다.

## 바로 확인하기

```powershell
cd F:\PROJECT\EV_ErgMt
.\make_demo.ps1
.\start_viewer.ps1
```

브라우저의 **Data Viewer** 탭에서
`F:\PROJECT\EV_ErgMt\output\formula_e_demo.log`를 선택합니다.
Formula E 더미 데이터의 전압, 전류, 출력, 누적 에너지, 회생제동 구간을 기존
에너지미터 로직과 그래프로 확인할 수 있습니다.

뷰어의 기본 Power Limit은 80 kW이므로 Formula E 데이터에는 350 kW로 설정하는
것이 좋습니다.

## ASC_TLMTSYS CSV 변환

`ASC_TLMTSYS`가 사용하는 `Time,Speed,RPM,Gear,Gas,Brake` CSV를 직접 변환합니다.

```powershell
python .\generate_ev_log.py `
  --csv ..\ASC_TLMTSYS\telemetry_log.csv `
  --output .\output\assetto_corsa_ev.log
```

열 이름은 대소문자를 구분하지 않으며 `Throttle`, `speed_kmh`, `timestamp` 별칭도
인식합니다. Gas/Brake가 0~1 또는 0~100 어느 범위여도 자동으로 정규화합니다.

## 변환 모델

- 최대 구동 출력: 350 kW
- 최대 회생 입력: 350 kW
- 공칭 HV 전압: 약 600 V
- 전류 범위: -749~749 A
- 12 V 보조 전원과 부하 기반 온도 신호 포함
- 양의 전류는 배터리 방전/구동, 음의 전류는 회생제동

전력 신호는 AC의 속도, 스로틀 및 브레이크를 적극 사용하지만, Assetto Corsa가
실제 Formula E 배터리 전압과 전류를 제공하지 않으므로 결과는 물리 기반의
시각화용 추정 데이터이며 공식 계측값이 아닙니다.

## 구성

- `generate_ev_log.py`: CSV 변환기와 100 Hz Formula E 더미 랩 생성기
- `capture_assetto_corsa.py`: AC 공유 메모리를 직접 읽는 100 Hz 실시간 기록기
- `start_ac_capture.ps1`: 게임 연동 기록 실행
- `make_demo.ps1`: 샘플 로그 재생성
- `start_viewer.ps1`: 기존 FSK-EEM Vue 뷰어 실행
- `output/formula_e_demo.log`: 즉시 열어볼 수 있는 샘플
