@echo off
setlocal
title DJY EV Telemetry Dashboard
set "ROOT=%~dp0"
set "EXE=%ROOT%bin\DJY_EvTLMT.exe"

if /I "%~1"=="rebuild" goto BUILD
if exist "%EXE%" goto RUN

:BUILD
echo [1/2] Building DJY EV Telemetry Dashboard...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\build.ps1"
if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

:RUN
echo [2/2] Starting dashboard...
start "" "%EXE%"
exit /b 0
