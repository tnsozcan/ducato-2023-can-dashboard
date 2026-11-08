@echo off
cd /d "%~dp0"
set "DASH_PORT=%~1"
if not defined DASH_PORT set "DASH_PORT=COM8"
py bridge\ets2_bridge.py --port "%DASH_PORT%"
pause
