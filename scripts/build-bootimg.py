#!/usr/bin/env python3
"""Pack the Aloha boot image with the full raphael DTB table.

unixnu's bootimg insists on a single FDT; the Aloha raphael DTB is a table
whose first FDT is the relevant one. The prebuilt image appends the whole
table, matching the ABL/UEFI expectation, so do the same: patch only the first
FDT's /chosen/linux,initrd-start/-end and keep the rest byte for byte.
"""
import argparse
import gzip
import json
import os
import struct
import sys
import tempfile
import tomllib
import types
from pathlib import Path

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def patch_first_fdt_initrd(blob, start, end):
    """Set /chosen/linux,initrd-start/end in the first FDT of a DTB table."""
    if blob[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit("device tree: bad magic")
    (magic, total, off_struct, off_strings, off_rsv, version, last,
     cpu, size_strings, size_struct) = struct.unpack_from(">10I", blob, 0)
    if total > len(blob) or off_struct + size_struct > total or off_strings + size_strings > total:
        raise SystemExit("device tree: invalid first-blob extents")
    first, rest = blob[:total], blob[total:]
    body = first[off_struct:off_struct + size_struct]
    strings = bytearray(first[off_strings:off_strings + size_strings])

    def be32(at):
        return struct.unpack_from(">I", body, at)[0]

    depth = -1
    in_chosen = False
    insert_at = None
    i = 0
    while i < len(body):
        token = be32(i)
        i += 4
        if token == FDT_BEGIN_NODE:
            nul = body.index(0, i)
            name = body[i:nul].decode()
            i = (nul + 4) & ~3
            depth += 1
            if depth == 1 and name == "chosen":
                in_chosen = True
        elif token == FDT_END_NODE:
            if in_chosen and depth == 1:
                insert_at = i - 4
                break
            depth -= 1
        elif token == FDT_PROP:
            length, nameoff = struct.unpack_from(">II", body, i)
            i += 8 + ((length + 3) & ~3)
        elif token == FDT_NOP:
            pass
        elif token == FDT_END:
            break
        else:
            raise SystemExit(f"device tree: unknown token {token:#x}")
    if insert_at is None:
        raise SystemExit("device tree: no /chosen node")

    def name_offset(name):
        wanted = name.encode() + b"\0"
        if wanted not in strings:
            offset = len(strings)
            strings.extend(wanted)
            return offset
        offset = strings.index(wanted)
        if offset and strings[offset - 1] != 0:
            raise SystemExit(f"device tree: substring match for {name}")
        return offset

    def prop(name, value):
        offset = name_offset(name)
        record = struct.pack(">III", FDT_PROP, len(value), offset) + value
        return record + b"\0" * ((-len(value)) & 3)

    props = prop("linux,initrd-start", struct.pack(">I", start))
    props += prop("linux,initrd-end", struct.pack(">I", end))
    body = body[:insert_at] + props + body[insert_at:]
    patched = bytearray(first[:off_struct]) + body + bytes(strings)
    struct.pack_into(">I", patched, 4, len(patched))
    struct.pack_into(">I", patched, 12, off_struct + len(body))
    struct.pack_into(">I", patched, 32, len(strings))
    struct.pack_into(">I", patched, 36, len(body))
    return bytes(patched) + rest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unixnu" / "scripts"))
sys.path.insert(0, str(ROOT / "scripts"))

from unixnu import bootimg  # noqa: E402
from unixnu.util import run, sha256_file  # noqa: E402
from tree import assemble  # noqa: E402

FW_TARGET = os.environ.get("FW_TARGET", "DEBUG")

parser = argparse.ArgumentParser()
parser.add_argument("--fd", default=str(ROOT / "uefi" / "Build" / "SurfaceDuo1Pkg" / f"{FW_TARGET}_CLANGPDB" / "FV" / "SM8150_EFI.fd"))
parser.add_argument("--dtb", default=str(ROOT / "inputs" / "uefi-prebuilt" / "raphael.dtb"))
parser.add_argument("--ramdisk", default=str(ROOT / "out" / "raphael" / "payload" / "payload.img"))
parser.add_argument("-o", "--output", default=str(ROOT / "out" / "raphael" / "boot" / "boot-raphael.img"))
args = parser.parse_args()

WORK = ROOT / "out" / "raphael"
TREE = assemble(WORK, ROOT / "unixnu", ROOT)
UEFI = ROOT / "uefi"
board = tomllib.loads((ROOT / "board.toml").read_text())
ctx = types.SimpleNamespace(board=board, work=WORK, tree=TREE)
image, base, size, requires_header, config = bootimg.settings(ctx, UEFI)
fv = Path(args.fd).read_bytes()
if len(fv) != size:
    raise SystemExit(f"firmware volume is {len(fv):#x} bytes, expected {size:#x}")
dtb = Path(args.dtb).read_bytes()
ramdisk = Path(args.ramdisk)
if ramdisk.is_file() and image.get("ramdisk_offset") is not None:
    ramdisk_start = (
        image.get("abl_base", 0) + image.get("base", 0) + image["ramdisk_offset"]
    )
    dtb = patch_first_fdt_initrd(
        dtb,
        ramdisk_start,
        ramdisk_start + ramdisk.stat().st_size,
    )
out = Path(args.output)
out.parent.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="unixnu-bootimg-") as scratch_name:
    scratch = Path(scratch_name)
    shim = bootimg.compile_shim(UEFI, base, size, requires_header, scratch)
    combined = shim + fv
    kernel = gzip.compress(combined, mtime=0) + dtb
    kernel_path = scratch / "Image.gz-dtb"
    kernel_path.write_bytes(kernel)
    command = ["python3", str(UEFI / "Resources" / "Scripts" / "mkbootimg.py"),
               "--kernel", str(kernel_path), "--ramdisk", str(ramdisk), "-o", str(out)]
    for key in bootimg.MKBOOTIMG_FLAGS:
        if key in image:
            command += [f"--{key}", str(image[key])]
    run(command)

report = {
    "kernel_sha256": sha256_file(kernel_path) if kernel_path.exists() else None,
    "image_sha256": sha256_file(out),
    "image_bytes": out.stat().st_size,
    "fd_sha256": sha256_file(args.fd),
    "dtb_sha256": sha256_file(args.dtb),
    "dtb_bytes": len(dtb),
    "ramdisk_bytes": ramdisk.stat().st_size,
    "shim_bytes": len(shim),
    "hardware_tested": False,
}
(out.parent / "boot.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"bootimg: {out} ({out.stat().st_size} bytes)")
