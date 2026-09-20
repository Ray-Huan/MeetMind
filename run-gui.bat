@echo off
rem ============================================================
rem  MeetMind - On-device Meeting Transcription & Minutes  (GUI)
rem  Usage: double-click this file, or:
rem         run-gui.bat [audio.wav]
rem  Notes: keep this file ASCII-only -- cmd.exe parses .bat using
rem         the active code page, non-ASCII text can be mis-decoded.
rem ============================================================
setlocal
cd /d "%~dp0"

set "EXE=build\bin\MeetMind.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found.
    echo         Build first:  bash tools/build.sh release
    pause
    exit /b 1
)

set "CFG=config\meetmind.json"
if exist "%CFG%" (
    start "" "%EXE%" -c "%CFG%" %*
) else (
    start "" "%EXE%" %*
)
endlocal
