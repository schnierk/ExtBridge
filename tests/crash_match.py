#!/usr/bin/env python3
"""After a simulated power loss the volume must show the state after some
prefix of the operations, with the exceptions ext4's ordered mode allows
(Linux behaves the same):

- the file targeted by the operation in flight may differ in any way
  (an operation is not atomic). So may the target of an earlier "write"
  or "truncate" that a commit (for room in the journal) cut in two;
- data that operations after the last commit wrote into existing blocks
  may already be on the disk although their metadata is not: such files
  keep size, mode and everything else of the matched state, only their
  content may differ ("write" in place, "truncate" zeroing the tail of the
  last block). Several operations can share one transaction, so this can
  be more than the operation in flight.

    crash_match.py GOT_TREE SNAPDIR OPS [STARTED]
    crash_match.py --selftest

STARTED is the number of operations started before the power cut (the
last one was in flight); without it only the first exception applies, for
any operation."""
import os, sys


def paths_of(lines, p):
    return {l for l in lines if l.split('\t', 1)[0] == p}


def overwritten(window):
    """Names (in the state before the window) of the files that the
    operations of the window wrote into in place."""
    alias = []  # (name now, name before the window or None if created in it)
    names = set()

    def before(p):
        for now, old in reversed(alias):
            if p == now or p.startswith(now + '/'):
                if old is None:
                    return None
                p = old + p[len(now):]
        return p
    for op in window:
        f = op.split('\t')
        if f[0] in ('write', 'truncate'):
            p = before(f[1])
            if p is not None:
                names.add(p)
        elif f[0] in ('mv', 'ln'):
            alias.append((f[2], f[1]))
        elif f[0] in ('put', 'mkdir', 'symlink'):
            alias.append((f[-1], None))
    return names


def content_only(snap, got, names):
    """True if snap and got differ only in the content of files in names
    (or of their hard links, which show the same change)."""
    pairs = {}
    for side, lines in ((0, snap - got), (1, got - snap)):
        for l in lines:
            f = l.split('\t')
            if len(f) != 5 or f[1] != 'file':
                return False
            pairs.setdefault(f[0], [None, None])[side] = f
    changed, others = set(), []
    for p, (a, b) in pairs.items():
        if a is None or b is None or a[:4] != b[:4]:
            return False
        (changed.add if p in names else others.append)((a[4], b[4]))
    return all(c in changed for c in others)


def match(got, snaps, ops, started=None):
    """snaps: (k, lines) for the states after k operations, k ascending.
    Returns (matched, message)."""
    n = len(ops)
    best = None
    for k, snap in snaps:
        if snap == got:
            return True, f'state = after {k} of {n} operations'
        s, g, half = snap, got, ''
        f = ops[k].split('\t') if k < n else ['']
        if f[0] in ('put', 'write', 'truncate'):
            target = f[2] if f[0] == 'put' else f[1]
            # "put" syncs at its end, so only the one in flight can be half done
            if started is None or k < started and (f[0] != 'put' or k == started - 1):
                s, g = snap - paths_of(snap, target), got - paths_of(got, target)
                half = f', "{f[0]}" on {target} in flight'
                if s == g:
                    return True, f'state = after {k} of {n} operations{half}'
        if started is not None and k < started:
            names = overwritten(ops[k:started])
            # the whole operation k written in place, or (half) done in any way
            for a, b, h in ((snap, got, ''), (s, g, half)):
                if names and content_only(a, b, names):
                    return True, (f'state = after {k} of {n} operations{h}, in-place data of operations '
                                  f'{k + 1}-{started} already on the disk ({len(names)} file(s) may differ in content)')
        d = len(snap ^ got)
        if best is None or d < best[0]:
            best = (d, k, sorted(snap - got)[:5], sorted(got - snap)[:5])
    if best is None:
        return False, 'NO MATCHING STATE (no snapshots)'
    return False, (f'NO MATCHING STATE. closest prefix {best[1]} differs by {best[0]}\n'
                   f'  missing: {best[2]}\n  extra:   {best[3]}')


def selftest():
    """The exceptions must not hide anything else."""
    def F(p, size, h):
        return f'{p}\tfile\t0644\t{size}\t{h}'

    def D(p):
        return f'{p}\tdir\t0755'
    cases = [
        # (what, ops, states after 0.., got, started, must match)
        ('write in place after a rename', ['put\tx\t/a', 'mv\t/a\t/c', 'write\t/c\t0\tz'],
         [[], [F('/a', 3, 'h1')], [F('/c', 3, 'h1')], [F('/c', 3, 'h2')]], [F('/a', 3, 'hX')], 3, True),
        ('size changed too', ['put\tx\t/a', 'mv\t/a\t/c', 'write\t/c\t0\tz'],
         [[], [F('/a', 3, 'h1')], [F('/c', 3, 'h1')], [F('/c', 3, 'h2')]], [F('/a', 4, 'hX')], 3, False),
        ('a file nobody wrote changed', ['put\tx\t/a', 'put\tx\t/b', 'write\t/a\t0\tz'],
         [[], [F('/a', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1')], [F('/a', 3, 'h2'), F('/b', 3, 'h1')]],
         [F('/a', 3, 'h1'), F('/b', 3, 'hX')], 3, False),
        ('hard link shows the same change', ['put\tx\t/a', 'ln\t/a\t/b', 'write\t/b\t0\tz'],
         [[], [F('/a', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1')], [F('/a', 3, 'h2'), F('/b', 3, 'h2')]],
         [F('/a', 3, 'hX'), F('/b', 3, 'hX')], 3, True),
        ('"hard link" with another change', ['put\tx\t/a', 'ln\t/a\t/b', 'write\t/b\t0\tz'],
         [[], [F('/a', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1')], [F('/a', 3, 'h2'), F('/b', 3, 'h2')]],
         [F('/a', 3, 'hY'), F('/b', 3, 'hX')], 3, False),
        ('the name was given to a new file',
         ['put\tx\t/a', 'rm\t/a', 'put\ty\t/a', 'put\tx\t/q', 'write\t/a\t0\tz', 'mkdir\t/z'],
         [[], [F('/a', 3, 'h1')], [], [F('/a', 5, 'h3')], [F('/a', 5, 'h3'), F('/q', 1, 'hq')],
          [F('/a', 5, 'h4'), F('/q', 1, 'hq')], [F('/a', 5, 'h4'), F('/q', 1, 'hq'), D('/z')]],
         [F('/a', 3, 'hX')], 6, False),
        ('half-done "put" that was not in flight', ['put\tx\t/a', 'put\tx\t/b', 'mkdir\t/z'],
         [[], [F('/a', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1'), D('/z')]],
         [F('/a', 3, 'h1'), F('/b', 1, 'hX')], 3, False),
        ('half-done "put" in flight', ['put\tx\t/a', 'put\tx\t/b', 'mkdir\t/z'],
         [[], [F('/a', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1')], [F('/a', 3, 'h1'), F('/b', 3, 'h1'), D('/z')]],
         [F('/a', 3, 'h1'), F('/b', 1, 'hX')], 2, True),
        ('truncate in a renamed directory', ['mkdir\t/d', 'put\tx\t/d/f', 'mv\t/d\t/e', 'truncate\t/e/f\t1'],
         [[], [D('/d')], [D('/d'), F('/d/f', 3, 'h1')], [D('/e'), F('/e/f', 3, 'h1')], [D('/e'), F('/e/f', 1, 'h5')]],
         [D('/d'), F('/d/f', 3, 'hX')], 4, True),
    ]
    bad = 0
    for what, ops, states, got, started, want in cases:
        ok, msg = match(set(got), [(k, set(s)) for k, s in enumerate(states)], ops, started)
        if ok != want:
            print(f'self-test "{what}": {"matched" if ok else "did not match"} ({msg.splitlines()[0]})')
            bad += 1
    if not bad:
        print(f'{len(cases)} self-test cases')
    return 1 if bad else 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        sys.exit(selftest())
    got_file, snapdir, opsfile = sys.argv[1:4]
    started = int(sys.argv[4]) if len(sys.argv) > 4 else None
    got = set(open(got_file).read().splitlines())
    ops = open(opsfile).read().splitlines()

    def snaps():
        for k in range(len(ops) + 1):
            fn = os.path.join(snapdir, f'snap_{k}.txt')
            if os.path.exists(fn):
                yield k, set(open(fn).read().splitlines())
    ok, msg = match(got, snaps(), ops, started)
    print(msg)
    sys.exit(0 if ok else 1)
