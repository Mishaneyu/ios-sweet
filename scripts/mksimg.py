#!/usr/bin/env python3
"""Convert a raw image to an Android sparse image, streaming.

Why: `fastboot -S <n> flash` loads the WHOLE raw image into RAM to sparsify it,
which dies with std::bad_alloc on an 8 GB image. Feeding fastboot an
already-sparse file lets it stream chunk by chunk instead.

The converter itself must also stream: an HFS+ iOS root is ~9 GB of data, and
buffering it in RAM does not fit next to the build. Raw runs are capped at
--max-chunk-blocks and written out immediately; the file header is patched at
the end (total_chunks is only known then).

Android sparse format (little endian):

  file header (28 bytes)
    u32 magic        0xED26FF3A
    u16 major_ver    1
    u16 minor_ver    0
    u16 file_hdr_sz  28
    u16 chunk_hdr_sz 12
    u32 blk_sz       block size in bytes
    u32 total_blks   blocks in the ORIGINAL image
    u32 total_chunks number of chunks that follow
    u32 image_crc    0 (unused)

  chunk header (12 bytes)
    u16 chunk_type   0xCAC1 raw / 0xCAC2 fill / 0xCAC3 don't care
    u16 reserved
    u32 total_sz     blocks covered by this chunk      <-- NOTE: reversed!
    u32 chunk_sz     bytes of this chunk including its header

  AOSP's libsparse declares these the other way round (chunk_sz first).  The
  mi 9 / K20 Pro bootloader does NOT follow it: it reads the third field as a
  block count and the fourth as a byte count.  Feed it a spec-compliant file
  and it reports "Error flashing partition : Volume Full", because it treats
  e.g. 1048588 as 1048588 *blocks* = 4 GiB.  Verified on raphael with 4 MiB
  probes (identical payload, only the two u32 swapped): spec order -> Volume
  Full, this order -> OKAY.  Keep this order; mksimg clients on this device
  are ABL, not simg2img.
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_DONT_CARE = 0xCAC3
FILE_HDR_SZ = 28
CHUNK_HDR_SZ = 12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--block-size", type=int, default=4096)
    ap.add_argument("--max-chunk-blocks", type=int, default=16384,
                    help="cap raw chunks (16384 x 4096 = 64 MiB)")
    args = ap.parse_args()

    bs = args.block_size
    src = Path(args.raw)
    size = src.stat().st_size
    total_blks = (size + bs - 1) // bs
    cap_bytes = args.max_chunk_blocks * bs

    n_raw = n_dc = 0
    raw_bytes = 0
    zero_run = 0
    buf = bytearray()

    f = open(src, "rb", buffering=0)
    out = open(args.output, "wb")

    def write_raw():
        nonlocal buf, n_raw, raw_bytes
        if not buf:
            return
        out.write(struct.pack("<HHII", CHUNK_RAW, 0, len(buf) // bs, CHUNK_HDR_SZ + len(buf)))
        out.write(buf)
        n_raw += 1
        raw_bytes += len(buf)
        buf = bytearray()

    def write_dontcare():
        nonlocal zero_run, n_dc
        if not zero_run:
            return
        out.write(struct.pack("<HHII", CHUNK_DONT_CARE, 0, zero_run, CHUNK_HDR_SZ))
        n_dc += 1
        zero_run = 0

    out.write(b"\0" * FILE_HDR_SZ)          # placeholder, patched below

    # Read in 1 MiB slabs, then test block by block.
    slab = 1 << 20
    idx = 0
    while True:
        chunk = f.read(slab)
        if not chunk:
            break
        pad = (-len(chunk)) % bs
        if pad:
            chunk = chunk + b"\0" * pad
        for off in range(0, len(chunk), bs):
            blk = chunk[off:off + bs]
            if blk.count(0) == bs:
                write_raw()
                zero_run += 1
            else:
                write_dontcare()
                buf += blk
                if len(buf) >= cap_bytes:
                    write_raw()
            idx += 1
        if idx % 262144 == 0:
            done = min(idx * bs, size)
            print("  %5.1f%%  %d / %d blocks" % (100.0 * done / size, idx, total_blks),
                  flush=True)
    f.close()

    # Tail: the loop counted padded blocks; drop any that extend past the image.
    if idx > total_blks:
        over = idx - total_blks
        if over * bs <= len(buf):
            buf = buf[:len(buf) - over * bs]
        idx = total_blks
    write_raw()
    write_dontcare()

    total_chunks = n_raw + n_dc
    covered = raw_bytes // bs
    # Recompute coverage exactly: raw blocks + dont-care blocks.
    out.seek(0, 2)
    out_size = out.tell()
    # dont-care blocks are implied by total_blks - raw blocks.
    dc_blocks = total_blks - covered
    if dc_blocks < 0:
        sys.exit("error: raw blocks exceed image blocks")

    out.seek(0)
    out.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, FILE_HDR_SZ, CHUNK_HDR_SZ,
                          bs, total_blks, total_chunks, 0))
    out.close()

    print("sparse image: %s" % args.output)
    print("  original   : %d bytes (%.3f GiB), %d blocks" % (size, size / 1024 ** 3, total_blks))
    print("  chunks     : %d (%d raw, %d don't-care)" % (total_chunks, n_raw, n_dc))
    print("  data bytes : %d (%.3f GiB)" % (raw_bytes, raw_bytes / 1024 ** 3))
    print("  output size: %d bytes (%.3f GiB)" % (out_size, out_size / 1024 ** 3))


if __name__ == "__main__":
    main()
