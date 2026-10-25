#!/usr/bin/env python3
"""Inspect Realme X2 Pro (RMX1931 / samurai) live.dtb and stock/custom boot.img.

Prints:
  - boot.img header version, kernel/ramdisk/tags/dtb offsets, page size, cmdline,
    kernel compression (gzip / lz4 / raw), and appended DTB offset (Image.gz-dtb).
  - Live FDT model, compatible, board-id, oppo project/dtsi_no, chosen initrd/bootargs.
  - System RAM banks (/memory) and all /reserved-memory regions (sorted), highlighting
    safe unreserved upper/lower DDR ranges and cont_splash_region + ramoops.
  - Display panel, touch controller (I2C/SPI), GPIO/PMIC buttons, UFS, GICv3, timer.
"""
import gzip
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fdt


def inspect_bootimg(path):
    print(f"=== BOOT IMAGE: {path} ({os.path.getsize(path):,} bytes) ===")
    with open(path, "rb") as fp:
        hdr = fp.read(4096)
    magic = hdr[:8]
    if magic != b"ANDROID!":
        print(f"  Not an ANDROID! boot image (magic={magic!r})")
        return
    (
        kernel_size, kernel_addr,
        ramdisk_size, ramdisk_addr,
        second_size, second_addr,
        tags_addr, page_size,
        header_version, os_version,
    ) = struct.unpack_from("<10I", hdr, 8)
    name = hdr[48:64].split(b"\0", 1)[0].decode(errors="replace")
    cmdline = hdr[64:576].split(b"\0", 1)[0].decode(errors="replace")
    print(f"  header_version : {header_version}")
    print(f"  page_size      : {page_size}")
    print(f"  kernel_size    : {kernel_size:#x} ({kernel_size:,} B) @ {kernel_addr:#010x}")
    print(f"  ramdisk_size   : {ramdisk_size:#x} ({ramdisk_size:,} B) @ {ramdisk_addr:#010x}")
    print(f"  second_size    : {second_size:#x} @ {second_addr:#010x}")
    print(f"  tags_addr      : {tags_addr:#010x}")
    if header_version >= 1:
        rec_dtbo_size, rec_dtbo_off, header_size = struct.unpack_from("<IQ1I", hdr, 1632)
        print(f"  header_size    : {header_size}, recovery_dtbo_size: {rec_dtbo_size:#x}")
    if header_version >= 2:
        dtb_size, dtb_addr = struct.unpack_from("<IQ", hdr, 1652)
        print(f"  dtb_size       : {dtb_size:#x} @ {dtb_addr:#010x}")
    print(f"  name           : {name!r}")
    print(f"  cmdline        : {cmdline}")

    with open(path, "rb") as fp:
        fp.seek(page_size)
        kblob = fp.read(kernel_size)
    if kblob[:3] == b"\x1f\x8b\x08":
        kind = "gzip"
    elif kblob[:4] == b"\x04\x22\x4d\x18":
        kind = "lz4-frame"
    elif kblob[:4] == b"\x02\x21\x4c\x18":
        kind = "lz4-legacy"
    elif kblob[0x38:0x3c] == b"ARM\x64":
        kind = "uncompressed ARM64 Image"
    else:
        kind = f"unknown ({kblob[:8].hex()})"

    # Find any appended FDT blobs (0xd00dfeed) inside the kernel payload
    fdt_offs = []
    pos = 0
    while True:
        idx = kblob.find(b"\xd0\x0d\xfe\xed", pos)
        if idx < 0:
            break
        if idx + 8 <= len(kblob):
            totalsz = struct.unpack_from(">I", kblob, idx + 4)[0]
            if 0x1000 <= totalsz <= len(kblob) - idx:
                fdt_offs.append((idx, totalsz))
                pos = idx + totalsz
                continue
        pos = idx + 4
    print(f"  kernel_format  : {kind}")
    print(f"  appended_dtbs  : {len(fdt_offs)} found -> {[(hex(o), hex(s)) for o, s in fdt_offs]}")
    print()


def inspect_fdt(path):
    blob = open(path, "rb").read()
    root = fdt.parse(blob)
    print(f"=== LIVE FDT: {path} ({len(blob):,} bytes) ===")
    print(f"  model          : {root.strs('model')}")
    print(f"  compatible     : {root.strs('compatible')}")
    for prop in ("qcom,board-id", "qcom,msm-id", "oppo,dtsi_no", "oppo,project"):
        if prop in root.props:
            print(f"  {prop:14s} : {fdt.fmt(root.props[prop])}")

    chosen = root.child("chosen")
    if chosen:
        print(f"  bootargs       : {chosen.str('bootargs')}")

    print("\n--- System Memory (/memory) ---")
    mem_banks = []
    for c in root.children:
        if c.name == "memory" or c.name.startswith("memory@"):
            for addr, size in c.reg():
                mem_banks.append((addr, size))
                print(f"  RAM: {addr:#012x} .. {addr + size:#012x} ({size / (1024**3):.2f} GiB)")

    print("\n--- Reserved Memory (/reserved-memory) ---")
    rsv = root.child("reserved-memory")
    rsv_list = []
    if rsv:
        for c in rsv.children:
            regs = c.reg()
            for addr, size in regs:
                rsv_list.append((addr, size, c.name, "no-map" in c.props))
        for addr, size, name, nomap in sorted(rsv_list):
            tag = " [no-map]" if nomap else ""
            mark = "  <-- IMPORTANT" if any(k in name for k in ("splash", "ramoops", "tz", "hyp")) else ""
            print(f"  {addr:#012x} .. {addr + size:#012x} ({size >> 10:7d} KiB)  {name}{tag}{mark}")

    print("\n--- Key Peripherals (Touch / Display / Keys / UFS / GIC) ---")
    for n in root.walk():
        compats = n.strs("compatible")
        p = n.path.lower()
        if any(
            any(k in c.lower() for k in ("gic-v3", "armv8-timer", "ufshc", "goodix", "synaptics", "focaltech", "touch", "gpio-keys", "qpnp-power-on", "dsi-panel"))
            for c in compats
        ) or any(k in p for k in ("touchscreen", "tpd", "synaptics", "goodix_ts", "focaltech", "dsi_samsung", "sofef")):
            status = n.str("status") or "okay"
            if status == "disabled":
                continue
            regs = ["%#x/%#x" % r for r in n.reg()]
            print(f"  {n.path}  compat={compats}  reg={regs}")


if __name__ == "__main__":
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    boot_p = os.path.join(root_dir, "backup", "boot.img")
    dtb_p = os.path.join(root_dir, "dtb", "live.dtb")
    if os.path.exists(boot_p):
        inspect_bootimg(boot_p)
    if os.path.exists(dtb_p):
        inspect_fdt(dtb_p)
    if not os.path.exists(boot_p) and not os.path.exists(dtb_p):
        print("Neither backup/boot.img nor dtb/live.dtb exists yet. Run tools/rmx1931-pull-info.ps1 first.")
