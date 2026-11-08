#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Build cxk-tools on macOS / Linux.
#   Usage: scripts/build.sh [Debug|Release]
#   Qt location: set QT_DIR (or CMAKE_PREFIX_PATH); otherwise CMake finds it.
#
# macOS 说明：全局热键与开机自启尚未实现，代码会返回明确的"暂不支持"提示，
# 构建与界面功能本身不受影响。
# ---------------------------------------------------------------------------
set -euo pipefail

BUILD_TYPE="${1:-Release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"

ARGS=()

# 有 ninja 就用 ninja（比默认 Makefiles 快，且与 Windows 脚本行为一致）
if command -v ninja >/dev/null 2>&1; then
    ARGS+=(-G Ninja)
fi

if [[ -n "${QT_DIR:-}" ]]; then
    ARGS+=(-DCMAKE_PREFIX_PATH="$QT_DIR")
elif [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    ARGS+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi

ARGS+=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")

echo "==> configure ($BUILD_TYPE)"
cmake -S "$ROOT" -B "$BUILD_DIR" ${ARGS[@]+"${ARGS[@]}"}

echo "==> build"
cmake --build "$BUILD_DIR" --parallel

echo
echo "BUILD_OK  -->  $BUILD_DIR/bin/cxk-tools"
