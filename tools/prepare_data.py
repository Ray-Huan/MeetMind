#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""从 jieba 词典生成 MeetMind 内置中文词典。

输入:  <site-packages>/jieba/dict.txt   (词 词频 词性)
输出:  data/lexicon_zh.txt              (词<TAB>词频<TAB>词性)
筛选:
  * 词频 >= MIN_FREQ
  * 词长 1..12
  * 允许纯 CJK / 纯 ASCII(长度>=2) / CJK 与 ASCII 数字混排
用法:  python tools/prepare_data.py [--jieba-dict PATH] [--out PATH]
"""
from __future__ import annotations

import argparse
import os
import re
import sys

DEFAULT_DICT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "..", "..", "..", "..",
)

CJK = r"\u4e00-\u9fff\u3400-\u4dbf"
RE_CJK = re.compile(f"[{CJK}]")
RE_ASCII_WORD = re.compile(r"^[A-Za-z][A-Za-z0-9\.\-_+#]*$")
RE_MIXED = re.compile(f"^[{CJK}A-Za-z0-9\\.\\-\\+#]+$")
RE_BAD = re.compile(r"[\s,;!?\"'()\[\]{}<>|/~`@$%^&*=:\\]")

MIN_FREQ = 5
MAX_LEN = 12


def is_valid(word: str, freq: int) -> bool:
    if not word or len(word) > MAX_LEN:
        return False
    if freq < MIN_FREQ:
        return False
    if RE_BAD.search(word):
        return False
    if not RE_MIXED.match(word):
        return False
    has_cjk = bool(RE_CJK.search(word))
    if not has_cjk:
        # 纯 ASCII：只保留长度 >= 2 的形似单词项，过滤单字母噪声
        if not RE_ASCII_WORD.match(word) or len(word) < 2:
            return False
    # 过滤纯数字
    if word.isdigit():
        return False
    return True


def build_t2s(root: str) -> int:
    """生成繁→简字符映射表 data/t2s_zh.txt。

    动机：whisper 等识别模型对中文时常输出繁体字（同一个模型的输出会在繁简之间摇摆）。
    转写后统一转简体，可以显著提升关键词抽取与纪要可读性。

    做法：不依赖第三方库的内部数据结构，直接对 CJK 基本区与扩展 A 的每个码点调用
    zhconv.convert()，保留发生变化的单字映射，得到完整且经过验证的映射表。
    """
    try:
        import zhconv  # type: ignore
    except ImportError:
        print("WARN: 未安装 zhconv，跳过繁简映射生成（pip install zhconv）", file=sys.stderr)
        return 1

    pairs = []
    for cp in list(range(0x4E00, 0xA000)) + list(range(0x3400, 0x4DC0)):
        ch = chr(cp)
        simplified = zhconv.convert(ch, "zh-cn")
        if len(simplified) == 1 and simplified != ch:
            pairs.append((ch, simplified))

    out = os.path.join(root, "data", "t2s_zh.txt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("# MeetMind 繁→简 字符映射表（由 zhconv 生成）\n")
        fp.write("# 格式: 繁体字<TAB>简体字\n")
        fp.write(f"# 映射条数: {len(pairs)}\n")
        for t, s_ in pairs:
            fp.write(f"{t}\t{s_}\n")
    print(f"繁简映射: {len(pairs)} 条 -> {out}")
    return 0


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--jieba-dict", default=None)
    ap.add_argument("--out", default=os.path.join(root, "data", "lexicon_zh.txt"))
    ap.add_argument("--t2s", action="store_true",
                    help="同时生成繁→简映射表 data/t2s_zh.txt")
    args = ap.parse_args()

    if args.t2s:
        build_t2s(root)
        return 0

    src = args.jieba_dict
    if src is None:
        try:
            import jieba  # type: ignore
            src = os.path.join(os.path.dirname(jieba.__file__), "dict.txt")
        except ImportError:
            print("ERROR: 未找到 jieba，请 pip install jieba 或使用 --jieba-dict 指定词典",
                  file=sys.stderr)
            return 2

    if not os.path.exists(src):
        print(f"ERROR: 词典不存在: {src}", file=sys.stderr)
        return 2

    total_raw = 0
    kept = []
    total_freq = 0
    with open(src, "r", encoding="utf-8") as fp:
        for line in fp:
            total_raw += 1
            parts = line.rstrip("\n").split(" ")
            if len(parts) < 2:
                continue
            word = parts[0]
            try:
                freq = int(parts[1])
            except ValueError:
                continue
            pos = parts[2] if len(parts) > 2 else "x"
            if not is_valid(word, freq):
                continue
            kept.append((word, freq, pos))
            total_freq += freq

    kept.sort(key=lambda x: -x[1])
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("# MeetMind 中文词典  格式: 词<TAB>词频<TAB>词性\n")
        fp.write(f"# 词条数: {len(kept)}  词频总和: {total_freq}\n")
        for w, f, p in kept:
            fp.write(f"{w}\t{f}\t{p}\n")

    size_kb = os.path.getsize(args.out) / 1024.0
    print(f"源词条 {total_raw} -> 保留 {len(kept)}  ({size_kb:.1f} KB)")
    print(f"输出: {args.out}")
    print(f"词频总和: {total_freq}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
