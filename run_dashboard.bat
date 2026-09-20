@echo off
setlocal EnableExtensions
title DJY EV Unified Launcher
set "ROOT=%~dp0"

if /I "%~1"=="internet" goto INTERNET
if /I "%~1"=="internet-control" goto INTERNET_CONTROL
if /I "%~1"=="local" goto LOCAL
if /I "%~1"=="wired" goto WIRED
if /I "%~1"=="bms" goto BMS
if /I "%~1"=="ev" goto EV_DASHBOARD
if /I "%~1"=="help" goto USAGE
if not "%~1"=="" goto USAGE_ERROR

:MENU
cls
echo ==================================================
echo              DJY EV Unified Launcher
echo ==================================================
echo [1] STM Rear USB direct telemetry ^(read only, recommended^)
echo [2] HTML local ESP/Gateway telemetry
echo [3] BMS USB bench
echo [4] Internet pit control ^(verified HTTPS only^)
echo [5] HTML internet pit telemetry ^(ngrok^)
echo [0] Exit
echo.
set "SELECT="
set /p "SELECT=Select: "
if "%SELECT%"=="1" goto WIRED
if "%SELECT%"=="2" goto LOCAL
if "%SELECT%"=="3" goto BMS_MENU
if "%SELECT%"=="4" goto INTERNET_CONTROL
if "%SELECT%"=="5" goto INTERNET
if "%SELECT%"=="0" exit /b 0
echo Invalid selection.
pause
goto MENU

:INTERNET
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\run_ev_internet_pit.ps1"
set "RUN_RC=%errorlevel%"
if not "%RUN_RC%"=="0" pause
exit /b %RUN_RC%

:INTERNET_CONTROL
echo WARNING: This mode can send pit commands toward the vehicle.
echo Verified HTTPS, PIT ENABLE, and stationary safety gates are required.
set "CONFIRM="
set /p "CONFIRM=Type ENABLE to continue: "
if /I not "%CONFIRM%"=="ENABLE" (
    echo Cancelled.
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\run_ev_internet_pit.ps1" -EnableControl
set "RUN_RC=%errorlevel%"
if not "%RUN_RC%"=="0" pause
exit /b %RUN_RC%

:LOCAL
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\run_ev_stack.ps1"
set "RUN_RC=%errorlevel%"
if not "%RUN_RC%"=="0" pause
exit /b %RUN_RC%

:WIRED
set "REAR_PORT=%~2"
if "%REAR_PORT%"=="" set "REAR_PORT=auto"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\run_ev_stack.ps1" -Wired -RearPort "%REAR_PORT%"
set "RUN_RC=%errorlevel%"
if not "%RUN_RC%"=="0" pause
exit /b %RUN_RC%

:BMS_MENU
set "BMS_PORT=COM5"
set /p "BMS_PORT=BMS port [COM5]: "
if "%BMS_PORT%"=="" set "BMS_PORT=COM5"
goto BMS_RUN

:BMS
set "BMS_PORT=%~2"
if "%BMS_PORT%"=="" set "BMS_PORT=COM5"

:BMS_RUN
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\run_bms_pit_dashboard.ps1" -Port "%BMS_PORT%"
set "RUN_RC=%errorlevel%"
if not "%RUN_RC%"=="0" pause
exit /b %RUN_RC%

:EV_DASHBOARD
start "" "http://127.0.0.1:8766/pit"
exit /b 0

:USAGE_ERROR
echo Unknown mode: %~1
echo.

:USAGE
echo Usage:
echo   run_dashboard.bat                  Open menu
echo   run_dashboard.bat internet         Internet read-only telemetry
echo   run_dashboard.bat local            Local Gateway
echo   run_dashboard.bat wired [COM port]  Rear STM USB read-only telemetry
echo   run_dashboard.bat ev               Open HTML pit dashboard
echo   run_dashboard.bat bms [COM port]   BMS USB bench
echo   run_dashboard.bat internet-control Internet pit control
if /I "%~1"=="help" exit /b 0
exit /b 1
