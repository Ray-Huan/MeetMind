#!/usr/bin/env bash
# MeetMind 构建脚本（MinGW-w64 + Ninja + Qt6）
# 用法:
#   tools/build.sh            # 完整构建（Release）
#   tools/build.sh gpu        # GPU 构建（接入 CUDA 版 whisper.dll）
#   tools/build.sh core       # 仅构建核心库（快速语法检查）
#   tools/build.sh clean      # 清理构建目录
#   tools/build.sh dev        # 快速开发回路（无 whisper / 无 GUI）
set -u

cd "$(dirname "$0")/.." || exit 2
# Windows 版 cmake 需要 D:/ 形式路径；Git Bash 下 pwd -W 提供该形式
ROOT="$(pwd -W 2>/dev/null || true)"
if [ -z "${ROOT}" ]; then ROOT="$(pwd)"; fi
BUILD_DIR="${ROOT}/build"

CMAKE="C:/Download/OfficeSoftware/Cmake/bin/cmake"
NINJA_DIR="C:/Download/OfficeSoftware/QT6/Tools/Ninja"
MINGW_BIN="C:/Download/OfficeSoftware/QT6/Tools/mingw1310_64/bin"
QT_ROOT="C:/Download/OfficeSoftware/QT6/6.7.1/mingw_64"

export PATH="${MINGW_BIN}:${NINJA_DIR}:${PATH}"

JOBS="${MEETMIND_JOBS:-8}"

if [ ! -x "${CMAKE}" ]; then
    CMAKE="$(command -v cmake)"
fi
if [ -z "${CMAKE}" ]; then
    echo "ERROR: 未找到 cmake" >&2
    exit 2
fi

MODE="${1:-all}"

if [ "${MODE}" = "clean" ]; then
    rm -rf "${BUILD_DIR}"
    echo "已清理 ${BUILD_DIR}"
    exit 0
fi

CONFIGURE_ARGS=(
    -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_MAKE_PROGRAM="${NINJA_DIR}/ninja.exe"
    -DCMAKE_C_COMPILER="${MINGW_BIN}/gcc.exe"
    -DCMAKE_CXX_COMPILER="${MINGW_BIN}/g++.exe"
    -DMEETMIND_QT_ROOT="${QT_ROOT}"
)

if [ "${MODE}" = "release" ]; then
    # 发布构建：启用 whisper.cpp 端侧推理
    CONFIGURE_ARGS+=(
        -DMEETMIND_BUILD_CLI=ON
        -DMEETMIND_BUILD_GUI=ON
        -DMEETMIND_BUILD_TESTS=ON
        -DMEETMIND_WITH_WHISPER=ON
    )
elif [ "${MODE}" = "gpu" ]; then
    # GPU 构建：接入预编译的 CUDA 版 whisper.dll（GPU 加速）
    # 前置：先运行 bash tools/build-whisper-cuda.sh 生成 DLL
    CONFIGURE_ARGS+=(
        -DMEETMIND_BUILD_CLI=ON
        -DMEETMIND_BUILD_GUI=ON
        -DMEETMIND_BUILD_TESTS=ON
        -DMEETMIND_WITH_WHISPER=ON
        -DMEETMIND_WHISPER_CUDA_DLL=ON
    )
elif [ "${MODE}" = "core" ]; then
    CONFIGURE_ARGS+=(
        -DMEETMIND_BUILD_CLI=OFF
        -DMEETMIND_BUILD_GUI=OFF
        -DMEETMIND_BUILD_TESTS=OFF
        -DMEETMIND_WITH_WHISPER=OFF
    )
elif [ "${MODE}" = "gui" ]; then
    # 界面开发回路：核心 + CLI + 测试 + GUI，关闭 whisper
    CONFIGURE_ARGS+=(
        -DMEETMIND_BUILD_CLI=ON
        -DMEETMIND_BUILD_GUI=ON
        -DMEETMIND_BUILD_TESTS=ON
        -DMEETMIND_WITH_WHISPER=OFF
    )
elif [ "${MODE}" = "dev" ]; then
    # 快速开发回路：核心 + CLI + 测试，关闭 whisper 与 GUI
    CONFIGURE_ARGS+=(
        -DMEETMIND_BUILD_CLI=ON
        -DMEETMIND_BUILD_GUI=OFF
        -DMEETMIND_BUILD_TESTS=ON
        -DMEETMIND_WITH_WHISPER=OFF
    )
fi

echo "== 配置 =="
"${CMAKE}" "${CONFIGURE_ARGS[@]}" || exit 3

echo "== 构建 (${MODE}) =="
if [ "${MODE}" = "core" ]; then
    "${CMAKE}" --build "${BUILD_DIR}" --target mm_core -j "${JOBS}"
elif [ "${MODE}" = "gui" ]; then
    # 界面开发回路：核心 + CLI + 测试 + GUI，关闭 whisper
    CONFIGURE_ARGS+=(
        -DMEETMIND_BUILD_CLI=ON
        -DMEETMIND_BUILD_GUI=ON
        -DMEETMIND_BUILD_TESTS=ON
        -DMEETMIND_WITH_WHISPER=OFF
    )
elif [ "${MODE}" = "dev" ]; then
    "${CMAKE}" --build "${BUILD_DIR}" --target mm_tests -j "${JOBS}"
elif [ "${MODE}" = "release" ]; then
    "${CMAKE}" --build "${BUILD_DIR}" -j "${JOBS}"
else
    "${CMAKE}" --build "${BUILD_DIR}" -j "${JOBS}"
fi
STATUS=$?
echo "构建退出码: ${STATUS}"
exit ${STATUS}
