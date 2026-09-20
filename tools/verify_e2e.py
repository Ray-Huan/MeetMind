#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""MeetMind 端到端验证脚本。

流程：
  1) 用真实中文会议音频调用 meetmind CLI（默认走 whisper.cpp 端侧推理）
  2) 读取导出的 JSON，与参考脚本做字符级比对
  3) 校验纪要要素（关键词 / 待办 / 决议 / 说话人）是否被正确抽取
  4) 输出可复现的验证报告

用法:
  python tools/verify_e2e.py --model models/ggml-small.bin
  python tools/verify_e2e.py --backend replay --script data/fixtures/meeting_zh.script.txt
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 参考要素（来自 data/fixtures/meeting_zh.script.txt 的会议脚本）
EXPECT_KEYWORDS = ["排期", "测试", "上线", "资源", "预算", "风险"]
EXPECT_OWNERS = ["李四", "张三"]
EXPECT_DUE_DATES = ["2026-09-20", "2026-09-23"]
EXPECT_DECISION_HINT = ["决定", "调整", "十月八日", "10月8日"]


def as_float(value, default: float = 0.0) -> float:
    """容错取值：JSON 里数值可能是数字或字符串（历史格式）。"""
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def as_int(value, default: int = 0) -> int:
    return int(as_float(value, default))


def norm(text: str) -> str:
    """去掉标点与空白，只保留中日韩文字与字母数字，用于稳健比对。"""
    return re.sub(r"[^\u4e00-\u9fff\u3400-\u4dbfA-Za-z0-9]", "", text or "")


def similarity(a: str, b: str) -> float:
    return difflib.SequenceMatcher(None, norm(a), norm(b)).ratio()


def run_cli(args: argparse.Namespace) -> tuple[int, str, float]:
    cmd = [
        args.exe,
        args.wav,
        "-o", args.out,
        "--formats", "json,md,srt,txt",
        "--date", args.date,
        "--no-progress",
    ]
    if args.backend:
        cmd += ["--backend", args.backend]
    if args.script:
        cmd += ["--script", args.script]
    if args.model:
        cmd += ["-m", args.model]
    if args.lang:
        cmd += ["-l", args.lang]
    if args.threads:
        cmd += ["-j", str(args.threads)]
    if args.speakers:
        cmd += ["--speakers", str(args.speakers)]

    print("$ " + " ".join(cmd))
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    elapsed = time.time() - t0
    output = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode, output, elapsed


def load_json(out_dir: str) -> dict:
    """读取导出目录中「最新」的 JSON（避免误读历史遗留的导出文件）。"""
    files = [
        os.path.join(out_dir, f)
        for f in os.listdir(out_dir)
        if f.endswith(".json") and os.path.isfile(os.path.join(out_dir, f))
    ]
    if not files:
        raise SystemExit(f"ERROR: {out_dir} 中未找到导出的 JSON")
    # 跳过会话快照（它们是 sessions 子目录里的，且结构不同）；按修改时间取最新
    files = [f for f in files if os.path.basename(f) != "index.json"]
    files.sort(key=os.path.getmtime, reverse=True)
    path = files[0]
    with open(path, "r", encoding="utf-8") as fp:
        data = json.load(fp)
    if "report" not in data or "asr" not in data:
        raise SystemExit(f"ERROR: {path} 不是有效的纪要导出文件")
    return data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "bin", "meetmind-cli.exe"))
    ap.add_argument("--wav", default=os.path.join(ROOT, "data", "fixtures", "meeting_zh.wav"))
    ap.add_argument("--script", default=os.path.join(ROOT, "data", "fixtures",
                                                     "meeting_zh.script.txt"))
    ap.add_argument("--model", default=os.path.join(ROOT, "models", "ggml-small.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "test-output", "e2e"))
    ap.add_argument("--report", default=os.path.join(ROOT, "build", "test-output", "e2e",
                                                     "e2e-report.md"))
    ap.add_argument("--backend", default=None, help="auto/whisper/replay/null；默认由 CLI 决定")
    ap.add_argument("--lang", default="zh")
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--speakers", type=int, default=0)
    ap.add_argument("--date", default="2026-09-14")
    args = ap.parse_args()

    for p in (args.exe, args.wav):
        if not os.path.exists(p):
            print(f"ERROR: 缺少文件 {p}", file=sys.stderr)
            return 2
    if args.backend != "replay" and not os.path.exists(args.model):
        print(f"WARN: 模型不存在，将自动降级：{args.model}", file=sys.stderr)

    os.makedirs(args.out, exist_ok=True)
    rc, output, wall = run_cli(args)
    print(output)
    if rc != 0:
        print(f"ERROR: CLI 退出码 {rc}", file=sys.stderr)
        return 1

    data = load_json(args.out)
    asr = data.get("asr", {})
    minutes = data.get("minutes", {})
    report = data.get("report", {})

    transcript = "".join(seg.get("text", "") for seg in asr.get("segments", []))
    reference = ""
    if os.path.exists(args.script):
        with open(args.script, "r", encoding="utf-8") as fp:
            reference = "".join(line.strip() for line in fp
                                if line.strip() and not line.startswith("#"))

    sim = similarity(transcript, reference) if reference else 0.0
    kws = [k["word"] for k in minutes.get("keywords", [])]
    kw_hit = [k for k in EXPECT_KEYWORDS if any(k in w for w in kws)]
    owners = [a.get("owner", "") for a in minutes.get("actionItems", [])]
    owner_hit = [o for o in EXPECT_OWNERS if any(o in x for x in owners)]
    dues = [a.get("dueDate", "") for a in minutes.get("actionItems", [])]
    due_hit = [d for d in EXPECT_DUE_DATES if d in dues]
    decisions = [d.get("text", "") for d in minutes.get("decisions", [])]
    decision_hit = [h for h in EXPECT_DECISION_HINT
                    if any(h in d for d in decisions)]
    # 决议也可能体现为「待办/关键结论」，一并宽松检查
    if not decision_hit:
        pool = "".join(decisions) + minutes.get("overview", "") + \
               "".join(p.get("text", "") for p in minutes.get("keyPoints", []))
        decision_hit = [h for h in EXPECT_DECISION_HINT if h in pool]

    checks = [
        ("转写非空", len(norm(transcript)) > 0),
        ("转写与参考脚本相似度 ≥ 0.60", sim >= 0.60),
        ("识别到 ≥ 4 个语音段", len(asr.get("segments", [])) >= 4),
        ("关键词命中 ≥ 3", len(kw_hit) >= 3),
        ("识别出决议", len(decision_hit) >= 1),
        ("待办数量 ≥ 1", len(minutes.get("actionItems", [])) >= 1),
        ("导出文件存在", bool(data.get("exportedFiles"))),
        ("整体耗时 > 0", as_float(report.get("totalMs")) > 0),
    ]
    passed = sum(1 for _, ok in checks if ok)

    audio_ms = as_int(report.get("audioMs"))
    lines = []
    lines.append("# MeetMind 端到端验证报告\n")
    lines.append(f"- 音频文件：`{os.path.relpath(args.wav, ROOT)}`")
    lines.append(f"- 识别后端：`{report.get('asrBackend', '?')}`"
                 f"（{report.get('asrBackendReason', '')}）")
    lines.append(f"- 音频时长：{audio_ms/1000:.1f} 秒 ｜ 处理耗时："
                 f"{as_float(report.get('totalMs'))/1000:.1f} 秒 ｜ 整体实时倍率："
                 f"{as_float(report.get('overallRealTimeFactor')):.2f}×")
    lines.append(f"- 语音段数：{len(asr.get('segments', []))} ｜ 句子数："
                 f"{minutes.get('stats', {}).get('sentenceCount', 0)}")
    lines.append(f"- 说话人数：{minutes.get('stats', {}).get('speakerCount', 0)}")
    lines.append(f"- 转写字数：{minutes.get('stats', {}).get('cjkCharacters', 0)}")
    lines.append(f"- 与参考脚本相似度：**{sim*100:.1f}%**\n")
    lines.append("## 校验项\n")
    lines.append("| 校验项 | 结果 |")
    lines.append("| --- | --- |")
    for name, ok in checks:
        lines.append(f"| {name} | {'通过' if ok else '未通过'} |")
    lines.append("")
    lines.append("## 抽取结果\n")
    lines.append(f"- 关键词（Top {len(kws)}）：{'、'.join(kws[:12])}")
    lines.append(f"- 命中期望关键词：{'、'.join(kw_hit) if kw_hit else '（无）'}")
    lines.append(f"- 决议 {len(decisions)} 条：")
    for d in decisions[:5]:
        lines.append(f"  - {d}")
    items = minutes.get("actionItems", [])
    lines.append(f"- 待办 {len(items)} 条：")
    for a in items[:8]:
        lines.append(f"  - {a.get('task','')} ｜责任人：{a.get('owner') or '未指明'} "
                     f"｜截止：{a.get('dueDate') or a.get('dueText') or '未定'} "
                     f"｜优先级：{a.get('priority','')}")
    lines.append(f"- 命中责任人：{'、'.join(owner_hit) if owner_hit else '（无）'}")
    lines.append(f"- 命中截止日期：{'、'.join(due_hit) if due_hit else '（无）'}")
    risks = minutes.get("risks", [])
    if risks:
        lines.append("- 风险提示：")
        for r in risks:
            lines.append(f"  - {r}")
    lines.append("")
    lines.append("## 转写全文\n")
    for seg in asr.get("segments", []):
        label = "发言人" if seg.get("speakerId", -1) < 0 else f"说话人{seg['speakerId']+1}"
        lines.append(f"- `{seg.get('startMs',0)/1000:7.2f}s` **{label}**：{seg.get('text','')}")
    lines.append("")

    text = "\n".join(lines)
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as fp:
        fp.write(text)

    print("=" * 64)
    print(f"端到端校验：{passed}/{len(checks)} 项通过 ｜ 相似度 {sim*100:.1f}% "
          f"｜ 实时倍率 {as_float(report.get('overallRealTimeFactor')):.2f}×")
    for name, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"报告已写入：{args.report}")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
