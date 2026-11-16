#!/usr/bin/env python3
"""Put original IPSW files back into the mounted HFSX root (a tweak that died half-way through a write).

  sudo -E RENEGADE_TARGET=ios18 python3 -I tools/renegade/restore_from_ipsw.py [--dmg os|sysos|appos] ROOTPATH ...

ROOTPATH is a path inside the root, e.g. System/Library/xpc/launchd.plist. The file is read from the IPSW dmg
(apfs-fuse, read-only) and written into research/<target>/full/root as a NEW file that then replaces the old one
(owner and mode from the dmg), so the old, possibly damaged inode and its extents are let go first. The root image
must be mounted there (a stopped mkroot-full.py leaves it mounted).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import renegade_platform as rplat  # noqa: E402
from targets import T as TARGET  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
W = os.path.join(ROOT, TARGET['work'], 'full')


def find_dmgs(d):
    import plistlib
    for c in (os.path.join(d, 'BuildManifest.plist'), os.path.join(os.path.dirname(d), 'BuildManifest.plist')):
        if os.path.exists(c):
            m = plistlib.load(open(c, 'rb'))['BuildIdentities'][0]['Manifest']
            break
    else:
        sys.exit(f'no BuildManifest.plist in or next to {d}')
    out = {}
    for key, name in (('OS', 'os'), ('Cryptex1,SystemOS', 'sysos'), ('Cryptex1,AppOS', 'appos')):
        p = m[key]['Info']['Path']
        out[name] = os.path.join(d, p[:-4] if p.endswith('.aea') else p)
    return out


def replace_file(dst, data, uid, gid, mode):
    """Write data as a new file next to dst in one write, fsync, then rename it over dst."""
    tmp = dst + '.sweet-new'
    if os.path.exists(tmp):
        os.unlink(tmp)
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        view = memoryview(data)
        while view:
            n = os.write(fd, view)
            view = view[n:]
        os.fsync(fd)
    finally:
        os.close(fd)
    os.chown(tmp, uid, gid)
    os.chmod(tmp, mode)
    os.replace(tmp, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--dmg', default='os', choices=('os', 'sysos', 'appos'))
    ap.add_argument('--ipsw-dir', default=os.path.join(ROOT, TARGET['ipsw_dir']))
    a = ap.parse_args()
    rplat.need_privileges()
    root = os.path.join(W, 'root')
    if not os.path.ismount(root):
        sys.exit(f'{root} is not mounted (mount the root image first)')
    dmg = find_dmgs(a.ipsw_dir)[a.dmg]
    if not os.path.exists(dmg):
        sys.exit(f'{dmg} is not here any more (deleted after the copy?): download it again with fetch-root.sh')
    mnt = os.path.join(W, 'src-restore')
    dev = rplat.attach_ipsw(dmg, mnt, owners=True)
    try:
        for rel in a.paths:
            rel = rel.lstrip('/')
            src, dst = os.path.join(mnt, rel), os.path.join(root, rel)
            st = os.lstat(src)
            data = open(src, 'rb').read()
            old = os.path.getsize(dst) if os.path.exists(dst) else None
            replace_file(dst, data, st.st_uid, st.st_gid, st.st_mode & 0o7777)
            ok = open(dst, 'rb').read() == data
            print(f'{rel}: {old} -> {len(data)} bytes, owner {st.st_uid}:{st.st_gid} mode {st.st_mode & 0o7777:o}, '
                  f'read-back {"OK" if ok else "MISMATCH"}')
            if not ok:
                sys.exit(1)
    finally:
        rplat.detach(dev)


if __name__ == '__main__':
    main()
