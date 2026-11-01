#!/usr/bin/env python3
"""Random file system operations.

Applies N random but valid operations to a model directory on the host
(MODEL, a real Linux file system) and prints the equivalent `l2m batch`
commands. Afterwards the l2m image must contain exactly what MODEL
contains. Payload files for `put` are written to PAYLOAD.

usage: gen_ops.py MODEL PAYLOAD SEED N [PREFIX]
"""
import os, random, sys

model, payload, seed, n = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
prefix = sys.argv[5] if len(sys.argv) > 5 else ''
rnd = random.Random(seed)
os.makedirs(payload, exist_ok=True)
out = []
pcount = [0]
SNAP = os.environ.get('SNAPDIR')
SMALL = os.environ.get('SMALL') == '1'
if SNAP:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ref_tree
    os.makedirs(SNAP, exist_ok=True)

def snapshot():
    lines = []
    ref_tree.walk(model, '', lines.append)
    with open(os.path.join(SNAP, f'snap_{len(out)}.txt'), 'w') as f:
        f.write('\n'.join(lines) + ('\n' if lines else ''))

def emit(*a):
    out.append('\t'.join(str(x) for x in a))
    if SNAP:
        snapshot()

def m(p):
    return os.path.join(model, p.lstrip('/'))

def name():
    alphabet = 'abcdefghijklmnopqrstuvwxyzABC0123456789_-. äöü'
    k = rnd.choice([1, 3, 8, 20, 60, 120, 255])
    s = ''.join(rnd.choice(alphabet) for _ in range(rnd.randint(1, k))).strip()
    s = s.encode()[:255].decode(errors='ignore').strip()
    if s in ('', '.', '..') or s.startswith('.') and set(s) == {'.'}:
        s = 'n' + str(rnd.randint(0, 1 << 30))
    return s

def all_paths():
    files, dirs = [], [prefix or '/']
    for root, ds, fs in os.walk(m(prefix or '/')):
        rel = '/' + os.path.relpath(root, model).lstrip('.')
        rel = rel.replace('//', '/')
        if rel == '/.':
            rel = '/'
        for d in ds:
            p = os.path.join(rel, d)
            if os.path.islink(m(p)):
                files.append(p)
            else:
                dirs.append(p)
        for f in fs:
            files.append(os.path.join(rel, f))
    return files, dirs

def payload_file(size):
    pcount[0] += 1
    p = os.path.join(payload, f'p{seed}_{pcount[0]}')
    with open(p, 'wb') as f:
        if size > 0 and rnd.random() < 0.3:
            f.seek(size - 1)  # sparse-ish payload
            f.write(b'z')
        else:
            f.write(rnd.randbytes(size))
    os.chmod(p, 0o644)
    return p

def sizes():
    if SMALL:
        return rnd.choice([0, 1, 100, 4095, 4096, 4097, 50000, 300000])
    return rnd.choice([0, 1, 100, 4095, 4096, 4097, 50000, 1 << 20, 3 << 20, 9_000_000])

if SNAP:
    snapshot()  # snap_0: state before any operation
if prefix:
    os.makedirs(m(prefix), exist_ok=True)
    os.chmod(m(prefix), 0o755)
    emit('mkdir', prefix)

for i in range(n):
    files, dirs = all_paths()
    op = rnd.choices(['mkdir', 'put', 'write', 'truncate', 'rm', 'rmdir', 'mv', 'ln', 'symlink', 'chmod', 'xattr', 'bulk', 'bulkdel'],
                     weights=[8, 14, 10, 6, 7, 3, 10, 4, 4, 3, 5, 1, 1])[0]
    if op == 'mkdir':
        p = os.path.join(rnd.choice(dirs), name())
        if os.path.lexists(m(p)) or len(p.encode()) > 3000:
            continue
        os.mkdir(m(p)); os.chmod(m(p), 0o755)
        emit('mkdir', p)
    elif op == 'put':
        p = os.path.join(rnd.choice(dirs), name())
        if os.path.lexists(m(p)) or len(p.encode()) > 3000:
            continue
        src = payload_file(sizes())
        with open(src, 'rb') as a, open(m(p), 'wb') as b:
            b.write(a.read())
        os.chmod(m(p), 0o644)
        emit('put', src, p)
    elif op == 'write':
        reg = [f for f in files if os.path.isfile(m(f)) and not os.path.islink(m(f))]
        if not reg:
            continue
        p = rnd.choice(reg)
        size = os.path.getsize(m(p))
        off = rnd.choice([0, size, max(0, size - 3), rnd.randint(0, size + 10000), size + rnd.randint(0, 1 << 22)])
        text = ''.join(rnd.choice('abcdefghij XYZ0123') for _ in range(rnd.randint(1, 5000))).strip() or 'q'
        with open(m(p), 'r+b') as f:
            f.seek(off); f.write(text.encode())
        emit('write', p, off, text)
    elif op == 'truncate':
        reg = [f for f in files if os.path.isfile(m(f)) and not os.path.islink(m(f))]
        if not reg:
            continue
        p = rnd.choice(reg)
        size = os.path.getsize(m(p))
        ns = rnd.choice([0, size // 2, max(0, size - 1), size + 1, size + 123456, rnd.randint(0, 1 << 23)])
        os.truncate(m(p), ns)
        emit('truncate', p, ns)
    elif op == 'rm':
        if not files:
            continue
        p = rnd.choice(files)
        os.unlink(m(p))
        emit('rm', p)
    elif op == 'rmdir':
        empty = [d for d in dirs if d not in ('/', prefix) and not os.listdir(m(d))]
        if not empty:
            continue
        p = rnd.choice(empty)
        os.rmdir(m(p))
        emit('rmdir', p)
    elif op == 'mv':
        if rnd.random() < 0.7 and files:
            src = rnd.choice(files)
            if rnd.random() < 0.3 and len(files) > 1:
                dst = rnd.choice([f for f in files if f != src])  # overwrite
            else:
                dst = os.path.join(rnd.choice(dirs), name())
            if os.path.isdir(m(dst)) and not os.path.islink(m(dst)):
                continue
            if len(dst.encode()) > 3000:
                continue
            os.rename(m(src), m(dst))
            emit('mv', src, dst)
        else:
            cand = [d for d in dirs if d not in ('/', prefix)]
            if not cand:
                continue
            src = rnd.choice(cand)
            parent = rnd.choice(dirs)
            if parent == src or parent.startswith(src + '/'):
                continue
            dst = os.path.join(parent, name())
            if os.path.lexists(m(dst)) or len(dst.encode()) > 3000:
                continue
            os.rename(m(src), m(dst))
            emit('mv', src, dst)
    elif op == 'ln':
        reg = [f for f in files if os.path.isfile(m(f)) and not os.path.islink(m(f))]
        if not reg:
            continue
        src = rnd.choice(reg)
        dst = os.path.join(rnd.choice(dirs), name())
        if os.path.lexists(m(dst)) or len(dst.encode()) > 3000:
            continue
        os.link(m(src), m(dst))
        emit('ln', src, dst)
    elif op == 'symlink':
        dst = os.path.join(rnd.choice(dirs), name())
        if os.path.lexists(m(dst)) or len(dst.encode()) > 3000:
            continue
        target = rnd.choice(['x', '../a/b', 'y' * 59, 'z' * 60, '/abs/' + 'w' * rnd.randint(1, 900)])
        os.symlink(target, m(dst))
        emit('symlink', target, dst)
    elif op == 'chmod':
        reg = [f for f in files if os.path.isfile(m(f)) and not os.path.islink(m(f))]
        if not reg:
            continue
        p = rnd.choice(reg)
        mode = rnd.choice([0o600, 0o640, 0o644, 0o755, 0o700])
        os.chmod(m(p), mode)
        emit('chmod', f'{mode:o}', p)
    elif op == 'xattr':
        reg = [f for f in files if os.path.isfile(m(f)) and not os.path.islink(m(f))]
        if not reg:
            continue
        p = rnd.choice(reg)
        key = rnd.choice(['com.apple.FinderInfo', 'com.apple.quarantine', 'k', 'long.' + 'n' * 100])
        xmax = int(os.environ.get('XATTR_MAX', '2000'))
        val = ''.join(rnd.choice('abc123') for _ in range(min(xmax, rnd.choice([1, 30, 400, 2000]))))
        if rnd.random() < 0.25:
            try:
                os.removexattr(m(p), 'user.' + key)
                emit('rmxattr', p, key)
            except OSError:
                pass
        else:
            try:
                os.setxattr(m(p), 'user.' + key, val.encode())
                emit('setxattr', p, key, val)
            except OSError:
                pass  # host fs out of xattr space: skip
    elif op == 'bulk':
        d = os.path.join(rnd.choice(dirs), 'bulk' + str(i))
        if os.path.lexists(m(d)) or len(d.encode()) > 3000:
            continue
        os.mkdir(m(d)); os.chmod(m(d), 0o755)
        emit('mkdir', d)
        src = payload_file(rnd.choice([0, 10]))
        for k in range(rnd.choice([40, 120] if SMALL else [200, 800, 2500])):
            p = os.path.join(d, f'file_{k:05d}_' + 'q' * rnd.randint(0, 100))
            with open(src, 'rb') as a, open(m(p), 'wb') as b:
                b.write(a.read())
            os.chmod(m(p), 0o644)
            emit('put', src, p)
    elif op == 'bulkdel':
        big = [d for d in dirs if os.path.isdir(m(d)) and len(os.listdir(m(d))) > 100]
        if not big:
            continue
        d = rnd.choice(big)
        for e in os.listdir(m(d)):
            full = os.path.join(d, e)
            if rnd.random() < 0.6 and (os.path.islink(m(full)) or not os.path.isdir(m(full))):
                os.unlink(m(full))
                emit('rm', full)

print('\n'.join(out))
