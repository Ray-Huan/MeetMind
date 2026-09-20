#!/usr/bin/env bash
# =============================================================================
#  MeetMind — 用 MSVC + CUDA 构建 whisper.dll（GPU 加速后端）
# =============================================================================
#  为什么需要这个脚本：
#    Windows 版 nvcc 的宿主编译器只能是 MSVC（cl.exe），MinGW 的 gcc 不受支持；
#    而本项目的 Qt / 主程序都是 MinGW 工具链。因此把 whisper.cpp 单独用
#    MSVC + CUDA 编译成 DLL，主程序以动态库方式链接（whisper 的 C API 是纯 C
#    接口，跨编译器 ABI 兼容）。
#
#  产物（全部落到 third_party/whisper-cuda-dll/）：
#    whisper.dll / ggml*.dll         —— CUDA 版推理后端
#    libwhisper.a                    —— MinGW 可直接链接的导入库（dlltool 生成）
#    cudart64_12.dll 等              —— CUDA 运行时
#    msvcp140.dll 等                 —— MSVC 运行时
#
#  用法：
#    bash tools/build-whisper-cuda.sh            # 配置 + 编译 + 落位
#    bash tools/build-whisper-cuda.sh --stage    # 只重新落位（DLL 已编译好时）
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WSRC="${ROOT}/third_party/whisper.cpp"
BUILD="${ROOT}/build-whisper-cuda"
STAGE="${ROOT}/third_party/whisper-cuda-dll"
CMAKE="C:/Download/OfficeSoftware/Cmake/bin/cmake"
NINJA="C:/Download/OfficeSoftware/QT6/Tools/Ninja/ninja.exe"
MINGW_BIN="C:/Download/OfficeSoftware/QT6/Tools/mingw1310_64/bin"
# RTX 4060 Laptop = Ada Lovelace = compute capability 8.9
CUDA_ARCH="${CUDA_ARCH:-89}"
JOBS="${JOBS:-12}"

log() { printf '\033[36m[whisper-cuda]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[whisper-cuda] 错误:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
#  1. 探测工具链（沙箱内无法调用 cmd.exe/vcvars64.bat，故手工构造环境变量）
# ---------------------------------------------------------------------------
detect_vs() {
    local bases=(
        "C:/Program Files (x86)/Microsoft Visual Studio/2019"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022"
        "C:/Program Files/Microsoft Visual Studio/2022"
    )
    local editions=(Enterprise Professional Community BuildTools Preview)
    local b e best=""
    for b in "${bases[@]}"; do
        for e in "${editions[@]}"; do
            if [ -d "${b}/${e}/VC/Tools/MSVC" ]; then
                # 取版本号最大的那一套
                local v
                v="$(ls "${b}/${e}/VC/Tools/MSVC" | sort -V | tail -1)"
                best="${b}/${e}|${v}"
            fi
        done
    done
    [ -n "$best" ] || die "未找到 Visual Studio（需要含 C++ 工具链的 VS2019/2022）"
    echo "$best"
}

detect_sdk() {
    local root="C:/Program Files (x86)/Windows Kits/10"
    [ -d "${root}/Include" ] || die "未找到 Windows SDK"
    ls "${root}/Include" | sort -V | tail -1
}

detect_cuda() {
    local root="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA"
    [ -d "$root" ] || die "未找到 CUDA Toolkit"
    ls "$root" | grep -E '^v[0-9]+' | sort -V | tail -1
}

VS_INFO="$(detect_vs)"
VS_ROOT="${VS_INFO%%|*}"
VS_VER="${VS_INFO##*|}"
SDK_VER="$(detect_sdk)"
CUDA_VER="$(detect_cuda)"

VS_WIN="${VS_ROOT//\//\\}"
SDK_WIN='C:\Program Files (x86)\Windows Kits\10'
CUDA_WIN="C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\${CUDA_VER}"

export INCLUDE="${VS_WIN}\\VC\\Tools\\MSVC\\${VS_VER}\\include;${SDK_WIN}\\Include\\${SDK_VER}\\ucrt;${SDK_WIN}\\Include\\${SDK_VER}\\um;${SDK_WIN}\\Include\\${SDK_VER}\\shared;${CUDA_WIN}\\include"
export LIB="${VS_WIN}\\VC\\Tools\\MSVC\\${VS_VER}\\lib\\x64;${SDK_WIN}\\Lib\\${SDK_VER}\\ucrt\\x64;${SDK_WIN}\\Lib\\${SDK_VER}\\um\\x64;${CUDA_WIN}\\lib\\x64"
export PATH="${VS_ROOT}/VC/Tools/MSVC/${VS_VER}/bin/Hostx64/x64:C:/Program Files (x86)/Windows Kits/10/bin/${SDK_VER}/x64:${CUDA_WIN//\\//}/bin:${MINGW_BIN}:${PATH}"
export CUDA_PATH="${CUDA_WIN}"

log "工具链: VS ${VS_VER} | Windows SDK ${SDK_VER} | CUDA ${CUDA_VER} | arch sm_${CUDA_ARCH}"

# ---------------------------------------------------------------------------
#  2. 配置 + 编译
# ---------------------------------------------------------------------------
if [ "${1:-}" != "--stage" ]; then
    command -v cl   >/dev/null || die "cl.exe 不可用（MSVC 环境未就绪）"
    command -v nvcc >/dev/null || die "nvcc 不可用（CUDA 环境未就绪）"

    log "配置 CMake ..."
    "${CMAKE}" -S "${WSRC}" -B "${BUILD}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl \
        -DCMAKE_MAKE_PROGRAM="${NINJA}" \
        -DGGML_CUDA=ON \
        -DGGML_NATIVE=OFF \
        -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCH}" \
        -DBUILD_SHARED_LIBS=ON \
        -DWHISPER_BUILD_EXAMPLES=OFF \
        -DWHISPER_BUILD_TESTS=OFF \
        -DWHISPER_BUILD_SERVER=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        2>&1 | tail -6

    log "编译（这步较慢，CUDA 内核数量多）..."
    "${CMAKE}" --build "${BUILD}" --config Release -j "${JOBS}"
fi

# ---------------------------------------------------------------------------
#  3. 落位到 STAGE
# ---------------------------------------------------------------------------
mkdir -p "${STAGE}"
log "落位 DLL 到 ${STAGE}"

# 3.1 whisper 及其 ggml 依赖（显式列举，避免把不相关的 parakeet.dll 等一起带上）
for n in whisper.dll ggml.dll ggml-base.dll ggml-cpu.dll ggml-cuda.dll; do
    [ -f "${BUILD}/bin/${n}" ] && cp -f "${BUILD}/bin/${n}" "${STAGE}/" || true
done

# 3.2 运行时依赖：按 DLL 的真实导入表精确拷贝。
#     实测 ggml-cuda.dll 只依赖 cudart64_12 / cublas64_12，
#     **不依赖** cublasLt64_12（551 MB）——精确拷贝可省下大量体积。
systemDll() {
    case "$1" in
        KERNEL32.dll|nvcuda.dll|USER32.dll|ADVAPI32.dll|SHELL32.dll|OLE32.dll| \
        api-ms-win-*.dll|ext-ms-*.dll) return 0 ;;
        *) return 1 ;;
    esac
}
collectDeps() {
    objdump -p "$1" 2>/dev/null | awk '/DLL Name:/{print $3}'
}
copyRuntime() {
    local name="$1" src
    if systemDll "$name"; then return 0; fi
    for src in \
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/${CUDA_VER}/bin/${name}" \
        "C:/Windows/System32/${name}" \
        "${MINGW_BIN}/${name}"
    do
        if [ -f "$src" ]; then
            cp -f "$src" "${STAGE}/"
            return 0
        fi
    done
    return 0   # 找不到就跳过（多为系统自带）
}

log "解析运行时依赖 ..."
# 在 STAGE 目录内用相对路径调用 objdump：objdump 是原生 Windows 程序，
# 传入 MSYS 风格绝对路径时可能无法打开文件。
(
    cd "${STAGE}"
    for own in whisper.dll ggml.dll ggml-base.dll ggml-cpu.dll ggml-cuda.dll; do
        if [ ! -f "$own" ]; then continue; fi
        while read -r dep; do
            if [ -n "$dep" ]; then copyRuntime "$dep"; fi
        done < <(collectDeps "$own")
    done
    objdump -p ggml-cuda.dll 2>/dev/null | awk '/DLL Name:/{print $3}' > /dev/null
)

# 3.3 导入库：优先使用 MSVC 直接产出的 whisper.lib。
#     原因：用 gendef 从 DLL 反推 .def 时，whisper.dll 里有 4800+ 个 MSVC 修饰的
#     C++ 符号，gendef 解析到一半就异常退出，写出的 .def 缺失 C API 导出，
#     生成的导入库链接不到 whisper_*。MSVC 自己导出的 .lib 是完整且权威的，
#     而 MinGW 的 ld 能直接读 COFF 导入库。
if [ -f "${BUILD}/src/whisper.lib" ]; then
    cp -f "${BUILD}/src/whisper.lib" "${STAGE}/whisper.lib"
    log "导入库: whisper.lib（来自 MSVC，供 MinGW ld 直接链接）"
elif [ -f "${STAGE}/whisper.dll" ]; then
    log "未找到 whisper.lib，回退用 dlltool 生成 libwhisper.a ..."
    (
        cd "${STAGE}"
        gendef whisper.dll >/dev/null 2>&1 || true
        if [ ! -f whisper.def ]; then
            echo "gendef 未能生成 whisper.def"
            exit 1
        fi
        dlltool -d whisper.def -l libwhisper.a -D whisper.dll
        rm -f whisper.def
    ) || die "导入库生成失败（gendef/dlltool 不在 PATH？）"
fi

log "产物清单："
ls -la "${STAGE}" | grep -E '\.(dll|a)$' || true
log "完成。现在可用 -DMEETMIND_WHISPER_CUDA_DLL=ON 构建主程序。"
