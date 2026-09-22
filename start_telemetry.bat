@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start_park_telemetry.ps1" -OpenDashboard %*
if errorlevel 1 pause
