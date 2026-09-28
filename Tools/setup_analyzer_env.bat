@echo off
rem UniversalProductionTools local pose analyzer environment setup (double-click to install, add -CheckOnly to only check)
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_analyzer_env.ps1" %*
set UPT_EXIT=%ERRORLEVEL%
pause
exit /b %UPT_EXIT%
