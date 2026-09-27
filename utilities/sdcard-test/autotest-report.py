#!/usr/bin/env python3
"""HDZGOGGLE.log の "AT" 行を集計して、1 回の起動で走った自動テストの結果を表にする。

    autotest-report.py [-v] HDZGOGGLE.log

-v を付けると再開 (resume) の 1 件ごとの行も出す。
"""
import re
import statistics
import sys

KV = re.compile(r"(\w+)=(-?\w+)")


def kv(rest):
    return {k: (int(v) if re.fullmatch(r"-?\d+", v) else v) for k, v in KV.findall(rest)}


def mean(xs):
    return statistics.fmean(xs) if xs else 0.0


def main():
    verbose = "-v" in sys.argv
    paths = [a for a in sys.argv[1:] if a != "-v"]
    if not paths:
        print(__doc__)
        return 2

    per_file = {}
    current = "(none)"
    diag = {"watch away": 0, "reading the output took": 0, "no new picture": 0, "is past": 0, "giving up": 0}
    tunes = []
    resumes_all = []
    drops = []

    def bucket():
        return per_file.setdefault(current, {"pause": [], "step": [], "resume": [], "waitresume": []})

    for path in paths:
        with open(path, errors="replace") as fp:
            for line in fp:
                for key in diag:
                    if "step: " in line and key in line:
                        diag[key] += 1
                m = re.search(r"\] AT (\w+) ?(.*)$", line.rstrip())
                if not m:
                    continue
                kind, rest = m.group(1), m.group(2)
                ts = line[11:23]
                if kind == "cmd":
                    text = rest.split(" ", 1)[1] if " " in rest else ""
                    if text.startswith("open "):
                        current = text.split(None, 1)[1].rsplit("/", 1)[-1]
                        bucket()
                    continue
                if kind == "tune":
                    tunes.append(rest)
                    continue
                if kind == "drop":
                    d = kv(rest)
                    d["_file"] = current
                    drops.append(d)
                    continue
                if kind in ("pause", "step", "resume", "waitresume"):
                    d = kv(rest)
                    d["_ts"] = ts
                    bucket()[kind].append(d)
                    if kind == "resume":
                        d["_file"] = current
                        resumes_all.append(d)

    if not per_file:
        print("AT 行が見つからない (自動テストは走っていない)")
        return 1

    print(f"{'file':<16} {'pauses':>6} {'off':>4} | {'steps':>5} {'1frame%':>7} {'wrong':>5} {'ms':>6} | "
          f"{'resumes':>7} {'ok':>3} {'1seek%':>6} {'hold ms avg/max':>16} {'jump ms avg/max':>16} {'>40':>3}")
    for name, b in per_file.items():
        p = b["pause"]
        off = sum(1 for d in p if d.get("delta", 0) != 0)
        s = b["step"]
        s_ok = [d for d in s if d.get("result") == "ok"]
        one = sum(1 for d in s_ok if abs(abs(d.get("frames", 0)) - 100) <= 10)
        wrong = sum(1 for d in s if d.get("result") == "wrongway")
        s_ms = [d.get("ms", 0) for d in s_ok]
        r = b["resume"]
        r_ok = [d for d in r if d.get("result") == "ok"]
        one_seek = sum(1 for d in r_ok if d.get("seeks") == 1)
        holds = [d.get("hold_ms", 0) for d in r_ok]
        jumps = [abs(d.get("drop_delta", 0)) for d in r]
        big = sum(1 for j in jumps if j > 40)
        print(f"{name:<16} {len(p):>6} {off:>4} | {len(s):>5} {100 * one // len(s_ok) if s_ok else 0:>6}% {wrong:>5} "
              f"{mean(s_ms):>6.0f} | {len(r):>7} {len(r_ok):>3} {100 * one_seek // len(r_ok) if r_ok else 0:>5}% "
              f"{mean(holds):>7.0f}/{max(holds) if holds else 0:<8} {mean(jumps):>7.0f}/{max(jumps) if jumps else 0:<8} {big:>3}")

    print()
    print("diagnostics:", ", ".join(f"{k} {v}" for k, v in diag.items()))
    if tunes:
        print("tunes:", "; ".join(tunes))

    biases = [(d["_file"], d["landed"] - d["ask"]) for d in resumes_all if d.get("landed", -1) >= 0]
    if biases:
        print("landing bias (landed - ask) per file:")
        for name in per_file:
            bs = [b for f, b in biases if f == name]
            if bs:
                print(f"  {name:<16} n={len(bs)} min={min(bs):+d} avg={mean(bs):+.0f} max={max(bs):+d}")

    if drops:
        print("picture off the panel (output after the redraw minus the frame shown), per file:")
        for name in per_file:
            ds = [d.get("delta", 0) for d in drops if d["_file"] == name]
            rs = [d.get("redraw_ms", 0) for d in drops if d["_file"] == name]
            if ds:
                print(f"  {name:<16} n={len(ds)} delta min={min(ds):+d} avg={mean(ds):+.0f} max={max(ds):+d}  "
                      f"redraw avg={mean(rs):.0f} max={max(rs)}ms")

    if verbose:
        print()
        print(f"{'time':<12} {'file':<14} {'target':>7} {'key':>6} {'ask':>6} {'landed':>7} {'first':>6} {'seeks':>5} {'hold':>6} {'jump':>5} result")
        for d in resumes_all:
            print(f"{d['_ts']:<12} {d['_file']:<14} {d.get('target', 0):>7} {d.get('key', 0):>6} {d.get('ask', 0):>6} "
                  f"{d.get('landed', -1):>7} {d.get('first', -1):>6} {d.get('seeks', 0):>5} {d.get('hold_ms', 0):>6} "
                  f"{d.get('drop_delta', 0):>+5} {d.get('result', '')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
