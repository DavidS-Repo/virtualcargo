@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Clippy PostgreSQL Virtual Cargo Server Manager

set "CLIPPY_MANAGER_SCRIPT=%~f0"
set "CLIPPY_MANAGER_ROOT=%~dp0"
set "CLIPPY_MANAGER_COMMAND=%~1"
if not defined CLIPPY_MANAGER_COMMAND set "CLIPPY_MANAGER_COMMAND=start"

if /i "%CLIPPY_NO_ELEVATE%"=="1" goto run_manager
if /i "%CLIPPY_MANAGER_COMMAND%"=="help" goto run_manager
if /i "%CLIPPY_MANAGER_COMMAND%"=="validate" goto run_manager
fltmc >nul 2>&1
if not errorlevel 1 goto run_manager

echo Requesting Administrator permission for PostgreSQL and the DayZ server manager...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$arguments=@($env:CLIPPY_MANAGER_COMMAND); Start-Process -FilePath $env:CLIPPY_MANAGER_SCRIPT -ArgumentList $arguments -WorkingDirectory $env:CLIPPY_MANAGER_ROOT -Verb RunAs"
exit /b %ERRORLEVEL%

:run_manager
if not exist "%~dp0ClippyServerManager.ps1" (
  echo ERROR: ClippyServerManager.ps1 is missing next to START-CLIPPY-SERVER.bat.
  exit /b 2
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0ClippyServerManager.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" (echo CLIPPY SERVER MANAGER COMPLETED.) else (echo CLIPPY SERVER MANAGER FAILED with exit code %RESULT%.)
echo.
if /i not "%CLIPPY_NO_PAUSE%"=="1" pause
exit /b %RESULT%
