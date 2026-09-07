@echo off
setlocal
title DJY EV Telemetry Dashboard
set "ROOT=%~dp0"
set "EXE=%ROOT%bin\DJY_EvTLMT.exe"

if /I "%~1"=="rebuild" goto BUILD
set "MODE="
if /I "%~1"=="ev" set "MODE=--ev"
if /I "%~1"=="ev-direct" set "MODE=--ev-direct"
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
start "" "%EXE%" %MODE%
exit /b 0
