#!/usr/bin/env python3
"""Verify the ABL pieces against the original sparse image, byte for byte.

The pieces are what actually reaches the phone, so they are checked against
the source image rather than trusted: for every RAW chunk in a piece the
payload must equal the corresponding blocks of the source sparse image, and
the pieces together must cover the source's whole block range exactly once.

Chunk header layout (the order this bootloader expects):
    u16 type, u16 reserved, u32 blocks, u32 total_bytes
`total_bytes` counts the 12-byte header itself, so a RAW chunk carries
total_bytes - 12 bytes of payload and a DONT_CARE chunk carries none.

    verify_pieces.py SOURCE.simg PIECE_DIR
"""
import os
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW, CHUNK_FILL, CHUNK_DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
FILE_HDR_SZ, CHUNK_HDR_SZ = 28, 12


class Sparse:
    """A sparse image: the chunk list plus random access to its blocks.

    `piece=True` describes a split piece, whose first chunk is a DONT_CARE
    prefix whose blocks field is the ABSOLUTE start block of the piece
    (that is how the bootloader learns where to place it).  A whole image
    instead starts at block 0 and every chunk counts blocks from there.
    """

    def __init__(self, path, piece=False):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(FILE_HDR_SZ)
        magic, _maj, _mn, fhs, chs, blk, tot, nch, _ck = struct.unpack(
            "<IHHHHIIII", hdr)
        if magic != SPARSE_MAGIC:
            raise SystemExit("%s: not a sparse image" % path)
        if (fhs, chs) != (FILE_HDR_SZ, CHUNK_HDR_SZ):
            raise SystemExit("%s: unexpected header sizes %d/%d" % (path, fhs, chs))
        self.blk = blk
        self.header_blocks = tot
        self.chunks = []
        pos = FILE_HDR_SZ
        block = 0
        for i in range(nch):
            self.f.seek(pos)
            t, _res, a, b = struct.unpack("<HHII", self.f.read(CHUNK_HDR_SZ))
            self.chunks.append((t, a, b, pos + CHUNK_HDR_SZ, block))
            if piece and i == 0:
                # absolute start block, not a length
                block = a
            else:
                block += a
            pos += b            # total_bytes includes the header
        self.total_blocks = block

    def raw_chunks(self):
        """Yield (block, nblocks, payload_bytes) for the RAW chunks."""
        for (t, a, b, off, block) in self.chunks:
            if t == CHUNK_RAW:
                self.f.seek(off)
                yield block, a, self.f.read(b - CHUNK_HDR_SZ)

    def blocks(self, start, count):
        """The content of `count` blocks starting at block `start`."""
        out = bytearray()
        for (t, a, b, off, block) in self.chunks:
            if block + a <= start or block >= start + count:
                continue
            take = min(a - (start - block), count - len(out) // self.blk)
            if t == CHUNK_RAW:
                self.f.seek(off + (start - block) * self.blk)
                out += self.f.read(take * self.blk)
            else:
                out += b"\0" * (take * self.blk)
        return bytes(out)


def main():
    src_path, pdir = sys.argv[1], sys.argv[2]
    src = Sparse(src_path)
    print("source: %d blocks of %d B (%.2f GiB)"
          % (src.total_blocks, src.blk, src.total_blocks * src.blk / 2 ** 30))

    names = sorted(p for p in os.listdir(pdir) if p.endswith(".simg"))
    if not names:
        raise SystemExit("no pieces in " + pdir)

    covered = [0] * src.total_blocks
    bad = 0
    for name in names:
        path = os.path.join(pdir, name)
        piece = Sparse(path, piece=True)
        # the leading DONT_CARE chunk states the piece's absolute start block
        prefix = 0
        if piece.chunks and piece.chunks[0][0] == CHUNK_DONT_CARE:
            prefix = piece.chunks[0][1]
        nblocks = piece.total_blocks - prefix
        if piece.header_blocks != piece.total_blocks:
            print("  %-12s header says %d blocks, chunks cover %d"
                  % (name, piece.header_blocks, piece.total_blocks))
        # every RAW chunk must equal the source at the same absolute block
        for (block, a, data) in piece.raw_chunks():
            want = src.blocks(block, a)
            if data != want:
                bad += 1
                print("  %-12s RAW chunk at block %d differs from the source"
                      % (name, block))
        print("  %-12s blocks %8d..%-8d (%d blocks, %.3f GiB)"
              % (name, prefix, prefix + nblocks, nblocks, nblocks * piece.blk / 2 ** 30))
        for b in range(prefix, prefix + nblocks):
            covered[b] += 1

    gaps = [b for b, c in enumerate(covered) if c == 0]
    dups = [b for b, c in enumerate(covered) if c > 1]
    print()
    print("coverage: %d/%d blocks covered exactly once"
          % (sum(1 for c in covered if c == 1), src.total_blocks))
    print("gaps: %d, duplicated: %d" % (len(gaps), len(dups)))
    if gaps:
        print("  first gap at block %d" % gaps[0])
    if dups:
        print("  first duplicate at block %d" % dups[0])

    if bad or gaps or dups:
        raise SystemExit("VERIFY FAILED")
    print("VERIFY OK: the pieces reproduce the source image exactly")


if __name__ == "__main__":
    main()
