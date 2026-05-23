#!/usr/bin/env python3
"""
scripts/permission_log.py
Claude Code 세션 transcript 의 Bash 호출을 3 분류로 추출.

  AUTO     — .claude/settings.json allow 패턴 매치 (prompt 없음)
  APPROVED — allow 미매치이나 정상 실행 (prompt 후 사용자 승인 추정)
  DENIED   — tool_result 에 "has been denied" 마커 존재

사용:
  ./scripts/permission_log.py              # 최신 session 분석
  ./scripts/permission_log.py --all        # 모든 session 통합
  ./scripts/permission_log.py --print      # markdown 만 stdout

한계 (정직):
 - shell-glob 단순 매칭이라 Claude Code 실제 정책과 미세 차이 가능.
 - 다중줄/compound 명령은 공백 단일화 후 비교.
 - DENIED 마커 부재 = 실행된 것으로 추정 → APPROVED 도 추정치.
"""
import argparse
import fnmatch
import glob
import json
import os
import re
import sys
from collections import defaultdict
from datetime import datetime

PROJECT_DIR    = "/home/claudedev/DetectBase"
TRANSCRIPT_DIR = "/home/claudedev/.claude/projects/-home-claudedev-DetectBase"
SETTINGS       = f"{PROJECT_DIR}/.claude/settings.json"
OUT            = f"{PROJECT_DIR}/logs/permission_log.md"
TOPN           = 12


def find_key(obj, key):
    """settings.json 의 어디에 있든 'allow'/'deny' 키의 list 를 모음."""
    out = []
    if isinstance(obj, dict):
        if key in obj and isinstance(obj[key], list):
            out.extend(obj[key])
        for v in obj.values():
            out.extend(find_key(v, key))
    elif isinstance(obj, list):
        for v in obj:
            out.extend(find_key(v, key))
    return out


def load_patterns(settings_path):
    with open(settings_path) as f:
        s = json.load(f)
    raw_allow = find_key(s, "allow")
    raw_deny  = find_key(s, "deny")
    allow = [p[5:-1] for p in raw_allow if isinstance(p, str) and p.startswith("Bash(") and p.endswith(")")]
    deny  = [p[5:-1] for p in raw_deny  if isinstance(p, str) and p.startswith("Bash(") and p.endswith(")")]
    return allow, deny


def match_any(cmd, patterns):
    for p in patterns:
        if fnmatch.fnmatchcase(cmd, p):
            return p
    return None


def iter_events(files):
    for f in files:
        with open(f, "r", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue


def extract_tool_result_text(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        out = []
        for item in content:
            if isinstance(item, dict):
                txt = item.get("text") or item.get("content") or ""
                if isinstance(txt, str):
                    out.append(txt)
        return "".join(out)
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all",   action="store_true", help="모든 session transcript 통합")
    ap.add_argument("--print", dest="print_only", action="store_true", help="stdout 만")
    args = ap.parse_args()

    files = sorted(glob.glob(f"{TRANSCRIPT_DIR}/*.jsonl"),
                   key=os.path.getmtime, reverse=True)
    if not files:
        print("[ERROR] transcript JSONL 미발견", file=sys.stderr)
        sys.exit(1)
    if not args.all:
        files = files[:1]

    allow, _deny = load_patterns(SETTINGS)

    # 1) DENIED tool_use_id 모음
    denied_ids = set()
    for ev in iter_events(files):
        msg = ev.get("message") if isinstance(ev, dict) else None
        if not isinstance(msg, dict):
            continue
        content = msg.get("content")
        if not isinstance(content, list):
            continue
        for c in content:
            if not isinstance(c, dict) or c.get("type") != "tool_result":
                continue
            text = extract_tool_result_text(c.get("content"))
            if "has been denied" in text:
                tid = c.get("tool_use_id")
                if tid:
                    denied_ids.add(tid)

    # 2) Bash tool_use 분류
    cnt_auto     = defaultdict(int)
    cnt_approved = defaultdict(int)
    cnt_denied   = defaultdict(int)
    total_auto = total_approved = total_denied = 0

    for ev in iter_events(files):
        msg = ev.get("message") if isinstance(ev, dict) else None
        if not isinstance(msg, dict):
            continue
        content = msg.get("content")
        if not isinstance(content, list):
            continue
        for c in content:
            if not isinstance(c, dict):
                continue
            if c.get("type") != "tool_use" or c.get("name") != "Bash":
                continue
            cmd_raw = (c.get("input") or {}).get("command", "") or ""
            cmd_oneline = re.sub(r"\s+", " ", cmd_raw).strip()
            key = (cmd_oneline[:60] if cmd_oneline else "(empty)")
            tid = c.get("id")
            if tid in denied_ids:
                cnt_denied[key] += 1
                total_denied += 1
            elif match_any(cmd_oneline, allow):
                cnt_auto[key] += 1
                total_auto += 1
            else:
                cnt_approved[key] += 1
                total_approved += 1

    total = total_auto + total_approved + total_denied
    def pct(n):
        return f"{100.0*n/total:.1f}%" if total else "0%"

    def top_block(title, m):
        lines = [f"### {title}", "", "```"]
        for k, v in sorted(m.items(), key=lambda x: -x[1])[:TOPN]:
            lines.append(f"{v:6d}  {k}")
        lines.append("```")
        lines.append("")
        return "\n".join(lines)

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z").strip() \
          or datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    md = []
    md.append("# Claude Code Bash 권한 흐름 기록 (AUTO / APPROVED / DENIED)\n")
    md.append("> 자동 생성: `scripts/permission_log.py` (직접 편집 X — 다시 실행하면 덮어씀).")
    md.append(f"> 갱신 시각: {now}")
    md.append(f"> 분석 대상: {len(files)} 개 transcript ({'all' if args.all else 'latest'})")
    md.append("> 분류 규칙:")
    md.append("> - **AUTO**     = `.claude/settings.json` allow 패턴 매치 (prompt 없음)")
    md.append("> - **APPROVED** = allow 미매치이나 정상 실행 (prompt 후 승인 추정)")
    md.append("> - **DENIED**   = tool_result 에 'has been denied' 마커")
    md.append("")
    md.append("> 한계: shell-glob 단순 매칭이라 Claude Code 실제 정책과 미세 차이 가능.")
    md.append("> compound 명령(`a && b`)은 단일 패턴 미매치로 APPROVED 분류되곤 함. 추정치.\n")

    md.append("## 총괄\n")
    md.append("| 카테고리 | 총 호출 | 비중 |")
    md.append("|---|---:|---:|")
    md.append(f"| **AUTO**     | {total_auto}     | {pct(total_auto)} |")
    md.append(f"| **APPROVED** | {total_approved} | {pct(total_approved)} |")
    md.append(f"| **DENIED**   | {total_denied}   | {pct(total_denied)} |")
    md.append(f"| **합계**     | {total}          | 100% |\n")

    md.append(top_block(f"APPROVED (prompt 후 허가 추정) — 상위 {TOPN}", cnt_approved))
    md.append(top_block(f"DENIED — 상위 {TOPN}", cnt_denied))
    md.append(top_block(f"AUTO (참고용) — 상위 {TOPN}", cnt_auto))

    md.append("## 활용")
    md.append("- **APPROVED 가 잦은 read-only / idempotent 패턴** → `.claude/settings.json` allow 추가하면 prompt ↓.")
    md.append("- **DENIED** 가 잦은 deny-policy 매치(sed/rm/sudo/docker rm 등)는 의도 — allow 승격 금지.")
    md.append("- **AUTO** 는 이미 자동 — 참고용.\n")
    md.append("## 재실행")
    md.append("```bash")
    md.append("./scripts/permission_log.py           # 최신 session")
    md.append("./scripts/permission_log.py --all     # 모든 session 통합")
    md.append("./scripts/permission_log.py --print   # stdout 만")
    md.append("```")

    md_text = "\n".join(md) + "\n"
    if args.print_only:
        sys.stdout.write(md_text)
    else:
        with open(OUT, "w") as f:
            f.write(md_text)
        print(f"[DONE] {OUT} 갱신 (AUTO={total_auto} APPROVED={total_approved} DENIED={total_denied})")


if __name__ == "__main__":
    main()
