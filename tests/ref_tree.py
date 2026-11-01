#!/usr/bin/env python3
"""Print a directory tree in exactly the format of `l2m tree`."""
import os, stat, sys

if hasattr(os, 'listxattr'):
    def user_xattrs(path):
        out = {}
        for x in os.listxattr(path):
            if x.startswith('user.'):
                out[x[5:]] = os.getxattr(path, x)
        return out
else:
    # macOS: ExtBridge shows Linux "user.*" attributes without the prefix
    import ctypes
    _libc = ctypes.CDLL(None, use_errno=True)
    _NOFOLLOW = 1

    def user_xattrs(path):
        p = path.encode()
        n = _libc.listxattr(p, None, 0, _NOFOLLOW)
        if n <= 0:
            return {}
        buf = ctypes.create_string_buffer(n)
        n = _libc.listxattr(p, buf, n, _NOFOLLOW)
        out = {}
        for name in buf.raw[:n].split(b'\0'):
            if not name:
                continue
            size = _libc.getxattr(p, name, None, 0, 0, _NOFOLLOW)
            vbuf = ctypes.create_string_buffer(max(size, 1))
            size = _libc.getxattr(p, name, vbuf, size, 0, _NOFOLLOW)
            out[name.decode()] = vbuf.raw[:size]
        return out

POLY = 0x82F63B78
TABLE = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ (POLY if c & 1 else 0)
    TABLE.append(c)

def crc32c(data, crc=0):
    t = TABLE
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc

def walk(root, prefix='', out=None):
    if out is None:
        out = print
    names = sorted(os.listdir(os.path.join(root, prefix.lstrip('/'))) if prefix else os.listdir(root),
                   key=lambda s: s.encode())
    for n in names:
        if n == 'lost+found' and prefix == '':
            continue
        rel = prefix + '/' + n
        full = os.path.join(root, rel.lstrip('/'))
        st = os.lstat(full)
        m = stat.S_IMODE(st.st_mode)
        if stat.S_ISREG(st.st_mode):
            with open(full, 'rb') as f:
                c = crc32c(f.read())
            out(f'{rel}\tfile\t{m:04o}\t{st.st_size}\t{c:08x}')
            xs = user_xattrs(full)
            for x in sorted(xs, key=lambda s: s.encode()):
                v = xs[x]
                out(f'{rel}\txattr\t{x}\t{len(v)}\t{crc32c(v):08x}')
        elif stat.S_ISLNK(st.st_mode):
            out(f'{rel}\tlink\t{os.readlink(full)}')
        elif stat.S_ISDIR(st.st_mode):
            out(f'{rel}\tdir\t{m:04o}')
            walk(root, rel, out)
        else:
            out(f'{rel}\t?\t{m:04o}')

if __name__ == '__main__':
    walk(sys.argv[1])
