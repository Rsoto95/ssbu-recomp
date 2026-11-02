@echo off
rem The launcher: play, settings (graphics.ini, controllers.ini), setup guide and troubleshooting (scripts\launcher_window.ps1).
cd /d "%~dp0"
start "" powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0scripts\launcher_window.ps1"
