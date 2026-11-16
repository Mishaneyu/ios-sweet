#!/usr/bin/env python3
"""Write the Renegade iOS root (Android sparse image) onto the G8S userdata partition, from Arch on the phone.

  sudo python3 -I tools/betalm/flash-userdata.py ROOT.simg [--dev /dev/disk/by-partlabel/userdata] --yes

Every block of the volume is written: RAW chunks as they are, FILL chunks with their value, DONT_CARE as zeros, so
nothing of the old Android data survives inside the HFS+ volume (a raw image with holes would leave it there).
Refuses unless the partition label is userdata, the device is not mounted and it is at least as big as the image.
Afterwards fsck.hfsplus -n checks the result read-only. Destroys whatever is on userdata.
"""
import argparse
import hashlib
import os
import stat
import struct
import subprocess
import sys
import time

RAW, FILL, DONT_CARE, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
PIECE = 8 << 20


def die(msg):
    sys.exit('flash-userdata: ' + msg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('simg')
    ap.add_argument('--dev', default='/dev/disk/by-partlabel/userdata')
    ap.add_argument('--yes', action='store_true', help='really write (otherwise: checks only)')
    a = ap.parse_args()
    if os.geteuid() != 0:
        die('run as root (sudo)')
    dev = os.path.realpath(a.dev)
    if not stat.S_ISBLK(os.stat(dev).st_mode):
        die(f'{dev} is not a block device')
    label = subprocess.run(['lsblk', '-no', 'PARTLABEL', dev], capture_output=True, text=True).stdout.strip()
    if label != 'userdata':
        die(f'{dev} has partition label {label!r}, not userdata: refusing')
    mounts = open('/proc/mounts').read().split()
    if dev in mounts:
        die(f'{dev} is mounted: unmount it first')
    devsize = int(open(f'/sys/class/block/{os.path.basename(dev)}/size').read()) * 512

    f = open(a.simg, 'rb')
    m, mj, mn, hs, cs, bs, tb, nc, crc = struct.unpack('<IHHHHIIII', f.read(28))
    if m != 0xED26FF3A:
        die(f'{a.simg} is not an Android sparse image')
    total = tb * bs
    print(f'image: {a.simg}: {total / 2**30:.2f} GiB volume, {nc} chunks, block {bs}')
    print(f'target: {a.dev} -> {dev} (label {label}), {devsize / 2**30:.2f} GiB')
    if total > devsize:
        die('the image is bigger than the partition')
    if not a.yes:
        print('checks OK; nothing written (add --yes to write)')
        return

    f.seek(hs)
    fd = os.open(dev, os.O_WRONLY)
    zero = bytes(PIECE)
    pos, written, t0, last = 0, 0, time.time(), 0
    try:
        for _ in range(nc):
            t, _r, sz, tot = struct.unpack('<HHII', f.read(cs if cs >= 12 else 12)[:12])
            n = sz * bs
            if t == RAW:
                left = n
                while left:
                    b = f.read(min(left, PIECE))
                    if not b:
                        die('simg ends inside a RAW chunk')
                    os.pwrite(fd, b, pos)
                    pos += len(b); left -= len(b); written += len(b)
            elif t in (FILL, DONT_CARE):
                pat = f.read(4) if t == FILL else b'\0\0\0\0'
                buf = zero if pat == b'\0\0\0\0' else pat * (PIECE // 4)
                left = n
                while left:
                    k = min(left, PIECE)
                    os.pwrite(fd, buf[:k], pos)
                    pos += k; left -= k; written += k
            elif t == CRC:
                f.read(4)
            else:
                die(f'unknown chunk type {t:#x}')
            now = time.time()
            if now - last > 5:
                last = now
                rate = written / max(now - t0, 1) / 2**20
                print(f'  {pos / total * 100:5.1f}%  {pos / 2**30:6.2f} / {total / 2**30:.2f} GiB  {rate:.0f} MB/s', flush=True)
        os.fsync(fd)
    finally:
        os.close(fd)
    if pos != total:
        die(f'wrote {pos} bytes, the image says {total}')
    print(f'written {total / 2**30:.2f} GiB in {time.time() - t0:.0f}s, synced')
    subprocess.run(['sync'])
    r = subprocess.run(['fsck.hfsplus', '-f', '-n', dev], capture_output=True, text=True)
    print((r.stdout + r.stderr).strip()[-1500:])
    print('fsck.hfsplus -n:', 'OK' if r.returncode == 0 else f'exit {r.returncode}')
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
