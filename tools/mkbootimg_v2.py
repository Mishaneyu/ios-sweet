#!/usr/bin/env python3
"""Minimal Android boot image (header v2) packer, AOSP mkbootimg compatible.

Accepts the subset of AOSP mkbootimg's options that shim/build.sh uses, so it
can stand in for mkbootimg when that is not installed:

  mkbootimg_v2.py --header_version 2 --kernel K --ramdisk R --dtb D \
      --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
      --tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
      --os_version 17.0.0 --os_patch_level 2035-06 --cmdline "..." -o OUT

Layout (system/tools/mkbootimg/include/bootimg/bootimg.h, boot_img_hdr_v2):
header page, kernel, ramdisk, second (empty), recovery dtbo (empty), dtb,
each padded to the page size.
"""
import argparse, hashlib, struct, sys

HDR_V2_SIZE = 1660


def num(s):
    return int(s, 0)


def pad(n, page):
    return (page - n % page) % page


def os_version(ver, patch):
    a, b, c = (int(x) for x in (ver.split('.') + ['0', '0'])[:3])
    y, m = (int(x) for x in patch.split('-')[:2])
    return ((a << 14) | (b << 7) | c) << 11 | ((y - 2000) << 4) | m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--header_version', type=num, default=2)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk')
    ap.add_argument('--dtb', required=True)
    ap.add_argument('--base', type=num, default=0x10000000)
    ap.add_argument('--kernel_offset', type=num, default=0x8000)
    ap.add_argument('--ramdisk_offset', type=num, default=0x1000000)
    ap.add_argument('--second_offset', type=num, default=0xf00000)
    ap.add_argument('--tags_offset', type=num, default=0x100)
    ap.add_argument('--dtb_offset', type=num, default=0x1f00000)
    ap.add_argument('--pagesize', type=num, default=4096)
    ap.add_argument('--os_version', default='0.0.0')
    ap.add_argument('--os_patch_level', default='2000-01')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--board', default='')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()
    if a.header_version != 2:
        sys.exit('mkbootimg_v2.py only writes header version 2')

    kernel = open(a.kernel, 'rb').read()
    ramdisk = open(a.ramdisk, 'rb').read() if a.ramdisk else b''
    dtb = open(a.dtb, 'rb').read()
    cmd = a.cmdline.encode()
    if len(cmd) > 512 + 1024 - 1:
        sys.exit('cmdline too long')

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', b'', dtb):  # kernel ramdisk second dtbo dtb
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))

    page = a.pagesize
    hdr = b'ANDROID!'
    hdr += struct.pack('<10I',
                       len(kernel), a.base + a.kernel_offset,
                       len(ramdisk), a.base + a.ramdisk_offset,
                       0, a.base + a.second_offset,
                       a.base + a.tags_offset, page, a.header_version,
                       os_version(a.os_version, a.os_patch_level))
    hdr += struct.pack('16s', a.board.encode())
    hdr += struct.pack('512s', cmd[:511])
    hdr += struct.pack('32s', sha.digest())
    hdr += struct.pack('1024s', cmd[511:])
    hdr += struct.pack('<IQI', 0, 0, HDR_V2_SIZE)          # recovery dtbo, header size
    hdr += struct.pack('<IQ', len(dtb), a.base + a.dtb_offset)
    assert len(hdr) == HDR_V2_SIZE, len(hdr)

    with open(a.output, 'wb') as f:
        f.write(hdr + b'\0' * pad(len(hdr), page))
        for blob in (kernel, ramdisk, dtb):
            f.write(blob + b'\0' * pad(len(blob), page))


if __name__ == '__main__':
    main()
