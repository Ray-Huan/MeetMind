@echo off
rem ============================================================
rem  MeetMind CLI
rem  Usage:
rem    run-cli.bat --list-engines
rem    run-cli.bat meeting.wav -o output --formats md,json,srt
rem  Tip: you can also drag a .wav file onto this script.
rem ============================================================
setlocal
cd /d "%~dp0"
set "EXE=build\bin\meetmind-cli.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found.
    echo         Build first:  bash tools/build.sh release
    pause
    exit /b 1
)
if "%~1"=="" (
    echo.
    echo No arguments given - showing help.
    echo Example:
    echo    run-cli.bat data\fixtures\meeting_zh.wav -o output
    echo.
    "%EXE%" --help
    pause
    exit /b 0
)
"%EXE%" %*
endlocal
