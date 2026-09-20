@echo off
rem Download whisper small model (~466 MB) via HuggingFace mirror.
rem Run this after installing MeetMind, once.
setlocal
cd /d "%~dp0"
if not exist models mkdir models
echo Downloading whisper small model (~466 MB) ...
echo Mirror: hf-mirror.com
curl -L -o models\ggml-small.bin https://hf-mirror.com/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
if %errorlevel%==0 (
  echo.
  echo Done: models\ggml-small.bin
  echo You can now start MeetMind.
) else (
  echo.
  echo Download failed. Please download ggml-small.bin manually and put it under the models\ folder.
  echo See: https://github.com/Ray-Huan/MeetMind
)
echo.
pause
endlocal
