@echo off
setlocal EnableExtensions
set "CLIPPY_NO_PAUSE=1"
call "%~dp0START-CLIPPY-SERVER.bat" admin
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" pause
exit /b %RESULT%
