#!/usr/bin/env bash
# MeetMind 打包脚本：把构建产物整理成可分发的绿色包（zip）
#
# 用法:
#   bash tools/package.sh              # 用现有 build/bin 打 GPU 包（默认）
#   bash tools/package.sh release      # 打 CPU 包（whisper 静态编入，体积小）
#   bash tools/package.sh --rebuild    # 先重新构建再打包
#   bash tools/package.sh --with-model # 把 models/ 下已有模型一并打入包（默认不含）
#
# 产物: dist/MeetMind-v1.0.0-win64-<gpu|cpu>.zip
# 说明: 模型权重（约 466 MB）默认不打包，包内附「下载模型.bat」按需下载。
set -euo pipefail

cd "$(dirname "$0")/.." || exit 2
ROOT="$(pwd -W 2>/dev/null || true)"; [ -z "${ROOT}" ] && ROOT="$(pwd)"
BIN_DIR="${ROOT}/build/bin"
DIST_DIR="${ROOT}/dist"
STAGE="${DIST_DIR}/stage"

# 版本号取自 project(MeetMind VERSION x.y.z) 块，而非 cmake_minimum_required 的 VERSION
VERSION="$(grep -A3 '^project(MeetMind' CMakeLists.txt | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
[ -z "${VERSION}" ] && VERSION="1.0.0"

MODE="gpu"
REBUILD=0
WITH_MODEL=0
for a in "$@"; do
    case "${a}" in
        release|cpu) MODE="cpu" ;;
        gpu) MODE="gpu" ;;
        --rebuild) REBUILD=1 ;;
        --with-model) WITH_MODEL=1 ;;
        *) echo "未知参数: ${a}"; exit 2 ;;
    esac
done

if [ "${REBUILD}" = "1" ]; then
    echo "== 重新构建 (${MODE}) =="
    if [ "${MODE}" = "gpu" ]; then
        bash tools/build.sh gpu
    else
        bash tools/build.sh release
    fi
fi

if [ ! -f "${BIN_DIR}/MeetMind.exe" ] || [ ! -f "${BIN_DIR}/meetmind-cli.exe" ]; then
    echo "ERROR: 未找到 ${BIN_DIR} 下的可执行文件，请先构建（bash tools/build.sh ${MODE}）" >&2
    exit 2
fi

echo "== 组装发布目录 =="
# 清理旧目录：用 find -delete 而非 rm -rf，避免触发批量删除的交互确认
[ -d "${STAGE}" ] && find "${STAGE}" -mindepth 1 -delete 2>/dev/null || true
mkdir -p "${STAGE}/data" "${STAGE}/config" "${STAGE}/models"

# 1) 可执行文件（不含开发测试 mm_tests.exe）
cp -f "${BIN_DIR}/MeetMind.exe" "${STAGE}/"
cp -f "${BIN_DIR}/meetmind-cli.exe" "${STAGE}/"

# 2) 运行库与插件（DLL + Qt 插件目录）
cp -f "${BIN_DIR}"/*.dll "${STAGE}/" 2>/dev/null || true
for d in platforms imageformats iconengines multimedia networkinformation styles tls generic; do
    [ -d "${BIN_DIR}/${d}" ] && cp -rf "${BIN_DIR}/${d}" "${STAGE}/"
done

# 3) 词典数据与示例配置
cp -f data/*.txt "${STAGE}/data/"
cp -f config/meetmind.json "${STAGE}/config/" 2>/dev/null || true

# 4) 模型（可选）
if [ "${WITH_MODEL}" = "1" ]; then
    cp -f models/*.bin "${STAGE}/models/" 2>/dev/null || true
fi

# 5) 启动脚本（保持 ASCII，避免 cmd 编码问题）
cat > "${STAGE}/启动MeetMind.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
if not exist MeetMind.exe ( echo [ERROR] MeetMind.exe not found. & pause & exit /b 1 )
if exist config\meetmind.json ( start "" MeetMind.exe -c config\meetmind.json %* ) else ( start "" MeetMind.exe %* )
endlocal
BAT

cat > "${STAGE}/命令行.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  meetmind-cli.exe --help
  echo.
  echo Example: 命令行.bat 音频.wav -o 输出目录 --formats md,json,srt
  pause
  exit /b 0
)
meetmind-cli.exe %*
endlocal
BAT

cat > "${STAGE}/下载模型.bat" <<'BAT'
@echo off
setlocal
cd /d "%~dp0"
if not exist models mkdir models
echo Downloading whisper small model (~466 MB) ...
curl -L -o models\ggml-small.bin https://hf-mirror.com/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
if %errorlevel%==0 (
  echo Done: models\ggml-small.bin
) else (
  echo Failed. Download manually and put it under models\ .
)
pause
endlocal
BAT

# 6) 使用说明
cat > "${STAGE}/使用说明.txt" <<'TXT'
MeetMind v1.0.0 — 端侧语音转写与智能会议纪要
================================================

一、开始使用
  1. 双击「下载模型.bat」下载语音识别模型（约 466 MB，只需一次）。
  2. 双击「启动MeetMind.bat」打开图形界面；
     或在命令行运行「命令行.bat 音频.wav -o 输出目录」。
  3. 程序会在本机离线完成全部处理，不产生任何网络请求。

二、GPU 加速
  本包为 GPU 版构建：有 NVIDIA 显卡时自动启用 CUDA 加速（约 40 倍实时）；
  无显卡或驱动不可用时自动回退 CPU，无需任何设置。
  如需强制 CPU，命令行加 --no-gpu。

三、文件说明
  MeetMind.exe / meetmind-cli.exe  图形界面 / 命令行
  *.dll                            运行所需动态库（含 Qt、CUDA、whisper）
  data\                            中文词典等数据
  config\meetmind.json             默认配置示例
  models\                          模型存放目录（下载模型.bat 会写入这里）

四、命令行示例
  命令行.bat 音频.wav -o 输出 --formats md,json,srt,txt
  命令行.bat 音频.wav --realtime              # 实时（流式）转写
  命令行.bat --list-engines                   # 查看识别后端状态

五、许可与来源
  本项目采用 MIT 许可，详见随附源码仓库 https://github.com/Ray-Huan/MeetMind
  依赖：whisper.cpp(MIT)、Qt6(LGPLv3)、CUDA(NVIDIA 专有)、jieba/zhconv(仅数据准备)。
TXT

# 7) 打包
PKG_NAME="MeetMind-v${VERSION}-win64-${MODE}"
ZIP_PATH="${DIST_DIR}/${PKG_NAME}.zip"
echo "== 压缩 ${PKG_NAME}.zip =="
[ -f "${ZIP_PATH}" ] && rm -f "${ZIP_PATH}"
(cd "${STAGE}" && powershell -NoProfile -Command \
    "Compress-Archive -Path '*' -DestinationPath '${ZIP_PATH}' -Force")
find "${STAGE}" -mindepth 1 -delete 2>/dev/null || true
rmdir "${STAGE}" 2>/dev/null || true

echo
echo "完成: ${ZIP_PATH}"
du -sh "${ZIP_PATH}" | sed 's/^/大小: /'
