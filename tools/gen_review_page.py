#!/usr/bin/env python3
"""MeetMind 转写审核页生成器。

把软件导出的结果 JSON 转成一个「单文件审核网页」，供人工复核/修改：
  - 逐句点播放复核（内嵌 <audio> 定位到该句起止时间）
  - 文本直接编辑
  - 相邻句合并
  - 说话人修改
  - 导出修改后的结果 JSON（格式与原 result.json 一致，可直接传回软件重新生成纪要）

用法:
  python tools/gen_review_page.py <result.json> [--audio <音频文件>] [-o <输出目录>]

示例:
  python tools/gen_review_page.py output_final_gpu/xxx.json --audio 多人对话商务座谈会.part11_爱给网_aigei_com.wav
"""
import argparse
import json
import os
import shutil
import sys

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MeetMind · 转写审核</title>
<style>
:root{--accent:#2563eb;--bg:#f4f6f9;--card:#ffffff;--text:#1e293b;--muted:#64748b;--border:#e2e8f0;}
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:"Microsoft YaHei","Segoe UI",sans-serif;background:var(--bg);color:var(--text);padding:24px;max-width:960px;margin:0 auto;}
header{position:sticky;top:0;background:var(--bg);z-index:10;padding:12px 0;border-bottom:1px solid var(--border);margin-bottom:16px;}
h1{font-size:20px;font-weight:600;}
.meta{color:var(--muted);font-size:13px;margin-top:6px;}
.toolbar{display:flex;gap:10px;align-items:center;margin-top:12px;flex-wrap:wrap;}
button{font-family:inherit;border:1px solid var(--border);background:var(--card);color:var(--text);border-radius:8px;padding:8px 16px;cursor:pointer;font-size:13px;transition:.15s;}
button:hover{border-color:var(--accent);color:var(--accent);}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff;}
button.primary:hover{background:#1d4ed8;}
#status{color:var(--muted);font-size:13px;}
.seg{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:12px 14px;margin-bottom:10px;display:flex;gap:12px;align-items:flex-start;transition:.15s;}
.seg.playing{border-color:var(--accent);box-shadow:0 0 0 3px rgba(37,99,235,.12);}
.play-btn{flex:none;width:34px;height:34px;border-radius:50%;display:flex;align-items:center;justify-content:center;background:#eef2ff;color:var(--accent);border:none;cursor:pointer;font-size:14px;}
.play-btn:hover{background:#dbeafe;}
.seg.playing .play-btn{background:var(--accent);color:#fff;}
.col{display:flex;flex-direction:column;gap:6px;flex:1;min-width:0;}
.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;}
.time{color:var(--muted);font-size:12px;white-space:nowrap;font-variant-numeric:tabular-nums;}
.speaker{font-size:12px;font-weight:600;border-radius:999px;padding:2px 10px;color:#fff;white-space:nowrap;}
.speaker select{background:inherit;color:inherit;border:none;font:inherit;outline:none;cursor:pointer;padding:0;}
.text{outline:none;line-height:1.65;font-size:15px;border-radius:6px;padding:2px 4px;margin:-2px -4px;}
.text:focus{background:#f1f5f9;}
.text:empty::before{content:"（空）";color:#cbd5e1;}
.ops{flex:none;display:flex;flex-direction:column;gap:4px;}
.ops button{font-size:12px;padding:3px 8px;border-radius:6px;}
.empty{text-align:center;color:var(--muted);padding:60px 0;}
footer{margin-top:20px;color:var(--muted);font-size:12px;text-align:center;}
</style>
</head>
<body>
<header>
  <h1>MeetMind · 转写审核</h1>
  <div class="meta" id="meta"></div>
  <div class="toolbar">
    <button id="playall">▶ 从头播放</button>
    <button id="export" class="primary">导出修改结果</button>
    <span id="status"></span>
  </div>
</header>
<main id="list"></main>
<audio id="audio" preload="auto"></audio>
<footer>逐句点击 ▶ 复核音频 · 点击文字直接修改 · ↑ 合并到上一句 · 说话人下拉可改 · 导出后传回软件重新生成纪要</footer>
<script>
const DATA = @@DATA@@;
const AUDIO_SRC = "@@AUDIO@@";
const SPEAKER_LABELS = @@LABELS@@;

const audio = document.getElementById('audio');
const list = document.getElementById('list');
const meta = document.getElementById('meta');
const status = document.getElementById('status');

// 从完整 result.json 提取 segments 与 speakerLabels 工作副本
let segments = (DATA.asr && DATA.asr.segments ? DATA.asr.segments : []).map(function(s, i){
  return { idx:i, startMs:s.startMs, endMs:s.endMs, text:s.text||'', speakerId:s.speakerId>=0?s.speakerId:0 };
});
let labels = SPEAKER_LABELS && SPEAKER_LABELS.length ? SPEAKER_LABELS.slice() : ['说话人1'];
let playingIdx = -1;

const SP_COLORS = ['#2563eb','#db2777','#059669','#d97706','#7c3aed','#dc2626','#0891b2','#65a30d'];

function fmt(ms){ const t=Math.round(ms/1000); const m=Math.floor(t/60); const s=t%60; return (m<10?'0':'')+m+':'+(s<10?'0':'')+s; }
function spColor(i){ return SP_COLORS[((i%SP_COLORS.length)+SP_COLORS.length)%SP_COLORS.length]; }
function spLabel(i){ return labels[i] || ('说话人'+(i+1)); }

function render(){
  meta.textContent = (DATA.title||'未命名') + ' · ' + (DATA.meetingDate||'') + ' · ' + segments.length + ' 句' +
      (DATA.asr&&DATA.asr.audioMs?(' · 总时长 '+(Math.round(DATA.asr.audioMs/60000))+' 分钟'):'');
  list.innerHTML = '';
  if(!segments.length){ list.innerHTML = '<div class="empty">没有转写段落</div>'; return; }
  segments.forEach(function(seg, i){
    const div = document.createElement('div');
    div.className = 'seg' + (i===playingIdx?' playing':'');
    div.dataset.i = i;

    // 播放按钮
    const play = document.createElement('button');
    play.className = 'play-btn';
    play.title = '播放/停止';
    play.textContent = i===playingIdx ? '■' : '▶';
    play.onclick = function(){ togglePlay(i); };

    // 说话人（可改）
    const sp = document.createElement('span');
    sp.className = 'speaker';
    sp.style.background = spColor(seg.speakerId);
    const sel = document.createElement('select');
    sel.title = '修改说话人';
    const n = Math.max(labels.length, seg.speakerId+1, 4);
    for(let k=0;k<n;k++){ const o=document.createElement('option'); o.value=k; o.textContent=spLabel(k); if(k===seg.speakerId) o.selected=true; sel.appendChild(o); }
    sel.onchange = function(){ seg.speakerId = parseInt(sel.value,10); render(); };
    sp.appendChild(sel);

    // 时间
    const time = document.createElement('span');
    time.className = 'time';
    time.textContent = fmt(seg.startMs) + ' – ' + fmt(seg.endMs);

    const row = document.createElement('div'); row.className='row';
    row.appendChild(sp); row.appendChild(time);

    // 文本（可编辑）
    const text = document.createElement('div');
    text.className = 'text';
    text.contentEditable = 'true';
    text.textContent = seg.text;
    text.onblur = function(){ seg.text = text.textContent.trim(); };
    text.onkeydown = function(e){ if(e.key==='Escape'){ text.blur(); } };

    const col = document.createElement('div'); col.className='col';
    col.appendChild(row); col.appendChild(text);

    // 操作
    const ops = document.createElement('div'); ops.className='ops';
    const up = document.createElement('button');
    up.textContent = '↑ 合并';
    up.title = '合并到上一句';
    up.disabled = (i===0);
    up.onclick = function(){ mergeUp(i); };
    const del = document.createElement('button');
    del.textContent = '删';
    del.title = '删除本句';
    del.onclick = function(){ segments.splice(i,1); render(); };
    ops.appendChild(up); ops.appendChild(del);

    div.appendChild(play); div.appendChild(col); div.appendChild(ops);
    list.appendChild(div);
  });
}

function togglePlay(i){
  if(playingIdx === i){ audio.pause(); playingIdx=-1; render(); return; }
  const seg = segments[i];
  audio.src = AUDIO_SRC;
  audio.currentTime = seg.startMs/1000;
  audio.play().catch(function(){ status.textContent='音频无法播放：' + AUDIO_SRC; });
  playingIdx = i;
  render();
}
audio.addEventListener('timeupdate', function(){
  if(playingIdx>=0 && playingIdx<segments.length){
    const seg = segments[playingIdx];
    if(audio.currentTime >= seg.endMs/1000){ audio.pause(); playingIdx=-1; render(); }
  }
});
audio.addEventListener('ended', function(){ playingIdx=-1; render(); });
document.getElementById('playall').onclick = function(){
  audio.src = AUDIO_SRC; audio.currentTime = segments.length?segments[0].startMs/1000:0; audio.play().catch(function(){});
  playingIdx=-1; render();
};

function mergeUp(i){
  if(i<=0 || i>=segments.length) return;
  const a = segments[i-1], b = segments[i];
  a.endMs = Math.max(a.endMs, b.endMs);
  a.text = (a.text + (a.text && b.text ? '' : '') + b.text).trim();
  segments.splice(i,1);
  render();
}

function exportJson(){
  // 以原 result.json 为底，回写修改后的 segments 与 speakerLabels
  const out = JSON.parse(JSON.stringify(DATA));
  out.schemaVersion = '1.0';
  out.speakerLabels = labels;
  out.asr = out.asr || {};
  out.asr.segments = segments.map(function(s){
    return { startMs:s.startMs, endMs:s.endMs, durationMs:Math.max(0,s.endMs-s.startMs), text:s.text, confidence:1.0, noSpeechProb:0, speakerId:s.speakerId };
  });
  out.asr.segmentCount = segments.length;
  const blob = new Blob([JSON.stringify(out, null, 2)], {type:'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = (DATA.title||'review') + '_reviewed.json';
  a.click();
  URL.revokeObjectURL(a.href);
  status.textContent = '已导出 ' + segments.length + ' 句（' + a.download + '）';
}

document.getElementById('export').onclick = exportJson;
render();
</script>
</body>
</html>
"""


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 MeetMind 转写审核网页")
    ap.add_argument("result_json", help="软件导出的结果 JSON 文件")
    ap.add_argument("--audio", help="原始音频文件（用于逐句播放复核）")
    ap.add_argument("-o", "--out", help="输出目录（默认在 JSON 同目录下建 <名>_review）")
    args = ap.parse_args()

    if not os.path.isfile(args.result_json):
        print(f"ERROR: 找不到 {args.result_json}", file=sys.stderr)
        return 2

    with open(args.result_json, "r", encoding="utf-8") as f:
        data = json.load(f)

    base = os.path.splitext(os.path.basename(args.result_json))[0]
    outdir = args.out or os.path.join(os.path.dirname(os.path.abspath(args.result_json)), base + "_review")
    os.makedirs(outdir, exist_ok=True)

    audio_name = ""
    if args.audio:
        if not os.path.isfile(args.audio):
            print(f"WARN: 音频文件不存在 {args.audio}，网页将无法播放", file=sys.stderr)
        else:
            audio_name = os.path.basename(args.audio)
            shutil.copyfile(args.audio, os.path.join(outdir, audio_name))
            print(f"已复制音频: {audio_name}")

    labels = data.get("speakerLabels") or []
    html = (HTML_TEMPLATE
            .replace("@@DATA@@", json.dumps(data, ensure_ascii=False))
            .replace("@@AUDIO@@", audio_name)
            .replace("@@LABELS@@", json.dumps(labels, ensure_ascii=False)))

    out_path = os.path.join(outdir, "review.html")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)

    seg_n = len((data.get("asr") or {}).get("segments") or [])
    print(f"审核网页已生成: {out_path}")
    print(f"  句子数: {seg_n} | 说话人: {labels or '（无）'} | 音频: {audio_name or '（未提供）'}")
    print(f"  打开方式: 双击 review.html（建议与音频文件同目录）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
