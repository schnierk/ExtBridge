#!/usr/bin/env python3
"""Reads an ExtBridge I/O trace (macos/Extension/Trace.swift) and checks how
the kernel's write operations were mapped and reported.

    trace_check.py TRACE.log [-v]

For every write operation (MAP lines with the write flag) it compares the
mapped byte ranges with the range of its completion report (DONE), and
lists ranges handed out as unwritten ("U") that no report of the same
operation covers. The summary counts the shapes of the reports: "same"
means the report starts at the first mapping and ends at the last one,
"below" a report that starts earlier (the first page was mapped before),
"gaps" an operation whose mappings leave out ranges in between (pages that
were not dirty). -v prints every operation.
"""
import re
import sys
from collections import defaultdict

BS = 4096
WRITE = 0x200


def parse(path):
    ops = defaultdict(lambda: {"maps": [], "done": [], "ino": None})
    for line in open(path, errors="replace"):
        f = line.split()
        if len(f) < 3 or f[2] not in ("MAP", "DONE"):
            continue
        kv = dict(x.split("=", 1) for x in f[3:] if "=" in x)
        op = int(kv["op"])
        if f[2] == "MAP":
            if not int(kv["fl"]) & WRITE:
                continue
            ext = re.findall(r"(\d+)\+(\d+)@(\d+)([DUH])", line.split(":", 1)[1] if ":" in line else "")
            ops[op]["ino"] = kv["ino"]
            ops[op]["maps"].append((int(kv["off"]), int(kv["len"]), [(int(a), int(b), t) for a, b, _, t in ext]))
        else:
            ops[op]["ino"] = ops[op]["ino"] or kv["ino"]
            ops[op]["done"].append((int(kv["off"]), int(kv["len"]), int(kv["fl"]), kv.get("st")))
    return ops


def main():
    path = sys.argv[1]
    verbose = "-v" in sys.argv
    ops = parse(path)
    shapes = defaultdict(int)
    problems = 0
    for op, o in sorted(ops.items()):
        maps, done = sorted(o["maps"]), o["done"]
        if not maps:
            shapes["report without mapping (op 0: kept mappings)" if op == 0 else "report without mapping"] += 1
            continue
        if not done:
            shapes["MAPPING WITHOUT REPORT"] += 1
            print(f"op {op} ino {o['ino']}: mapped {[(a, n) for a, n, _ in maps]} but never reported")
            continue
        lo, hi = maps[0][0], max(a + n for a, n, _ in maps)
        dlo, dhi = done[0][0], done[0][0] + done[0][1]
        gaps, pos = [], lo
        for a, n, _ in maps:
            if a > pos:
                gaps.append((pos, a))
            pos = max(pos, a + n)
        shape = ("start=" + ("same" if dlo == lo else "below" if dlo < lo else "ABOVE") +
                 " end=" + ("same" if dhi == hi else "above" if dhi > hi else "BELOW") + (" gaps" if gaps else ""))
        if len(done) > 1:
            shape += " SEVERAL REPORTS"
        shapes[shape] += 1
        if verbose:
            print(f"op {op} ino {o['ino']}: maps {[(a, n) for a, n, _ in maps]} report {done} gaps {gaps}")
        # unwritten blocks handed out that the operation's reports do not cover
        cover = sorted((a, a + n) for a, n, fl, st in done if fl & WRITE and st in ("nil", None))
        for off, n, exts in maps:
            for lb, ln, typ in exts:
                if typ != "U":
                    continue
                a, b = max(lb * BS, off), min((lb + ln) * BS, off + n)
                p = a
                for x, y in cover:
                    if y <= p or x >= b:
                        continue
                    if x > p:
                        break
                    p = max(p, y)
                if p < b:
                    problems += 1
                    if problems <= 40:
                        print(f"UNCOVERED op {op} ino {o['ino']}: bytes {p}-{b} (blocks {p // BS}-{(b - 1) // BS}), "
                              f"report {done}")
    for k, v in sorted(shapes.items()):
        print(f"{v:6d}  {k}")
    print(f"{len(ops)} operation(s), {problems} handed-out range(s) outside their operation's report")
    return 1 if problems else 0


sys.exit(main())
