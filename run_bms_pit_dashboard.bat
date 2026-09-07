@echo off
setlocal
title DJY EV DALY BMS Pit Dashboard
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run_bms_pit_dashboard.ps1" -Port COM5
if errorlevel 1 pause
