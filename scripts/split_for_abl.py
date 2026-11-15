#!/usr/bin/env python3
"""Split a sparse image into pieces the 32-bit fastboot client can actually send.

Why this exists
---------------
Google ships a 32-bit fastboot.exe for Windows (verified: PE machine 0x014c on
platform-tools 37.0.1).  It mmaps the whole file before streaming, so a 12 GiB
root image dies immediately with `std::bad_alloc`.  The fix is to hand it
several ~1.4 GiB pieces and let the bootloader place each one at the right
offset.

How the offset is encoded
------------------------
`fastboot flash <partition>` always writes from block 0, so each piece carries a
leading CHUNK_TYPE_DONT_CARE chunk spanning every block before its own range.
The bootloader seeks over it instead of writing zeros -- confirmed on raphael:
a 413 MB piece whose don't-care prefix covered 11.27 GiB finished with
`Writing 'userdata'  OKAY [ 0.002s]`, which is impossible if it had written
those zeros.

Ordering
--------
Pieces are flashed in DESCENDING order (last block range first).  With the
seek behaviour above that is merely tidy, but it is also the only ordering that
stays correct if a bootloader were to materialise don't-care as zeros: such a
write only ever touches addresses below the current piece, i.e. pieces that
have not been written yet.

Usage
-----
    split_for_abl.py ios16-root.simg -o pieces/ [--limit-gib 1.35]

Then, in the output directory:

    for p in $(ls p*.simg | sort -r); do fastboot flash userdata "$p"; done

`ORDER.txt` lists the pieces in ascending block order.
"""
import argparse
import os
import shutil
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW, CHUNK_FILL, CHUNK_DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
FILE_HDR_SZ, CHUNK_HDR_SZ = 28, 12


def index(path):
    """Return (block_size, total_blocks, [(type, blocks, bytes, offset)])."""
    f = open(path, "rb")
    magic, _maj, _mn, fhs, chs, blk, tot, nch, _ck = struct.unpack(
        "<IHHHHIIII", f.read(28))
    if magic != SPARSE_MAGIC:
        raise SystemExit("%s: not a sparse image" % path)
    if (fhs, chs) != (FILE_HDR_SZ, CHUNK_HDR_SZ):
        raise SystemExit("%s: unexpected header sizes %d/%d" % (path, fhs, chs))
    pos, chunks, blocks = fhs, [], 0
    for i in range(nch):
        f.seek(pos)
        t, _res, a, b = struct.unpack("<HHII", f.read(12))
        # ABL order: a = blocks, b = bytes.  Try that first, then the AOSP order,
        # so this tool also eats images built by stock mksimg/linux tools.
        if t in (CHUNK_FILL, CHUNK_DONT_CARE):
            csz = CHUNK_HDR_SZ
            tsz = a if a else b
        elif b - CHUNK_HDR_SZ == a * blk:
            tsz, csz = a, b
        elif a - CHUNK_HDR_SZ == b * blk:
            tsz, csz = b, a
        else:
            raise SystemExit("%s: chunk %d is neither field order" % (path, i))
        if csz < CHUNK_HDR_SZ or (csz - CHUNK_HDR_SZ) % blk:
            raise SystemExit("%s: chunk %d bad size %d" % (path, i, csz))
        chunks.append((t, tsz, csz, pos))
        pos += csz
        blocks += tsz
    if blocks != tot:
        raise SystemExit("%s: chunk blocks %d != header %d" % (path, blocks, tot))
    if pos != os.path.getsize(path):
        raise SystemExit("%s: table ends at %d, file is %d" % (path, pos, os.path.getsize(path)))
    return blk, tot, chunks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sparse")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("--limit-gib", type=float, default=1.35,
                    help="raw payload per piece; keep the file well under 2 GiB")
    args = ap.parse_args()

    blk, tot, chunks = index(args.sparse)
    limit = int(args.limit_gib * 1024 ** 3)
    print("%s: %d chunks, %d blocks (%.2f GiB)" % (args.sparse, len(chunks), tot, tot * blk / 2 ** 30))

    groups, cur, raw, start, span = [], [], 0, 0, 0
    for c in chunks:
        cur.append(c)
        span += c[1]
        if c[0] == CHUNK_RAW:
            raw += c[2] - CHUNK_HDR_SZ
        if raw >= limit:
            groups.append((start, start + span, cur))
            start += span
            span, cur, raw = 0, [], 0
    if cur:
        groups.append((start, start + span, cur))

    if os.path.isdir(args.outdir):
        shutil.rmtree(args.outdir)
    os.makedirs(args.outdir)

    src = open(args.sparse, "rb")
    rows = []
    for gi, (sblk, eblk, cs) in enumerate(groups):
        path = os.path.join(args.outdir, "p%02d.simg" % gi)
        with open(path, "wb") as o:
            o.write(b"\0" * FILE_HDR_SZ)
            n = 0
            if sblk:
                o.write(struct.pack("<HHII", CHUNK_DONT_CARE, 0, sblk, CHUNK_HDR_SZ))
                n += 1
            for (t, tsz, csz, off) in cs:
                o.write(struct.pack("<HHII", t, 0, tsz, csz))
                n += 1
                left = csz - CHUNK_HDR_SZ
                src.seek(off + CHUNK_HDR_SZ)
                while left:
                    buf = src.read(min(left, 8 << 20))
                    if not buf:
                        raise SystemExit("short read in %s" % args.sparse)
                    o.write(buf)
                    left -= len(buf)
            o.seek(0)
            o.write(struct.pack("<IHHHHIIII", SPARSE_MAGIC, 1, 0, FILE_HDR_SZ,
                                CHUNK_HDR_SZ, blk, eblk, n, 0))
        size = os.path.getsize(path)
        rows.append((os.path.basename(path), sblk, eblk, size))
        print("  %-10s blocks %8d..%-8d  %9d B  (covers up to %.2f GiB)"
              % (rows[-1][0], sblk, eblk, size, eblk * blk / 2 ** 30))
    src.close()

    with open(os.path.join(args.outdir, "ORDER.txt"), "w") as fh:
        for name, sblk, eblk, size in rows:
            fh.write("%s %d %d %d\n" % (name, sblk, eblk, size))
    print("%d pieces, %.2f GiB total" % (len(rows), sum(r[3] for r in rows) / 2 ** 30))
    print("flash with:  for p in $(ls p*.simg | sort -r); do fastboot flash userdata $p; done")


if __name__ == "__main__":
    sys.exit(main())
