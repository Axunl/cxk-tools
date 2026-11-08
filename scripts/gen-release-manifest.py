#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成发布用的 latest.json 与 SHA256SUMS.txt。

用法：
    python scripts/gen-release-manifest.py \
        --version 0.1.0 --tag v0.1.0 \
        --dir dist --notes release-notes.md \
        --repo Axunl/cxk-tools --out dist

只依赖标准库，CI 与本地都能直接跑。

产物命名是这个脚本的唯一输入约定（与 AGENTS.md「产物命名」一节一致）：
    cxk-tools-<ver>-windows-x64.zip             -> windows-x64 / portable
    cxk-tools-<ver>-windows-x64-setup.exe       -> windows-x64 / installer
    cxk-tools-<ver>-macos-arm64.dmg             -> macos-arm64 / dmg
    cxk-tools-<ver>-macos-x64.dmg               -> macos-x64 / dmg
    cxk-tools-<ver>-linux-x64.tar.gz            -> linux-x64 / tarball
下载地址按 GitHub 的 release 资产规则拼出来，不依赖上传接口的返回值。
"""

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

# 后缀 -> (平台 key, 包类型)。顺序有意义：先匹配更长的 setup.exe，再落到 .exe
SUFFIX_RULES = [
    ("-windows-x64-setup.exe", "windows-x64", "installer"),
    ("-windows-x64.zip", "windows-x64", "portable"),
    ("-macos-arm64.dmg", "macos-arm64", "dmg"),
    ("-macos-x64.dmg", "macos-x64", "dmg"),
    ("-linux-x64.tar.gz", "linux-x64", "tarball"),
    ("-linux-x64.zip", "linux-x64", "tarball"),
]


def classify(name: str, version: str):
    """按命名规则推断 (platform, kind)；不符合规则返回 None。"""
    prefix = f"cxk-tools-{version}"
    if not name.startswith(prefix):
        return None
    rest = name[len(prefix):]
    for suffix, platform, kind in SUFFIX_RULES:
        if rest == suffix:
            return platform, kind
    return None


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 latest.json 与 SHA256SUMS.txt")
    parser.add_argument("--version", required=True, help="不带 v 前缀的版本号，如 0.1.0")
    parser.add_argument("--tag", required=True, help="git tag，如 v0.1.0")
    parser.add_argument("--dir", required=True, help="存放发布产物的目录")
    parser.add_argument("--notes", default="", help="release notes 文件（markdown），可省略")
    parser.add_argument("--repo", default="Axunl/cxk-tools", help="GitHub 仓库 owner/name")
    parser.add_argument("--out", default="", help="输出目录，默认与 --dir 相同")
    args = parser.parse_args()

    src_dir = os.path.abspath(args.dir)
    out_dir = os.path.abspath(args.out or args.dir)
    if not os.path.isdir(src_dir):
        print(f"[error] 目录不存在: {src_dir}", file=sys.stderr)
        return 1

    notes = ""
    if args.notes and os.path.isfile(args.notes):
        with open(args.notes, "r", encoding="utf-8") as handle:
            notes = handle.read().strip()

    # 只看顶层文件：dist/ 下还有 windows-x64/ 之类的 staging 目录，不该进清单
    assets = []
    unmatched = []
    for name in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, name)
        if not os.path.isfile(path):
            continue
        if name in ("latest.json", "SHA256SUMS.txt"):
            continue
        guess = classify(name, args.version)
        if not guess:
            if name.startswith("cxk-tools-"):
                unmatched.append(name)
            continue
        platform, kind = guess
        assets.append(
            {
                "platform": platform,
                "kind": kind,
                "name": name,
                "url": f"https://github.com/{args.repo}/releases/download/{args.tag}/{name}",
                "sha256": sha256_of(path),
                "size": os.path.getsize(path),
            }
        )

    if not assets:
        print("[error] 没有找到符合命名规则的产物；检查 --version 与文件名是否一致", file=sys.stderr)
        return 1

    for name in unmatched:
        # 只报警不失败：可能是人工放进去的安装器日志之类
        print(f"[warn] 文件名不符合规则，已跳过: {name}", file=sys.stderr)

    manifest = {
        "version": args.version,
        "tag": args.tag,
        "published_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "page_url": f"https://github.com/{args.repo}/releases/tag/{args.tag}",
        "notes": notes,
        "assets": assets,
    }

    os.makedirs(out_dir, exist_ok=True)
    manifest_path = os.path.join(out_dir, "latest.json")
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")

    sums_path = os.path.join(out_dir, "SHA256SUMS.txt")
    with open(sums_path, "w", encoding="utf-8", newline="\n") as handle:
        for asset in assets:
            handle.write(f"{asset['sha256']}  {asset['name']}\n")

    print(f"MANIFEST_OK version={args.version} assets={len(assets)}")
    for asset in assets:
        print(f"  {asset['platform']:<12} {asset['kind']:<9} {asset['size']:>10}  {asset['name']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
