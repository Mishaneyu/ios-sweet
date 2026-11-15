#!/usr/bin/env python3
"""Assemble and verify the HFS+ iOS root that the SWEET kernel mounts.

The kernel's root device is the phone's UFS GPT partition named "userdata"
(boot-arg ufsroot=, default "userdata"), published by bsd/dev/sweet_ufs.c and
mounted with the built-in Apple OSS HFS+ driver (hfs-627).  It expects an HFS+
volume at the START of that partition -- there is no SHNGROOT manifest and no
APFS window involved (the kernel binary contains the string "SHNGROOT" zero
times).

The release publishes the sparse image in parts; this script concatenates them,
checks the published sha256, and prints the HFS+ volume header for a sanity
check before anything is flashed.

    assemble_hfsroot.py PARTS_DIR -o OUT.simg [--expect-sha FILE]
"""
import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

SPARSE_MAGIC = 0xED26FF3A


def read_sparse_header(path):
    with open(path, "rb") as f:
        hdr = f.read(28)
    magic, maj, minor, fhs, chs, blk, total, chunks, ck = struct.unpack(
        "<IHHHHIIII", hdr)
    if magic != SPARSE_MAGIC:
        raise SystemExit("%s: not a sparse image (magic %08x)" % (path, magic))
    return dict(major=maj, minor=minor, file_hdr_sz=fhs, chunk_hdr_sz=chs,
                block_size=blk, total_blocks=total, chunk_count=chunks)


def hfs_header(path, block_size):
    """Read the HFS+ volume header, which lives at byte 1024 of the volume."""
    with open(path, "rb") as f:
        f.seek(1024)
        h = f.read(512)
    if len(h) < 512:
        return None
    sig = h[0:2]
    if sig not in (b"H+", b"HX"):
        return None
    version = struct.unpack_from(">H", h, 2)[0]
    attributes = struct.unpack_from(">I", h, 4)[0]
    # fileCount/blockSize live in the volume header (offsets 0x28 / 0x2C on the
    # 512-byte HFS+ header at 1024+2).
    block_size_field = struct.unpack_from(">I", h, 0x28)[0]
    file_count = struct.unpack_from(">I", h, 0x2C)[0]
    # signature is 0x48455320 ("HES ") for HFSX, 0x484B5348 ("HKSH") for HFS+
    return dict(signature=sig.decode("ascii"), version=version,
                attributes=attributes, block_size=block_size_field,
                file_count=file_count)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("parts", help="directory holding root-part.NN files")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--expect-sha", default=None,
                    help="file with the expected sha256 (one per line)")
    ap.add_argument("--no-hash", action="store_true",
                    help="skip the sha256 pass (it reads the whole image)")
    args = ap.parse_args()

    parts = sorted(Path(args.parts).glob("root-part.*"))
    if not parts:
        raise SystemExit("no root-part.* files in %s" % args.parts)

    total = 0
    for p in parts:
        total += p.stat().st_size
    print("parts: %d files, %.2f GiB" % (len(parts), total / 2 ** 30))

    # Per-part sha256 first: a half-downloaded or corrupt part must never
    # reach the flash step.
    parts_sha = Path(args.parts) / "root-parts.sha256"
    if parts_sha.exists():
        want = {}
        for line in parts_sha.read_text().splitlines():
            bits = line.split()
            if len(bits) == 2:
                want[bits[1]] = bits[0]
        bad = 0
        for p in parts:
            exp = want.get(p.name)
            if exp is None:
                print("  %-16s no checksum published" % p.name)
                continue
            h = hashlib.sha256()
            with open(p, "rb") as f:
                for chunk in iter(lambda: f.read(16 << 20), b""):
                    h.update(chunk)
            got = h.hexdigest()
            ok = got == exp
            if not ok:
                bad += 1
            print("  %-16s %s" % (p.name, "OK" if ok else "SHA256 MISMATCH"))
        if bad:
            raise SystemExit("%d part(s) failed the checksum -- do not flash" % bad)
        print("all parts match the published checksums")

    out = Path(args.output)
    with open(out, "wb") as o:
        for p in parts:
            print("  + %s (%d B)" % (p.name, p.stat().st_size))
            with open(p, "rb") as f:
                while True:
                    buf = f.read(16 << 20)
                    if not buf:
                        break
                    o.write(buf)
    print("assembled: %s (%d B)" % (out, out.stat().st_size))

    hdr = read_sparse_header(out)
    print("sparse: block_size=%d total_blocks=%d chunks=%d -> %.2f GiB"
          % (hdr["block_size"], hdr["total_blocks"], hdr["chunk_count"],
             hdr["total_blocks"] * hdr["block_size"] / 2 ** 30))

    if args.expect_sha and Path(args.expect_sha).exists():
        want = Path(args.expect_sha).read_text().split()[0].strip()
        h = hashlib.sha256()
        with open(out, "rb") as f:
            for chunk in iter(lambda: f.read(16 << 20), b""):
                h.update(chunk)
        got = h.hexdigest()
        print("sha256: %s" % got)
        print("expect: %s" % want)
        if got != want:
            raise SystemExit("SHA256 MISMATCH -- do not flash")
        print("sha256 OK")

    vh = hfs_header(out, hdr["block_size"])
    if vh is None:
        print("note: no HFS+ volume header at byte 1024 of the sparse file")
        print("      (it may sit inside a RAW chunk; check the first data chunk)")
    else:
        print("HFS+ volume: signature=%s version=%d attributes=0x%x block_size=%d files=%d"
              % (vh["signature"], vh["version"], vh["attributes"],
                 vh["block_size"], vh["file_count"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
