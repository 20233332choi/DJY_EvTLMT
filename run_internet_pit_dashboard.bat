@echo off
setlocal
title DJY EV Internet Pit Dashboard
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run_ev_internet_pit.ps1"
if errorlevel 1 pause
