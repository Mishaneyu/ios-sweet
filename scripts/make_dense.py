#!/usr/bin/env python3
"""Rewrite a sparse image so that every block is stored explicitly.

Why this exists
---------------
On this phone the bootloader treats a DONT_CARE chunk as a seek: it moves the
write pointer forward and writes nothing.  That is what makes the split flash
work at all -- each piece declares its absolute start block with a leading
DONT_CARE chunk -- but it has a consequence: the blocks a piece skips keep
whatever the partition held before.

The partition is not empty.  It still holds the previous release of the root
(the earlier SHNGROOT build), so after flashing the new image the blocks that
are DONT_CARE in the new one but carried data in the old one keep the old
bytes.  The old image differed from the new one in 310 blocks, and 47 of those
sit inside the HFS+ catalog B-tree, so the volume the kernel would mount is not
the volume the build server verified.

Making the image dense removes the question entirely: every block is stored,
so the partition ends up holding exactly the bytes of the image, whatever was
there before.  It costs 4.7 GiB of extra transfer and nothing else.

    make_dense.py INPUT.simg -o OUTPUT.simg [--verify]

The output is a normal Android sparse image in the layout this bootloader
expects (the chunk header carries the block count before the byte count, the
opposite of libsparse), so split_for_abl.py, verify_pieces.py and
simulate_flash.py all take it unchanged.

Note on chunk sizes: the byte count in a chunk header is a 32 bit field, so a
single chunk can cover at most (2**32 - 1 - 12) / 4096 = 1048575 blocks.  The
source image has chunks far larger than that, so both the input and the output
are emitted as several chunks.

The chunk size also has to stay small enough for split_for_abl.py: that tool
can only cut between chunks, so one oversized chunk produces one oversized
piece -- and a piece larger than ~2 GiB is exactly what the 32-bit fastboot
client cannot send (it dies with std::bad_alloc).  --max-chunk-mib caps the
chunk size for that reason; the default is 128 MiB.
"""
import argparse
import os
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW, CHUNK_FILL, CHUNK_DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
FILE_HDR_SZ, CHUNK_HDR_SZ = 28, 12

# How many blocks we compare per window during --verify.  16384 blocks is
# 64 MiB, small enough that a window never needs more than a couple of
# hundred megabytes of RAM.
VERIFY_WINDOW = 16384


class Sparse:
    """A sparse image: its chunk list plus random access to its blocks."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        magic, _maj, _mn, fhs, chs, self.blk, self.total_blocks, nch, _ck = \
            struct.unpack("<IHHHHIIII", self.f.read(FILE_HDR_SZ))
        if magic != SPARSE_MAGIC:
            raise SystemExit("%s: not an Android sparse image" % path)
        if (fhs, chs) != (FILE_HDR_SZ, CHUNK_HDR_SZ):
            raise SystemExit("%s: unexpected header sizes %d/%d"
                             % (path, fhs, chs))
        # (type, blocks, byte_count, data_offset, first_block)
        self.chunks = []
        pos = FILE_HDR_SZ
        block = 0
        for _ in range(nch):
            self.f.seek(pos)
            t, _res, a, b = struct.unpack("<HHII", self.f.read(CHUNK_HDR_SZ))
            if t == CHUNK_FILL and b == CHUNK_HDR_SZ + 4:
                # Standard libsparse FILL (Renegade's img2simg): a 4-byte
                # value follows the header and repeats over every block.
                blocks, nbytes = a, b
            elif t in (CHUNK_FILL, CHUNK_DONT_CARE):
                blocks, nbytes = a, CHUNK_HDR_SZ
            elif b - CHUNK_HDR_SZ == a * self.blk:
                blocks, nbytes = a, b
            else:
                raise SystemExit("%s: chunk is neither field order" % path)
            self.chunks.append((t, blocks, nbytes, pos + CHUNK_HDR_SZ, block))
            block += blocks
            pos += nbytes
        if block != self.total_blocks:
            raise SystemExit("%s: chunks cover %d blocks, header says %d"
                             % (path, block, self.total_blocks))

    def raw_blocks(self):
        """The blocks that carry data, as a list of (first, count) ranges."""
        out = []
        for (t, blocks, _n, _o, blk0) in self.chunks:
            if t == CHUNK_RAW:
                out.append((blk0, blocks))
        return out

    def read_raw(self, blk0, count):
        """The bytes of `count` blocks starting at block `blk0`."""
        out = bytearray()
        end = blk0 + count
        for (t, blocks, nbytes, off, cblk) in self.chunks:
            if cblk + blocks <= blk0 or cblk >= end:
                continue
            # The window may start in the middle of this chunk.
            delta = blk0 - cblk if blk0 > cblk else 0
            take = min(blocks - delta, count - len(out) // self.blk)
            if t == CHUNK_RAW:
                self.f.seek(off + delta * self.blk)
                out += self.f.read(take * self.blk)
            elif t == CHUNK_FILL and nbytes == CHUNK_HDR_SZ + 4:
                out += self.fill_value(off) * (take * self.blk // 4)
            else:
                out += b"\0" * (take * self.blk)
        return bytes(out)

    def fill_value(self, off):
        """The 4-byte pattern of the FILL chunk whose data starts at `off`."""
        self.f.seek(off)
        return self.f.read(4)


def write_chunks(src, out, max_blocks):
    """Write every block of `src` to `out` as an explicit RAW chunk."""
    if max_blocks < 1:
        raise SystemExit("chunk size must be at least one block")
    nchunks = 0
    zero = b"\0" * (1 << 20)

    def emit(blocks):
        """Write one RAW chunk header plus `blocks` blocks of data."""
        nonlocal nchunks
        nbytes = CHUNK_HDR_SZ + blocks * src.blk
        if nbytes > 0xFFFFFFFF:
            raise SystemExit("chunk of %d blocks does not fit in the header"
                             % blocks)
        out.write(struct.pack("<HHII", CHUNK_RAW, 0, blocks, nbytes))
        nchunks += 1

    for (t, blocks, nbytes, off, _blk0) in src.chunks:
        left = blocks
        if t == CHUNK_RAW:
            src.f.seek(off)
            while left:
                take = min(left, max_blocks)
                emit(take)
                want = take * src.blk
                while want:
                    buf = src.f.read(min(want, 8 << 20))
                    if not buf:
                        raise SystemExit("short read in %s" % src.path)
                    out.write(buf)
                    want -= len(buf)
                left -= take
        elif t == CHUNK_DONT_CARE or (t == CHUNK_FILL and nbytes == CHUNK_HDR_SZ + 4):
            # Store the skipped blocks explicitly, as zeros; FILL blocks as
            # their repeated 4-byte value.
            pattern = zero if t == CHUNK_DONT_CARE else src.fill_value(off) * (len(zero) // 4)
            while left:
                take = min(left, max_blocks)
                emit(take)
                want = take * src.blk
                while want:
                    n = min(want, len(pattern))
                    out.write(pattern[:n])
                    want -= n
                left -= take
        else:
            raise SystemExit("unsupported chunk type %#x" % t)
    return nchunks


def verify(src, dense_path):
    """Compare the dense image with the source, block for block."""
    dense = Sparse(dense_path)
    if dense.total_blocks != src.total_blocks:
        raise SystemExit("block count differs")
    if dense.blk != src.blk:
        raise SystemExit("block size differs")

    bad = 0
    checked = 0
    for start in range(0, src.total_blocks, VERIFY_WINDOW):
        n = min(VERIFY_WINDOW, src.total_blocks - start)
        a = src.read_raw(start, n)
        b = dense.read_raw(start, n)
        if a != b:
            for i in range(n):
                if a[i * src.blk:(i + 1) * src.blk] != \
                        b[i * src.blk:(i + 1) * src.blk]:
                    bad += 1
                    if bad <= 10:
                        print("  block %d differs" % (start + i))
        checked += n
    if bad:
        raise SystemExit("VERIFY FAILED: %d blocks differ" % bad)
    print("  compared %d blocks, all identical" % checked)

    # The blocks the source skipped must now be zeros in the dense image.
    zero_bad = 0
    zero_checked = 0
    for (t, blocks, _n, _o, blk0) in src.chunks:
        if t != CHUNK_DONT_CARE:
            continue
        for s in range(blk0, blk0 + blocks, VERIFY_WINDOW):
            n = min(VERIFY_WINDOW, blk0 + blocks - s)
            data = dense.read_raw(s, n)
            if data.count(0) != len(data):
                zero_bad += 1
                print("  block %d is not zero in the dense image" % s)
            zero_checked += n
    if zero_bad:
        raise SystemExit("VERIFY FAILED: %d skipped ranges are not zero"
                         % zero_bad)
    print("  %d previously skipped blocks are now explicit zeros"
          % zero_checked)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--verify", action="store_true",
                    help="compare the result against the input, block for block")
    ap.add_argument("--max-chunk-mib", type=float, default=128.0,
                    help="maximum payload per chunk; keep it small enough that "
                         "split_for_abl.py can cut the image into pieces")
    args = ap.parse_args()

    src = Sparse(args.input)
    raw = src.raw_blocks()
    raw_blocks = sum(c for _s, c in raw)
    dc_blocks = src.total_blocks - raw_blocks
    max_blocks = int(args.max_chunk_mib * 1024 ** 2) // src.blk
    hard_max = (0xFFFFFFFF - CHUNK_HDR_SZ) // src.blk
    if max_blocks > hard_max:
        max_blocks = hard_max
    if max_blocks < 1:
        raise SystemExit("--max-chunk-mib is too small")
    print("%s: %d chunks, %d blocks (%.2f GiB)"
          % (args.input, len(src.chunks), src.total_blocks,
             src.total_blocks * src.blk / 2 ** 30))
    print("  raw      : %d blocks (%.2f GiB)"
          % (raw_blocks, raw_blocks * src.blk / 2 ** 30))
    print("  dont-care: %d blocks (%.2f GiB)  <- these would keep old bytes"
          % (dc_blocks, dc_blocks * src.blk / 2 ** 30))
    print("  chunk    : %d blocks (%.0f MiB) max"
          % (max_blocks, max_blocks * src.blk / 1024 ** 2))

    with open(args.output, "wb") as out:
        out.write(b"\0" * FILE_HDR_SZ)
        nchunks = write_chunks(src, out, max_blocks)
        out.seek(0)
        out.write(struct.pack("<IHHHHIIII", SPARSE_MAGIC, 1, 0, FILE_HDR_SZ,
                              CHUNK_HDR_SZ, src.blk, src.total_blocks,
                              nchunks, 0))

    size = os.path.getsize(args.output)
    print("dense image: %s (%d bytes, %.2f GiB, %d chunks)"
          % (args.output, size, size / 2 ** 30, nchunks))

    if args.verify:
        print("verifying...")
        verify(src, args.output)
        print("VERIFY OK: the dense image expands to the same bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
