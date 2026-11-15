#!/usr/bin/env python3
"""Simulate what the SM8150 ABL does with the flash pieces.

The bootloader is not available on the workstation, but its behaviour is
known from the flashes that already worked on this phone:

  * a sparse chunk header is read as "u16 type, u16 res, u32 blocks,
    u32 bytes" (the opposite of the AOSP/libsparse order),
  * DONT_CARE is a seek, not a write of zeros, and
  * each piece is placed at the absolute block its leading DONT_CARE
    chunk declares.

This replays the pieces in DESCENDING order and checks that the device
would end up holding exactly the source image, block for block.  It catches a
wrong chunk field order, a wrong piece order and an off-by-one in the offset
without touching the phone.

Memory
------
A 16 GiB root has 4,194,304 blocks.  Keeping a Python dict of block -> bytes
for those needs ~12 GB of RAM, so the simulation records only *where* each
block lives (which piece, which byte offset in that piece) in two numpy
arrays -- 4 bytes + 8 bytes per block, ~50 MB -- and then compares the bytes
by reading both sides in one pass.

    simulate_flash.py SOURCE.simg PIECE_DIR
"""
import argparse
import os
import struct
import sys

import numpy as np

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW, CHUNK_FILL, CHUNK_DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
FILE_HDR_SZ, CHUNK_HDR_SZ = 28, 12


def read_header(f):
    magic, _maj, _mn, fhs, chs, blk, tot, nch, _ck = struct.unpack(
        "<IHHHHIIII", f.read(FILE_HDR_SZ))
    if magic != SPARSE_MAGIC:
        raise SystemExit("not a sparse image")
    if (fhs, chs) != (FILE_HDR_SZ, CHUNK_HDR_SZ):
        raise SystemExit("unexpected header sizes %d/%d" % (fhs, chs))
    return blk, tot, nch


def read_chunks(path):
    """Return (block_size, total_blocks, [(type, blocks, bytes, data_offset)]).

    The chunk list is read with the ABL field order: the third u32 is a block
    count and the fourth is a byte count that includes the 12-byte header.
    """
    f = open(path, "rb")
    blk, tot, nch = read_header(f)
    out = []
    pos = FILE_HDR_SZ
    for _ in range(nch):
        f.seek(pos)
        t, _res, a, b = struct.unpack("<HHII", f.read(CHUNK_HDR_SZ))
        if t in (CHUNK_FILL, CHUNK_DONT_CARE):
            blocks, nbytes = a, CHUNK_HDR_SZ
        elif b - CHUNK_HDR_SZ == a * blk:
            blocks, nbytes = a, b
        else:
            raise SystemExit("%s: chunk is neither field order" % path)
        out.append((t, blocks, nbytes, pos + CHUNK_HDR_SZ))
        pos += nbytes
    f.close()
    return blk, tot, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("pieces")
    a = ap.parse_args()

    names = sorted(p for p in os.listdir(a.pieces) if p.endswith(".simg"))
    if not names:
        raise SystemExit("no pieces in " + a.pieces)
    print("flashing %d pieces in descending order" % len(names))

    # ---- pass 1: where does each written block live? --------------------
    owner = np.full(1, -1, dtype=np.int32)   # block -> piece index
    foff = np.zeros(1, dtype=np.int64)       # block -> byte offset in the piece
    total_blocks = 0

    for pi, name in enumerate(reversed(names)):
        path = os.path.join(a.pieces, name)
        blk, tot, chunks = read_chunks(path)
        if total_blocks == 0:
            total_blocks = tot
        cursor = None
        wrote = 0
        for (t, blocks, nbytes, off) in chunks:
            if t == CHUNK_DONT_CARE:
                if cursor is None:
                    # the piece's leading DONT_CARE states its absolute start
                    cursor = blocks
                    continue
                cursor += blocks          # a seek forward
                continue
            if cursor is None:
                cursor = 0
            if cursor + blocks > total_blocks:
                raise SystemExit("%s: piece writes past the end of the volume" % name)
            if owner.size < cursor + blocks:
                grow = cursor + blocks - owner.size
                owner = np.concatenate([owner, np.full(grow, -1, dtype=np.int32)])
                foff = np.concatenate([foff, np.zeros(grow, dtype=np.int64)])
            prev = owner[cursor:cursor + blocks]
            if np.any(prev != -1):
                raise SystemExit("%s: block %d is written twice"
                                 % (name, cursor + int(np.argmax(prev != -1))))
            owner[cursor:cursor + blocks] = pi
            foff[cursor:cursor + blocks] = off + np.arange(blocks, dtype=np.int64) * blk
            wrote += blocks
            cursor += blocks
        print("  %-12s wrote %d blocks" % (name, wrote))

    # ---- pass 2: compare against the source -----------------------------
    blk, tot, chunks = read_chunks(a.source)
    if tot != total_blocks:
        raise SystemExit("source has %d blocks, the pieces cover %d" % (tot, total_blocks))
    if owner.size < tot:
        grow = tot - owner.size
        owner = np.concatenate([owner, np.full(grow, -1, dtype=np.int32)])
        foff = np.concatenate([foff, np.zeros(grow, dtype=np.int64)])

    src_raw = 0
    written = int(np.count_nonzero(owner != -1))
    problems = 0
    cursor = 0
    for (t, blocks, nbytes, off) in chunks:
        if t != CHUNK_RAW:
            # DONT_CARE / FILL: the source has no data here, so the device
            # must not have written it either (DONT_CARE is a seek).
            got = owner[cursor:cursor + blocks]
            if np.any(got != -1):
                bad = cursor + int(np.argmax(got != -1))
                print("  block %d: the device wrote data where the source has none" % bad)
                problems += 1
            cursor += blocks
            continue

        src_raw += blocks
        seg = owner[cursor:cursor + blocks]
        if np.any(seg == -1):
            bad = cursor + int(np.argmax(seg == -1))
            print("  block %d: not written by any piece" % bad)
            problems += 1
            cursor += blocks
            continue
        # Every block of a source chunk was copied into one piece as one
        # contiguous run, so the piece bytes can be read in one go.
        owners = np.unique(seg)
        if len(owners) != 1:
            print("  block %d: split across pieces" % cursor)
            problems += 1
            cursor += blocks
            continue

        pname = names[len(names) - 1 - int(owners[0])]
        ppath = os.path.join(a.pieces, pname)
        with open(a.source, "rb") as sf, open(ppath, "rb") as pf:
            sf.seek(off)
            want = sf.read(blocks * blk)
            pf.seek(int(foff[cursor]))
            got = pf.read(blocks * blk)
        if want != got:
            problems += 1
            print("  block %d: the piece differs from the source" % cursor)
        cursor += blocks

    print()
    print("source raw blocks: %d, device written blocks: %d" % (src_raw, written))
    if written != src_raw:
        print("!! block counts differ: %d written vs %d in the source" % (written, src_raw))
        problems += 1
    if problems:
        raise SystemExit("SIMULATION FAILED (%d problem(s))" % problems)
    print("SIMULATION OK: the device would hold exactly the source image")


if __name__ == "__main__":
    sys.exit(main())
