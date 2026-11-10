#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# 下载并解包 ONNX Runtime C/C++ 开发包，产出 build/_third_party/onnxruntime-<平台>/
# 目录，供 FindOnnxruntime.cmake 通过 -DONNXRUNTIME_ROOT 找到。
#
# 为什么用 NuGet 包而不是官方 GitHub release 的 zip：
#   * NuGet 走 CDN，秒下（官方 release 的 zip 常被限速/超时）；
#   * 版本固定（Microsoft.ML.OnnxRuntime 1.20.1），CI 与本地结果一致；
#   * 包内已含 win-x64 的 onnxruntime.dll / .lib / 头文件，解出即可用。
#
# 只取 CPU 的 x64 子集（约 20 MB 解包后），不落 108 MB 的 nupkg 到仓库；nupkg 本身
# 下到临时目录，用完即删。缺网络/缺包时打印明确原因并以非零码退出，调用方（CI）据此
# 决定是否失败（发布构建要求成功，纯编译校验可容忍失败回落系统 OCR）。
#
# 平台：
#   Windows -> runtimes/win-x64/native  + build/native/include
#   Linux   -> runtimes/linux-x64/native（.so）+ 同上 include（未在本仓库实测，保留路径）
#   macOS   -> runtimes/osx-arm64/native（.dylib）+ 同上 include（未实测）
#
# 用法：
#   python3 scripts/fetch-onnxruntime.py          # 装到 build/_third_party/
#   python3 scripts/fetch-onnxruntime.py --version 1.20.1
# ---------------------------------------------------------------------------

import argparse
import io
import os
import shutil
import sys
import tempfile
import urllib.request
import zipfile

VERSION = "1.20.1"
PKG_ID = "Microsoft.ML.OnnxRuntime"
# NuGet flat-container 约定：{小写id}/{小写版本}/{小写id}.{小写版本}.nupkg
NUGET_URL_TMPL = "https://api.nuget.org/v3-flatcontainer/{pkg}/{ver}/{pkg}.{ver}.nupkg"

# 平台 -> (nupkg 里的 native 子目录, 产物目录名)
RUNTIMES = {
    "win32": ("runtimes/win-x64/native", "onnxruntime-win-x64"),
    "linux": ("runtimes/linux-x64/native", "onnxruntime-linux-x64"),
    "darwin": ("runtimes/osx-arm64/native", "onnxruntime-osx-arm64"),
}

# 库文件扩展名（import lib / 动态库）
LIB_EXTS = {".lib", ".so", ".dylib", ".a"}
DLL_EXTS = {".dll", ".so", ".dylib"}


def platform_key():
    if sys.platform.startswith("win"):
        return "win32"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform.startswith("darwin"):
        return "darwin"
    return None


def fetch(url, dest):
    print(f"[fetch] {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "cxk-tools-build"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = resp.read()
    with open(dest, "wb") as f:
        f.write(data)
    print(f"[fetch] downloaded {len(data)} bytes")


def extract(nupkg_path, native_subdir, out_dir):
    """把 nupkg 里的 include/ + native 库解到 out_dir/{include,lib,bin}。"""
    os.makedirs(out_dir, exist_ok=True)
    include_dir = os.path.join(out_dir, "include")
    lib_dir = os.path.join(out_dir, "lib")
    bin_dir = os.path.join(out_dir, "bin")
    os.makedirs(include_dir, exist_ok=True)
    os.makedirs(lib_dir, exist_ok=True)
    os.makedirs(bin_dir, exist_ok=True)

    with zipfile.ZipFile(nupkg_path) as z:
        names = z.namelist()
        # 1) 头文件：build/native/include/*
        for n in names:
            if n.startswith("build/native/include/") and n.endswith(".h"):
                base = os.path.basename(n)
                with z.open(n) as src, open(os.path.join(include_dir, base), "wb") as dst:
                    shutil.copyfileobj(src, dst)

        # 2) native 库：按扩展名分流到 lib/（import lib）或 bin/（运行库）
        for n in names:
            if not n.startswith(native_subdir + "/"):
                continue
            base = os.path.basename(n)
            ext = os.path.splitext(base)[1].lower()
            target = lib_dir if ext in LIB_EXTS else bin_dir
            with z.open(n) as src, open(os.path.join(target, base), "wb") as dst:
                shutil.copyfileobj(src, dst)


def main():
    ap = argparse.ArgumentParser(description="Fetch and unpack ONNX Runtime")
    ap.add_argument("--version", default=VERSION)
    ap.add_argument("--out", default=None,
                    help="output root (default: build/_third_party)")
    args = ap.parse_args()

    key = platform_key()
    if key is None:
        print(f"[error] unsupported platform: {sys.platform}")
        return 1
    native_subdir, dirname = RUNTIMES[key]

    out_root = args.out or os.path.join("build", "_third_party")
    out_dir = os.path.join(out_root, dirname)

    # 已就绪则跳过（幂等）
    if os.path.exists(os.path.join(out_dir, "include", "onnxruntime_cxx_api.h")):
        print(f"[skip] already present: {out_dir}")
        return 0

    os.makedirs(out_root, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="onnxruntime-")
    nupkg = os.path.join(tmp, "onnxruntime.nupkg")
    try:
        pkg_l = PKG_ID.lower()
        ver_l = args.version.lower()
        url = NUGET_URL_TMPL.format(pkg=pkg_l, ver=ver_l)
        fetch(url, nupkg)
        extract(nupkg, native_subdir, out_dir)
    except Exception as e:  # noqa: BLE001
        print(f"[error] {e}")
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"[done] ONNX Runtime unpacked to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
