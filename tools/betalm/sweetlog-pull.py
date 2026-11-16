#!/usr/bin/env python3
"""Read the persistent console log (boot-arg sweetlog=1) from the end of the userdata partition, from Arch.

  sudo python3 -I tools/betalm/sweetlog-pull.py [--dev /dev/disk/by-partlabel/userdata] [-o OUT.txt]

The kernel (patch 0047) keeps every console character in a 4 MiB ring and copies it about once a second to the
last 4 MiB + 4 KiB of the root partition: one 4 KiB header block ("SWEETLOG", u32 version, u32 ring size,
u64 total bytes, u64 flush sequence; little endian), then the ring. Read only.
"""
import argparse
import os
import struct
import sys

BLK = 4096
RING = 4 << 20


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--dev', default='/dev/disk/by-partlabel/userdata')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    dev = os.path.realpath(a.dev)
    fd = os.open(dev, os.O_RDONLY)
    try:
        size = os.lseek(fd, 0, os.SEEK_END)
        base = (size // BLK - (1 + RING // BLK)) * BLK
        hdr = os.pread(fd, BLK, base)
        magic, ver, rsize, total, seq = struct.unpack_from('<8sIIQQ', hdr)
        if magic != b'SWEETLOG':
            sys.exit(f'no SWEETLOG header at the end of {dev} (log never flushed: was sweetlog=1 and ufsrw=1 set?)')
        ring = os.pread(fd, rsize, base + BLK)
    finally:
        os.close(fd)
    if total <= rsize:
        data = ring[:total]
    else:
        s = total % rsize
        data = ring[s:] + ring[:s]
    text = data.decode('utf-8', 'replace')
    print(f'# sweetlog v{ver}: {total} bytes logged, flush #{seq}, showing the last {len(data)}', file=sys.stderr)
    if a.out:
        with open(a.out, 'w') as f:
            f.write(text)
        print(f'# written to {a.out}', file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
