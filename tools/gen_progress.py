#!/usr/bin/env python3
"""Render docs/linux-port-progress.html from docs/linux-port-progress.json.

The JSON is the single source of truth for the Linux-port progress report;
this script only formats it. Agents update the JSON, never the HTML, so two
agents editing progress concurrently conflict on structured data rather than
on markup.

Usage:  python3 tools/gen_progress.py
"""

import html
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STATE = ROOT / "docs" / "linux-port-progress.json"
OUT = ROOT / "docs" / "linux-port-progress.html"

STATUS = {
    "pending":  ("Pending",      "#6b7280"),
    "building": ("Builder",      "#3b82f6"),
    "critic":   ("Under critic", "#a855f7"),
    "rework":   ("Rework",       "#f59e0b"),
    "passed":   ("Passed",       "#10b981"),
    "blocked":  ("Blocked",      "#ef4444"),
}


def esc(v):
    return html.escape(str(v)) if v is not None else ""


def status_pill(key):
    label, colour = STATUS.get(key, (key, "#6b7280"))
    return (f'<span class="pill" style="--c:{colour}">{esc(label)}</span>')


def main():
    if not STATE.exists():
        sys.exit(f"missing state file: {STATE}")
    s = json.loads(STATE.read_text())

    comps = s.get("components", [])
    done = sum(1 for c in comps if c.get("status") == "passed")
    total = len(comps)
    pct = round(100 * done / total) if total else 0

    rows = []
    for c in comps:
        notes = c.get("note", "")
        rounds = c.get("rounds", 0)
        rows.append(f"""
        <tr>
          <td class="id">{esc(c.get('id'))}</td>
          <td class="name">{esc(c.get('name'))}
            <div class="bar">{esc(c.get('bar', ''))}</div></td>
          <td>{status_pill(c.get('status', 'pending'))}</td>
          <td class="num">{esc(rounds) if rounds else '&mdash;'}</td>
          <td class="note">{esc(notes)}</td>
        </tr>""")

    metric_rows = []
    for m in s.get("metrics", []):
        metric_rows.append(f"""
        <tr>
          <td class="name">{esc(m.get('metric'))}</td>
          <td class="num">{esc(m.get('baseline'))}</td>
          <td class="num">{esc(m.get('current'))}</td>
          <td class="num">{esc(m.get('target'))}</td>
          <td class="note">{esc(m.get('note', ''))}</td>
        </tr>""")
    if not metric_rows:
        metric_rows.append(
            '<tr><td colspan="5" class="empty">No measurements recorded yet.</td></tr>')

    log_items = []
    for e in reversed(s.get("log", [])):
        log_items.append(
            f'<li><span class="ts">{esc(e.get("t"))}</span>'
            f'<span class="who">{esc(e.get("who"))}</span>'
            f'<span class="msg">{esc(e.get("msg"))}</span></li>')
    if not log_items:
        log_items.append('<li class="empty">Nothing logged yet.</li>')

    doc = f"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>fwOGAppExplorer — Linux port progress</title>
<style>
  :root {{ color-scheme: dark; --bg:#0d1117; --panel:#161b22; --line:#30363d;
           --fg:#e6edf3; --dim:#8b949e; --accent:#58a6ff; }}
  * {{ box-sizing: border-box; }}
  body {{ margin:0; padding:2rem 1.25rem 4rem; background:var(--bg); color:var(--fg);
          font:15px/1.55 ui-sans-serif,-apple-system,"Segoe UI",Roboto,sans-serif; }}
  .wrap {{ max-width: 1100px; margin: 0 auto; }}
  h1 {{ font-size:1.5rem; margin:0 0 .25rem; }}
  h2 {{ font-size:1.05rem; margin:2.5rem 0 .75rem; color:var(--accent);
        text-transform:uppercase; letter-spacing:.06em; }}
  .sub {{ color:var(--dim); margin:0 0 1.5rem; font-size:.9rem; }}
  .progress {{ background:var(--panel); border:1px solid var(--line);
               border-radius:10px; padding:1rem 1.25rem; }}
  .track {{ height:10px; background:#21262d; border-radius:99px; overflow:hidden;
            margin:.6rem 0 .4rem; }}
  .fill {{ height:100%; background:linear-gradient(90deg,#10b981,#58a6ff);
           width:{pct}%; transition:width .4s; }}
  .counts {{ display:flex; justify-content:space-between; font-size:.85rem;
             color:var(--dim); }}
  table {{ width:100%; border-collapse:collapse; background:var(--panel);
           border:1px solid var(--line); border-radius:10px; overflow:hidden; }}
  th {{ text-align:left; font-size:.72rem; text-transform:uppercase;
        letter-spacing:.07em; color:var(--dim); font-weight:600;
        padding:.7rem .9rem; border-bottom:1px solid var(--line); }}
  td {{ padding:.7rem .9rem; border-bottom:1px solid var(--line);
        vertical-align:top; }}
  tr:last-child td {{ border-bottom:none; }}
  .id {{ font-family:ui-monospace,monospace; color:var(--dim); width:3.2rem; }}
  .name {{ font-weight:600; }}
  .bar {{ font-weight:400; color:var(--dim); font-size:.82rem; margin-top:.2rem; }}
  .num {{ font-family:ui-monospace,monospace; text-align:right;
          white-space:nowrap; }}
  .note {{ color:var(--dim); font-size:.86rem; }}
  .empty {{ color:var(--dim); font-style:italic; }}
  .pill {{ display:inline-block; padding:.15rem .55rem; border-radius:99px;
           font-size:.75rem; font-weight:600; white-space:nowrap;
           color:var(--c); border:1px solid var(--c);
           background:color-mix(in srgb, var(--c) 14%, transparent); }}
  ul.log {{ list-style:none; margin:0; padding:0; background:var(--panel);
            border:1px solid var(--line); border-radius:10px; }}
  ul.log li {{ display:flex; gap:.9rem; padding:.55rem .9rem;
               border-bottom:1px solid var(--line); font-size:.87rem; }}
  ul.log li:last-child {{ border-bottom:none; }}
  .ts {{ font-family:ui-monospace,monospace; color:var(--dim);
         white-space:nowrap; }}
  .who {{ color:var(--accent); min-width:7rem; }}
  .msg {{ flex:1; }}
  footer {{ margin-top:2.5rem; color:var(--dim); font-size:.8rem; }}
  @media (max-width:720px) {{ .bar,.note {{ display:none; }} }}
</style></head><body><div class="wrap">

<h1>fwOGAppExplorer &mdash; Linux port</h1>
<p class="sub">{esc(s.get('subtitle',''))} &middot; branch
  <code>{esc(s.get('branch','linux-support'))}</code> &middot; updated
  {esc(s.get('updated',''))}</p>

<div class="progress">
  <strong>{done} of {total} components passed</strong>
  <div class="track"><div class="fill"></div></div>
  <div class="counts"><span>{pct}% complete</span>
    <span>{esc(s.get('phase',''))}</span></div>
</div>

<h2>Components</h2>
<table>
  <thead><tr><th>#</th><th>Component / bar</th><th>Status</th>
    <th class="num">Rounds</th><th>Latest finding</th></tr></thead>
  <tbody>{''.join(rows)}</tbody>
</table>

<h2>Measurements</h2>
<table>
  <thead><tr><th>Metric</th><th class="num">Baseline</th>
    <th class="num">Current</th><th class="num">Bar</th><th>Note</th></tr></thead>
  <tbody>{''.join(metric_rows)}</tbody>
</table>

<h2>Activity</h2>
<ul class="log">{''.join(log_items)}</ul>

<footer>Auto-refreshes every 30s. Generated by
  <code>tools/gen_progress.py</code> from
  <code>docs/linux-port-progress.json</code>.</footer>
</div></body></html>
"""
    OUT.write_text(doc)
    print(f"wrote {OUT} ({done}/{total} passed)")


if __name__ == "__main__":
    main()
