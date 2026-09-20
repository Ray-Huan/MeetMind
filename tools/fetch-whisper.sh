#!/usr/bin/env bash
# 下载 whisper.cpp 源码到 third_party/whisper.cpp
#
# 为什么不入库：whisper.cpp 体积大且随上游更新，作为拷贝提交会让仓库膨胀；
# 用脚本按需获取，clone 后跑一次即可。
# 若 github.com 直连不稳定，可设置镜像，例如：
#   WHISPER_MIRROR=https://codeload.github.com  bash tools/fetch-whisper.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/whisper.cpp"

if [ -d "${DEST}" ]; then
    echo "已存在: ${DEST}"
    echo "如需重新获取，请先删除该目录。"
    exit 0
fi

BASE="${WHISPER_MIRROR:-https://codeload.github.com}"
URL="${BASE}/ggml-org/whisper.cpp/tar.gz/refs/heads/master"

mkdir -p "${ROOT}/third_party"
echo "下载 whisper.cpp 源码: ${URL}"
curl -fL "${URL}" | tar xz -C "${ROOT}/third_party"

if [ -d "${ROOT}/third_party/whisper.cpp-master" ]; then
    mv "${ROOT}/third_party/whisper.cpp-master" "${DEST}"
fi
[ -f "${DEST}/CMakeLists.txt" ] || { echo "下载/解压失败"; exit 1; }

echo "完成: ${DEST}"
echo "下一步： bash tools/build.sh release"
