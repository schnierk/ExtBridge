#!/usr/bin/env python3
"""After a simulated power loss the volume must show the state after some
prefix of the operations. Only the file targeted by the single operation
that was in flight may differ (ext4 ordered mode does not make in-place
overwrites atomic - neither does Linux)."""
import os, sys
got_file, snapdir, opsfile = sys.argv[1:4]
got = set(open(got_file).read().splitlines())
ops = open(opsfile).read().splitlines()
n = len(ops)

def paths_of(lines, p):
    return {l for l in lines if l.split('\t', 1)[0] == p}

best = None
for k in range(n + 1):
    fn = os.path.join(snapdir, f'snap_{k}.txt')
    if not os.path.exists(fn):
        continue
    snap = set(open(fn).read().splitlines())
    if snap == got:
        print(f'state = after {k} of {n} operations')
        sys.exit(0)
    if k < n:
        f = ops[k].split('\t')
        if f[0] in ('put', 'write', 'truncate'):
            target = f[2] if f[0] == 'put' else f[1]
            if snap - paths_of(snap, target) == got - paths_of(got, target):
                print(f'state = after {k} of {n} operations, "{f[0]}" on {target} in flight')
                sys.exit(0)
    d = len(snap ^ got)
    if best is None or d < best[0]:
        best = (d, k, sorted(snap - got)[:5], sorted(got - snap)[:5])
print('NO MATCHING STATE. closest prefix', best[1], 'differs by', best[0])
print('  missing:', best[2])
print('  extra:  ', best[3])
sys.exit(1)
