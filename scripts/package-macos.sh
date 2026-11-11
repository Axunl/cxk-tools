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
# 依赖：macdeployqt（Qt 自带）、codesign、hdiutil（系统自带）、
#       notarytool + stapler（Xcode 13+ 的命令行工具，只在公证时用到）。
#
# 签名与公证是**可选**的，由环境变量决定（详见 README「代码签名与公证」）：
#   配了 XTOOLS_MACOS_SIGN_IDENTITY  -> Developer ID 签名 + 硬化运行时；
#                                      再配上公证凭据就继续公证并装订票据；
#   没配                              -> 退回 ad-hoc（-s -）。Apple Silicon 上
#                                      未签名的可执行文件根本起不来，ad-hoc 只是
#                                      让它能跑，用户仍然会看到「无法验证开发者」。
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

# 部署目标一致性检查（只告警不中断）。
# 二进制里的 minos 来自 CMAKE_OSX_DEPLOYMENT_TARGET（见根 CMakeLists.txt），
# Info.plist 里的 LSMinimumSystemVersion 是另一份手写的值 —— 两者不一致时
# 表现是「在旧系统上双击毫无反应」，非常难查，所以每次都打出来。
echo "[step] deployment target"
EXE="$APP/Contents/MacOS/cxk-tools"
MINOS="$(otool -l "$EXE" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
if [[ -z "$MINOS" ]]; then
    # 老链接器只写 LC_VERSION_MIN_MACOSX
    MINOS="$(otool -l "$EXE" 2>/dev/null | awk '/LC_VERSION_MIN_MACOSX/{f=1} f&&/version/{print $2; exit}')"
fi
PLIST_MIN="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist" 2>/dev/null || true)"
echo "MINOS=${MINOS:-unknown}  PLIST_MIN=${PLIST_MIN:-unknown}"
if [[ -n "$MINOS" && -n "$PLIST_MIN" && "$MINOS" != "$PLIST_MIN" ]]; then
    echo "[warn] 可执行文件的 minos 与 Info.plist 的 LSMinimumSystemVersion 不一致：" \
         "实际 ${MINOS}，声明 ${PLIST_MIN}（检查根 CMakeLists.txt 的 CMAKE_OSX_DEPLOYMENT_TARGET）" >&2
fi

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

# jsrun 插件用 QJSEngine（QtQml）。macdeployqt 会顺着 -executable= 找到这个依赖，
# 但一旦它没跟上（版本差异/参数变化），表现是插件加载失败、工具静默消失，
# 所以这里只警告不中断：包本身还能用，其余 21 个工具不受影响。
if [[ ! -d "$APP/Contents/Frameworks/QtQml.framework" ]]; then
    echo "[warn] QtQml.framework 未进入 app bundle：jsrun 插件将无法加载" >&2
fi

echo "[step] strip + prune"
# -x 只去掉局部符号：保留导出符号，能压掉一部分体积又不影响运行。
strip -x "$APP/Contents/MacOS/cxk-tools" || true
if [[ -d "$APP/Contents/MacOS/plugins" ]]; then
    for plugin in "$APP/Contents/MacOS/plugins"/*; do
        [[ -f "$plugin" ]] && strip -x "$plugin" || true
    done
fi
cmake -DXTools_STAGE_DIR="$APP" -P "$ROOT/cmake/PruneQtRuntime.cmake"

# ---------------------------------------------------------------------------
# 签名与公证
#
# 两个钩子共用一个前提：签名身份来自 XTOOLS_MACOS_SIGN_IDENTITY，值是钥匙串里
# 那条 "Developer ID Application: 名字 (TEAMID)"，也可以是 40 位指纹。
# ---------------------------------------------------------------------------
SIGN_ID="${XTOOLS_MACOS_SIGN_IDENTITY:-}"
ENTITLEMENTS="${XTOOLS_MACOS_ENTITLEMENTS:-$ROOT/installer/macos/cxk-tools.entitlements}"

# 公证凭据三选一，优先级与 Apple 的推荐一致：
#   1) keychain profile：凭据存在钥匙串里，命令行上不留密码（CI 用这个）；
#   2) App Store Connect API Key（.p8）；
#   3) Apple ID + App 专用密码（个人机器上最省事）。
# 没配就只签名、不公证 —— 用户第一次打开仍会被 Gatekeeper 拦。
NOTARY_ARGS=()
if [[ -n "${XTOOLS_NOTARY_PROFILE:-}" ]]; then
    NOTARY_ARGS=(--keychain-profile "$XTOOLS_NOTARY_PROFILE")
elif [[ -n "${XTOOLS_API_KEY_PATH:-}" ]]; then
    NOTARY_ARGS=(--key "$XTOOLS_API_KEY_PATH"
                 --key-id "${XTOOLS_API_KEY_ID:?XTOOLS_API_KEY_ID is required with XTOOLS_API_KEY_PATH}"
                 --issuer "${XTOOLS_API_ISSUER:?XTOOLS_API_ISSUER is required with XTOOLS_API_KEY_PATH}")
elif [[ -n "${XTOOLS_APPLE_ID:-}" ]]; then
    NOTARY_ARGS=(--apple-id "$XTOOLS_APPLE_ID"
                 --password "${XTOOLS_APPLE_APP_PASSWORD:?XTOOLS_APPLE_APP_PASSWORD is required with XTOOLS_APPLE_ID}"
                 --team-id "${XTOOLS_APPLE_TEAM_ID:?XTOOLS_APPLE_TEAM_ID is required with XTOOLS_APPLE_ID}")
fi

DMG="$DIST_DIR/cxk-tools-$VERSION-macos-$ARCH_KEY.dmg"

# 签一个文件或 bundle。--timestamp 必须有：没有时间戳的签名在证书到期那天
# 会让所有已发布的包一起变成「签名无效」。
#
# 刻意不用 --deep：它按目录深度猜顺序，对 framework 里嵌套的 helper 判断不可靠，
# Apple 也明确不推荐用它做分发签名。顺序由下面的 sign_bundle 自己保证。
sign_path() { # $1=目标  $2=entitlements（可为空）
    if [[ -n "${2:-}" ]]; then
        codesign --force --timestamp --options runtime --entitlements "$2" --sign "$SIGN_ID" "$1"
    else
        codesign --force --timestamp --options runtime --sign "$SIGN_ID" "$1"
    fi
}

# 由内向外签名：先签所有被嵌入的 Mach-O（Qt 框架二进制、Qt 平台插件、
# 我们的插件、主程序），最后签 .app 容器。顺序颠倒的话容器封条会先算好、
# 随后内部文件又被改写，codesign --verify 立刻报
# "a sealed resource is missing or invalid"。
sign_bundle() { # $1=.app 路径
    local app="$1"
    local exe="$1/Contents/MacOS/cxk-tools"
    while IFS= read -r -d '' item; do
        # 主程序单独签（要带 entitlements），这里跳过以免签两遍
        if [[ "$item" == "$exe" ]]; then
            continue
        fi
        if file -b "$item" | grep -q 'Mach-O'; then
            sign_path "$item"
        fi
    done < <(find "$app" -type f -print0)
    sign_path "$exe" "$ENTITLEMENTS"
    sign_path "$app" "$ENTITLEMENTS"
}

notary_submit() { # $1=待提交的 zip / dmg
    echo "[step] notarytool submit $(basename "$1")"
    xcrun notarytool submit "$1" "${NOTARY_ARGS[@]}" --wait
}

if [[ -n "$SIGN_ID" ]]; then
    if [[ ! -f "$ENTITLEMENTS" ]]; then
        echo "[error] 找不到授权文件：$ENTITLEMENTS（硬化运行时签名需要它）" >&2
        exit 1
    fi
    if ! security find-identity -v -p codesigning | grep -qF "$SIGN_ID"; then
        echo "[error] 钥匙串里没有这个签名身份：$SIGN_ID" >&2
        echo "        先导入 Developer ID Application 证书（见 README「代码签名与公证」）" >&2
        exit 1
    fi
    echo "[step] codesign ($SIGN_ID)"
    sign_bundle "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"

    if [[ ${#NOTARY_ARGS[@]} -gt 0 ]]; then
        if ! xcrun --find notarytool >/dev/null 2>&1; then
            echo "[error] 找不到 notarytool，需要 Xcode 13+ 的命令行工具" >&2
            exit 1
        fi
        # 公证 .app 要先打成 zip（notarytool 不吃裸 bundle）。票据装订进 bundle 后，
        # 用户从 dmg 里拖出来第一次离线启动也能过 Gatekeeper。
        APP_ZIP="$DIST_DIR/macos-$ARCH_KEY/cxk-tools-notarize.zip"
        echo "[step] notarize app"
        ditto -c -k --keepParent "$APP" "$APP_ZIP"
        notary_submit "$APP_ZIP"
        xcrun stapler staple "$APP"
        rm -f "$APP_ZIP"
    elif [[ "${XTOOLS_REQUIRE_NOTARIZATION:-0}" == "1" ]]; then
        echo "[error] 有签名身份但没有公证凭据：XTOOLS_REQUIRE_NOTARIZATION=1 时不允许发未公证的包" >&2
        echo "        设 XTOOLS_NOTARY_PROFILE（推荐）或 XTOOLS_APPLE_ID + XTOOLS_APPLE_APP_PASSWORD + XTOOLS_APPLE_TEAM_ID" >&2
        exit 1
    else
        echo "[warn] 没有公证凭据，包已签名但未公证：用户首次打开仍会被 Gatekeeper 拦一次" >&2
    fi
else
    echo "[step] ad-hoc codesign"
    echo "[warn] 未设置 XTOOLS_MACOS_SIGN_IDENTITY：只能 ad-hoc 签名，用户首次打开会被拦（右键「打开」）" >&2
    codesign --force --deep --sign - "$APP"
fi

echo "[step] dmg"
DMG_ROOT="$DIST_DIR/dmg-$ARCH_KEY"
rm -rf "$DMG_ROOT" "$DMG"
mkdir -p "$DMG_ROOT"
# 用 ditto 而不是 cp -R：装订进去的公证票据是扩展属性，cp 只保证普通文件内容，
# ditto 才是 Apple 推荐的「整包搬运」工具。
ditto "$APP" "$DMG_ROOT/"
# 拖拽安装的惯例：放一个 /Applications 的软链接
ln -s /Applications "$DMG_ROOT/Applications"
hdiutil create -volname "CXK Tools" -srcfolder "$DMG_ROOT" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$DMG_ROOT"

# dmg 本体也要签、也要公证：用户双击的是这个文件，Gatekeeper 先看它的签名。
if [[ -n "$SIGN_ID" ]]; then
    echo "[step] sign dmg"
    codesign --force --timestamp --sign "$SIGN_ID" "$DMG"
    if [[ ${#NOTARY_ARGS[@]} -gt 0 ]]; then
        echo "[step] notarize dmg"
        notary_submit "$DMG"
        xcrun stapler staple "$DMG"
        # 最后一道自检：Gatekeeper 到底认不认这个包。刚公证完的几分钟内
        # 缓存可能还没同步，所以这里只告警、不中断发布。
        if ! spctl --assess --type open --context context:primary-signature -vv "$DMG"; then
            echo "[warn] spctl 对 dmg 的评估未通过（公证刚结束时可稍等几分钟再复查）" >&2
        fi
    fi
fi

echo
echo "APP_BYTES=$(du -sk "$APP" | cut -f1 | awk '{print $1 * 1024}')"
echo "DMG_BYTES=$(stat -f%z "$DMG")"
echo "PACKAGE_OK  -->  $DMG"
