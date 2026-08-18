@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Clippy PostgreSQL Virtual Cargo Server Manager

set "CLIPPY_MANAGER_SCRIPT=%~f0"
set "CLIPPY_MANAGER_ROOT=%~dp0"
set "CLIPPY_MANAGER_COMMAND=%~1"
if not defined CLIPPY_MANAGER_COMMAND set "CLIPPY_MANAGER_COMMAND=start"

if /i "%CLIPPY_MANAGER_COMMAND%"=="admin" if not defined CLIPPY_ADMIN_URL_FILE set "CLIPPY_ADMIN_URL_FILE=%TEMP%\ClippyAdmin-%RANDOM%-%RANDOM%.url"

if /i "%CLIPPY_NO_ELEVATE%"=="1" goto run_manager
if /i "%CLIPPY_MANAGER_COMMAND%"=="help" goto run_manager
if /i "%CLIPPY_MANAGER_COMMAND%"=="validate" goto run_manager
fltmc >nul 2>&1
if not errorlevel 1 goto run_manager

if /i "%CLIPPY_MANAGER_COMMAND%"=="admin" goto elevate_admin

echo Requesting Administrator permission for PostgreSQL and the DayZ server manager...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$arguments=@($env:CLIPPY_MANAGER_COMMAND); Start-Process -FilePath $env:CLIPPY_MANAGER_SCRIPT -ArgumentList $arguments -WorkingDirectory $env:CLIPPY_MANAGER_ROOT -Verb RunAs"
exit /b %ERRORLEVEL%

:elevate_admin
set "CLIPPY_ADMIN_ELEVATED_CHILD=1"
set "CLIPPY_NO_PAUSE=1"
echo Requesting Administrator permission for the local Clippy Admin Panel...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$arguments=@($env:CLIPPY_MANAGER_COMMAND); $p=Start-Process -FilePath $env:CLIPPY_MANAGER_SCRIPT -ArgumentList $arguments -WorkingDirectory $env:CLIPPY_MANAGER_ROOT -Verb RunAs -Wait -PassThru; exit $p.ExitCode"
set "RESULT=%ERRORLEVEL%"
set "CLIPPY_ADMIN_ELEVATED_CHILD="
goto after_manager

:run_manager
if not exist "%~dp0ClippyServerManager.ps1" (
  echo ERROR: ClippyServerManager.ps1 is missing next to START-CLIPPY-SERVER.bat.
  exit /b 2
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0ClippyServerManager.ps1" "%CLIPPY_MANAGER_COMMAND%"
set "RESULT=%ERRORLEVEL%"

:after_manager
if /i "%CLIPPY_MANAGER_COMMAND%"=="admin" goto open_admin_url

goto finish

:open_admin_url
if /i "%CLIPPY_ADMIN_ELEVATED_CHILD%"=="1" goto finish
if not "%RESULT%"=="0" goto finish
if not defined CLIPPY_ADMIN_URL_FILE goto finish
if not exist "%CLIPPY_ADMIN_URL_FILE%" goto finish
set /p CLIPPY_ADMIN_URL=<"%CLIPPY_ADMIN_URL_FILE%"
del /q "%CLIPPY_ADMIN_URL_FILE%" >nul 2>&1
if defined CLIPPY_ADMIN_URL start "" "%CLIPPY_ADMIN_URL%"

:finish
echo.
if "%RESULT%"=="0" (echo CLIPPY SERVER MANAGER COMPLETED.) else (echo CLIPPY SERVER MANAGER FAILED with exit code %RESULT%.)
echo.
if /i not "%CLIPPY_NO_PAUSE%"=="1" pause
exit /b %RESULT%
