#!/usr/bin/env python3
"""PostToolUse: after a write into this project's memory directory, check that
MEMORY.md and the memory files agree, and show the description of any touched
file whose body records a correction, so a stale description is caught.
Silent (exit 0, no output) when nothing in memory was written."""
import glob
import json
import os
import re
import sys

try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
proj = os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or os.getcwd()
mem = os.path.expanduser("~/.claude/projects/" + os.path.abspath(proj).replace("/", "-") + "/memory")
if not os.path.isdir(mem):
    sys.exit(0)
tool = data.get("tool_name")
ti = data.get("tool_input") or {}
touched = set()
if tool in ("Write", "Edit", "MultiEdit"):
    fp = ti.get("file_path") or ""
    if not fp.startswith(mem):
        sys.exit(0)
    touched.add(os.path.basename(fp))
elif tool == "Bash":
    cmd = ti.get("command") or ""
    if mem not in cmd and "/memory/" not in cmd:
        sys.exit(0)
    if not re.search(r">|\bsed\s+-i|\bpython3?\b|\btee\b|\bcp\b|\bmv\b|\brm\b", cmd):
        sys.exit(0)
    touched.update(re.findall(r"([\w.-]+\.md)\b", cmd))
else:
    sys.exit(0)

index_path = os.path.join(mem, "MEMORY.md")
index = open(index_path, encoding="utf-8").read() if os.path.exists(index_path) else ""
files = {os.path.basename(p) for p in glob.glob(os.path.join(mem, "*.md"))} - {"MEMORY.md"}
links = set(re.findall(r"\]\(([^)]+\.md)\)", index))
msgs = []
for f in sorted(files - links):
    msgs.append(f"MEMORY.md に {f} の行が無い。")
for f in sorted(links - files):
    msgs.append(f"MEMORY.md の {f} はファイルが無い。")
CORR = re.compile(r"Correction|corrected|CONFIRMED|was wrong|訂正|誤り")
# The mistake log itself is full of corrections by design.
SKIP_CORR = {"feedback-verify-before-asserting.md"}
for f in sorted((touched & files) - SKIP_CORR):
    text = open(os.path.join(mem, f), encoding="utf-8").read()
    m = re.match(r"---\n(.*?)\n---\n(.*)", text, re.S)
    if not m:
        msgs.append(f"{f} に frontmatter が無い。")
        continue
    front, body = m.groups()
    if CORR.search(body):
        desc = next((l for l in front.splitlines() if l.startswith("description:")), "description: (無し)")
        line = next((l for l in index.splitlines() if f"({f})" in l), "(MEMORY.md に行が無い)")
        msgs.append(f"{f} の本文に訂正の記録がある。見出しが訂正後の内容か確かめる:\n  {desc}\n  {line}")
if msgs:
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                      "additionalContext": "[メモリの点検] " + "\n".join(msgs)}}, ensure_ascii=False))
sys.exit(0)
