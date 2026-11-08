#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# macOS 打包：把 build/bin 的扁平产物拼成 cxk-tools.app，再压成 dmg。
#
# 用法：scripts/package-macos.sh [构建目录] [输出目录]
#   scripts/package-macos.sh                    # build/ + dist/
#   scripts/package-macos.sh build dist         # 显式指定
#
# 为什么在打包阶段才拼 .app：CMake 侧刻意不开 MACOSX_BUNDLE，好让三个平台的
# 开发期布局完全一致（bin/cxk-tools + bin/plugins）。拼出来的 bundle 把插件放在
# Contents/MacOS/plugins，正好是 PluginManager::pluginDir() 期待的「可执行文件
# 同级/plugins」。
#
# 依赖：macdeployqt（Qt 自带）、codesign、hdiutil（系统自带）。
# 签名用 ad-hoc（-s -）：Apple Silicon 上未签名的可执行文件根本起不来，
# 这里只是让它能跑，不代表用户看到「已验证的开发者」。
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
DIST_DIR="${2:-$ROOT/dist}"

if [[ ! -f "$BUILD_DIR/xtools-version.txt" ]]; then
    echo "[error] 缺少 $BUILD_DIR/xtools-version.txt，先跑 scripts/build.sh" >&2
    exit 1
fi
VERSION="$(cat "$BUILD_DIR/xtools-version.txt")"

# 更新清单里的平台 key：macos-arm64 / macos-x64
case "$(uname -m)" in
    arm64)  ARCH_KEY="arm64" ;;
    x86_64) ARCH_KEY="x64" ;;
    *)      echo "[error] 未知架构: $(uname -m)" >&2; exit 1 ;;
esac

APP="$DIST_DIR/macos-$ARCH_KEY/cxk-tools.app"
FLAT="$DIST_DIR/macos-$ARCH_KEY/flat"

rm -rf "$DIST_DIR/macos-$ARCH_KEY"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

echo "[step] install -> flat"
cmake --install "$BUILD_DIR" --prefix "$FLAT"

# 主程序与插件都进 Contents/MacOS：插件的相对位置必须与开发期一致。
mv "$FLAT/cxk-tools" "$APP/Contents/MacOS/cxk-tools"
if [[ -d "$FLAT/plugins" ]]; then
    mv "$FLAT/plugins" "$APP/Contents/MacOS/plugins"
fi
rmdir "$FLAT" 2>/dev/null || true

echo "[step] Info.plist + icon"
sed "s/@VERSION@/$VERSION/g" "$ROOT/installer/macos/Info.plist.in" > "$APP/Contents/Info.plist"
cp "$ROOT/assets/app.icns" "$APP/Contents/Resources/app.icns"

# macdeployqt 只自动处理主程序；插件是我们自己的动态库，必须用 -executable=
# 逐个告诉它，否则插件里的 Qt framework 路径不会被改写，运行时找不到 QtCore。
echo "[step] macdeployqt"
MACDEPLOYQT="${MACDEPLOYQT:-}"
if [[ -z "$MACDEPLOYQT" ]]; then
    if [[ -n "${QT_DIR:-}" && -x "$QT_DIR/bin/macdeployqt" ]]; then
        MACDEPLOYQT="$QT_DIR/bin/macdeployqt"
    else
        MACDEPLOYQT="$(command -v macdeployqt || true)"
    fi
fi
if [[ -z "$MACDEPLOYQT" || ! -x "$MACDEPLOYQT" ]]; then
    echo "[error] 找不到 macdeployqt，请设置 QT_DIR 或把它加进 PATH" >&2
    exit 1
fi

DEPLOY_ARGS=("$APP" -always-overwrite)
if [[ -d "$APP/Contents/MacOS/plugins" ]]; then
    for plugin in "$APP/Contents/MacOS/plugins"/*; do
        [[ -f "$plugin" ]] || continue
        DEPLOY_ARGS+=("-executable=$plugin")
    done
fi
"$MACDEPLOYQT" "${DEPLOY_ARGS[@]}"

echo "[step] strip + prune"
# -x 只去掉局部符号：保留导出符号，能压掉一部分体积又不影响运行。
strip -x "$APP/Contents/MacOS/cxk-tools" || true
if [[ -d "$APP/Contents/MacOS/plugins" ]]; then
    for plugin in "$APP/Contents/MacOS/plugins"/*; do
        [[ -f "$plugin" ]] && strip -x "$plugin" || true
    done
fi
cmake -DXTools_STAGE_DIR="$APP" -P "$ROOT/cmake/PruneQtRuntime.cmake"

echo "[step] ad-hoc codesign"
codesign --force --deep --sign - "$APP"

echo "[step] dmg"
DMG_ROOT="$DIST_DIR/dmg-$ARCH_KEY"
DMG="$DIST_DIR/cxk-tools-$VERSION-macos-$ARCH_KEY.dmg"
rm -rf "$DMG_ROOT" "$DMG"
mkdir -p "$DMG_ROOT"
cp -R "$APP" "$DMG_ROOT/"
# 拖拽安装的惯例：放一个 /Applications 的软链接
ln -s /Applications "$DMG_ROOT/Applications"
hdiutil create -volname "CXK Tools" -srcfolder "$DMG_ROOT" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$DMG_ROOT"

echo
echo "APP_BYTES=$(du -sk "$APP" | cut -f1 | awk '{print $1 * 1024}')"
echo "DMG_BYTES=$(stat -f%z "$DMG")"
echo "PACKAGE_OK  -->  $DMG"
