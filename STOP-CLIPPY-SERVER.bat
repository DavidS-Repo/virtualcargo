@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Clippy coordinated shutdown
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0SHUTDOWN-CLIPPY-SERVER.ps1" %*
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" (echo CLIPPY COORDINATED SHUTDOWN COMPLETED.) else (echo CLIPPY COORDINATED SHUTDOWN FAILED with exit code %RESULT%.)
if /i not "%CLIPPY_NO_PAUSE%"=="1" pause
exit /b %RESULT%
