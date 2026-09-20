#!/usr/bin/env bash
# MeetMind 部署脚本（让 build/bin 自包含运行库，可直接分发）
#
# 为什么不能只用 windeployqt：
#   windeployqt --compiler-runtime 复制的是 **Qt 安装目录自带**的 libstdc++-6.dll /
#   libgcc_s_seh-1.dll / libwinpthread-1.dll。Qt 6.7.1 的 mingw_64 里这套运行库是
#   2021 年构建的，与我们的编译工具链（mingw1310_64 / GCC 13.1）不是同一版本。
#   而 Windows 的 DLL 搜索顺序里 **exe 所在目录优先于 PATH**，于是旧运行库会覆盖
#   正确的那个 —— 后果是部分二进制启动即崩（实测 mm_tests.exe 段错误）。
#
# 因此本脚本在 windeployqt 之后，用编译器自带的运行库覆盖掉 Qt 那份，并做启动校验。
set -u

cd "$(dirname "$0")/.." || exit 2
ROOT="$(pwd -W 2>/dev/null || true)"; [ -z "${ROOT}" ] && ROOT="$(pwd)"

WINDEPLOYQT="${WINDEPLOYQT:-C:/Download/OfficeSoftware/QT6/6.7.1/mingw_64/bin/windeployqt.exe}"
MINGW_BIN="${MINGW_BIN:-C:/Download/OfficeSoftware/QT6/Tools/mingw1310_64/bin}"
BIN_DIR="${ROOT}/build/bin"

if [ ! -x "${WINDEPLOYQT}" ]; then echo "ERROR: 未找到 windeployqt: ${WINDEPLOYQT}" >&2; exit 2; fi
if [ ! -d "${BIN_DIR}" ]; then echo "ERROR: 未找到 ${BIN_DIR}，请先构建" >&2; exit 2; fi

echo "== 1/3 windeployqt =="
"${WINDEPLOYQT}" --release --compiler-runtime --no-translations \
    --no-system-d3d-compiler --no-opengl-sw "${BIN_DIR}/MeetMind.exe" \
    | tail -3

echo "== 2/3 用编译器自带运行库覆盖 Qt 版本 =="
RUNTIME_DLLS=(libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
for dll in "${RUNTIME_DLLS[@]}"; do
    src="${MINGW_BIN}/${dll}"
    dst="${BIN_DIR}/${dll}"
    if [ ! -f "${src}" ]; then
        echo "  WARN: 编译器运行库缺失 ${src}"
        continue
    fi
    if [ -f "${dst}" ] && cmp -s "${src}" "${dst}"; then
        echo "  ${dll} 已一致"
    else
        cp -f "${src}" "${dst}" && echo "  ${dll} 已覆盖为编译器版本"
    fi
done

echo "== 3/3 启动校验 =="
fail=0
if "${BIN_DIR}/mm_tests.exe" -q >/dev/null 2>&1; then
    echo "  mm_tests.exe   启动并全量跑通"
else
    echo "  mm_tests.exe   失败（退出码 $?）" >&2
    fail=1
fi
if "${BIN_DIR}/meetmind-cli.exe" --version >/dev/null 2>&1; then
    echo "  meetmind-cli   正常"
else
    echo "  meetmind-cli   失败" >&2
    fail=1
fi

echo "部署目录: ${BIN_DIR}  ($(du -sh "${BIN_DIR}" | cut -f1))"
exit ${fail}
