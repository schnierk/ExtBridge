#!/usr/bin/env python3
"""Create a deterministic test directory tree with the cases that matter
for ext4: big directories (htree), long and unicode names, symlinks of all
lengths, hard links, empty/small/large/sparse files and xattrs."""
import os, random, sys

def main(root, scale=1):
    rnd = random.Random(4711)
    os.makedirs(root, exist_ok=True)
    def wfile(p, size, sparse=False):
        with open(p, 'wb') as f:
            if sparse:
                f.seek(size - 4096)
                f.write(rnd.randbytes(4096))
            else:
                f.write(rnd.randbytes(size))
    # small and odd sized files
    os.makedirs(f'{root}/files', exist_ok=True)
    for s in [0, 1, 59, 60, 61, 1023, 1024, 1025, 4095, 4096, 4097, 65536, 100000, 1 << 20]:
        wfile(f'{root}/files/size_{s}', s)
    wfile(f'{root}/files/big_24M', 24 << 20)
    wfile(f'{root}/files/sparse_64M', 64 << 20, sparse=True)
    # big directory -> htree with several levels of leaves
    os.makedirs(f'{root}/bigdir', exist_ok=True)
    for i in range(3000 * scale):
        n = f'entry_{i:05d}_' + 'x' * rnd.randint(0, 60)
        with open(f'{root}/bigdir/{n}', 'wb') as f:
            f.write(str(i).encode())
    # names
    os.makedirs(f'{root}/names', exist_ok=True)
    for n in ['ä ö ü ß', 'Ελληνικά', '日本語のファイル', 'emoji_😀', 'a' * 255, 'with space', '.hidden', 'semi;colon', 'quote"s', "apo'strophe"]:
        with open(f'{root}/names/{n}', 'w') as f:
            f.write(n)
    # symlinks
    os.makedirs(f'{root}/links', exist_ok=True)
    os.symlink('../files/size_1', f'{root}/links/short')
    os.symlink('x' * 59, f'{root}/links/len59')
    os.symlink('y' * 60, f'{root}/links/len60')
    os.symlink('/very/long/' + 'z' * 300, f'{root}/links/long300')
    os.symlink('dangling_target', f'{root}/links/dangling')
    os.link(f'{root}/files/size_4096', f'{root}/links/hardlink_4096')
    # deep nesting
    p = f'{root}/deep'
    for i in range(40):
        p += f'/level{i}'
    os.makedirs(p)
    with open(p + '/bottom.txt', 'w') as f:
        f.write('bottom\n')
    # permissions
    os.makedirs(f'{root}/perm', exist_ok=True)
    for m in [0o600, 0o644, 0o755, 0o4755, 0o2755, 0o1777, 0o400]:
        fn = f'{root}/perm/mode_{m:04o}'
        with open(fn, 'w') as f:
            f.write('m')
        os.chmod(fn, m)
    # xattrs
    os.makedirs(f'{root}/xattr', exist_ok=True)
    for i, size in enumerate([1, 10, 100, 500, 900]):
        fn = f'{root}/xattr/file{i}'
        with open(fn, 'w') as f:
            f.write('x')
        os.setxattr(fn, 'user.small', b'v')
        os.setxattr(fn, 'user.sized', rnd.randbytes(size))
    # empty dir
    os.makedirs(f'{root}/empty', exist_ok=True)

if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1)
