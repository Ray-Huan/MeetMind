#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用 Windows SAPI 合成一段中文会议语音，用于端到端验证。

输出:
  <out>  22.05 kHz / 16 bit / 单声道 WAV
         （刻意选用非 16 kHz，以便同时验证重采样链路）

用法:
  python tools/gen_meeting_audio.py [--out data/fixtures/meeting_zh.wav]
                                    [--gap-ms 800] [--voice <描述关键词>]
"""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
import wave

# SAPI 音频格式枚举：22 kHz / 16 bit / 单声道
SAFT22kHz16BitMono = 22
SSFMCreateForWrite = 3

# 会议脚本：覆盖排期、风险、决议、待办（含人名与截止时间）、预算、收尾
MEETING_SCRIPT = [
    ("大家好，我们现在开始本周的项目周会。", 0),
    ("首先过一下排期，本周的排期整体是可控的。", 0),
    ("主要的风险在于测试资源不足，需要提前协调。", -1),
    ("这个由李四负责跟进测试资源，下周三之前给出结论。", 0),
    ("我们决定把上线时间调整到十月八日。", 0),
    ("请张三在九月二十日前提交测试方案。", -1),
    ("预算方面没有问题，成本控制在计划之内。", 0),
    ("下周我们再同步一次进展，今天就到这里。", 0),
]


def synthesize_segment(sapi, text: str, rate: int, path: str) -> None:
    """用指定语速把一句话合成到独立 WAV 文件。

    注意：SpVoice.GetVoices() 返回的是 SpObjectToken，不能直接设置
    AudioOutputStream；必须把 token 赋给 SpVoice.Voice 后再设置输出流。
    """
    import win32com.client  # type: ignore

    stream = win32com.client.Dispatch("SAPI.SpFileStream")
    fmt = win32com.client.Dispatch("SAPI.SpAudioFormat")
    fmt.Type = SAFT22kHz16BitMono
    stream.Format = fmt
    stream.Open(path, SSFMCreateForWrite, False)
    try:
        sapi.AudioOutputStream = stream
        sapi.Rate = rate
        sapi.Speak(text)
    finally:
        stream.Close()


def pick_chinese_voice(sapi, keyword: str):
    """返回 (token, 描述)；未找到时 token 为 None。"""
    voices = sapi.GetVoices()
    for i in range(voices.Count):
        v = voices.Item(i)
        desc = v.GetDescription()
        if keyword.lower() in desc.lower():
            return v, desc
    return None, ""


def silence_bytes(ms: int, params) -> bytes:
    frames = int(params.framerate * ms / 1000)
    return b"\x00\x00" * frames * params.nchannels


def main() -> int:
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--out", default=os.path.join(root, "data", "fixtures", "meeting_zh.wav"))
    ap.add_argument("--gap-ms", type=int, default=800, help="句间静音时长")
    ap.add_argument("--lead-ms", type=int, default=1500, help="开头静音时长")
    ap.add_argument("--tail-ms", type=int, default=1500, help="结尾静音时长")
    ap.add_argument("--voice", default="Chinese", help="语音描述关键词")
    args = ap.parse_args()

    try:
        import win32com.client  # type: ignore
    except ImportError:
        print("ERROR: 需要 pywin32（pip install pywin32）", file=sys.stderr)
        return 2

    sapi = win32com.client.Dispatch("SAPI.SpVoice")
    token, desc = pick_chinese_voice(sapi, args.voice)
    if token is None:
        print(f"ERROR: 未找到包含 '{args.voice}' 的语音", file=sys.stderr)
        return 2
    sapi.Voice = token
    print(f"使用语音: {desc}")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="meetmind_tts_")
    parts = []
    try:
        for idx, (line, rate) in enumerate(MEETING_SCRIPT):
            seg_path = os.path.join(tmpdir, f"seg{idx:02d}.wav")
            synthesize_segment(sapi, line, rate, seg_path)
            parts.append(seg_path)

        # 拼接：开头静音 + 各句（句间静音）+ 结尾静音
        with wave.open(parts[0], "rb") as first:
            params = first.getparams()

        with wave.open(args.out, "wb") as out:
            out.setparams(params)
            out.writeframes(silence_bytes(args.lead_ms, params))
            for i, p in enumerate(parts):
                with wave.open(p, "rb") as seg:
                    assert seg.getparams()[:3] == params[:3], "各段音频参数不一致"
                    out.writeframes(seg.readframes(seg.getnframes()))
                if i + 1 < len(parts):
                    out.writeframes(silence_bytes(args.gap_ms, params))
            out.writeframes(silence_bytes(args.tail_ms, params))
    finally:
        for p in parts:
            try:
                os.remove(p)
            except OSError:
                pass
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass

    with wave.open(args.out, "rb") as w:
        dur = w.getnframes() / float(w.getframerate())
        print(f"输出: {args.out}")
        print(f"  {w.getframerate()} Hz / {w.getsampwidth()*8} bit / {w.getnchannels()} 声道 / "
              f"{dur:.1f} 秒 / {len(MEETING_SCRIPT)} 句")

    script_path = os.path.splitext(args.out)[0] + ".script.txt"
    with open(script_path, "w", encoding="utf-8") as fp:
        fp.write("# MeetMind 参考转写脚本（用于回放引擎回归测试）\n")
        for line, _ in MEETING_SCRIPT:
            fp.write(line + "\n")
    print(f"参考脚本: {script_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
