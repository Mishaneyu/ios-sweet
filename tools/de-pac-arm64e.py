#!/usr/bin/env python3
"""Offline in-place ARMv8.3-A arm64e -> ARMv8.0-A executable opcode rewriter.

  de-pac-arm64e.py [OPTIONS] [FILE_OR_DIR ...]
  de-pac-arm64e.py --root ROOTFS_DIR [--dry-run] [--zero-pacga] [--set-subtype-arm64]

Rewrites fixed-width 4-byte non-hint arm64e PAC opcodes in-place so unmodified
iOS 26 (arm64e) binaries and dyld_shared_cache_arm64e run natively at full speed
on ARMv8.2-A Qualcomm Kryo 485 (Cortex-A55 / Cortex-A76) cores without trapping
on every call, return, indirect branch, pointer sign/auth, or vtable load:

  - retaa / retab                              -> ret
  - braaz / brabz / braa / brab                -> br Xn
  - blraaz / blrabz / blraa / blrab            -> blr Xn
  - pacia..autdb, paciza..autdzb, xpaci, xpacd -> nop
  - ldraa / ldrab (ONLY when W == 0, S == 0)   -> ldr Xt, [Xn, #uimm]
  - ldraa / ldrab (S == 1 or W == 1)           -> left untouched for XNU sleh
  - pacga Xd, Xn, Xm                           -> left untouched for XNU sleh
                                                  (or mov Xd, xzr with --zero-pacga)
  - HINT-space PAC ops (paciasp/autiasp/...)   -> left untouched (hardware NOPs on v8.0+)

Strict data-in-code safety:
  Only 4-byte words inside sections flagged S_ATTR_PURE_INSTRUCTIONS (0x80000000)
  or S_ATTR_SOME_INSTRUCTIONS (0x00000400) are scanned, and every [offset, offset+len)
  entry in LC_DATA_IN_CODE is strictly excluded.

Supports:
  - Thin 64-bit Mach-O files (MH_MAGIC_64)
  - Fat universal binaries (FAT_MAGIC / FAT_MAGIC_64, CPU_TYPE_ARM64 slices)
  - Monolithic and split dyld_shared_cache_arm64e (+ .01, .02, ... subcaches)
  - Directory tree mode (--root DIR, skipping symlinks & deduplicating hardlinks)
"""
import argparse
import bisect
import ctypes
import hashlib
import json
import mmap
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time

MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe
FAT_MAGIC_64 = 0xcafebabf

CPU_TYPE_ARM64 = 0x0100000c
CPU_SUBTYPE_ARM64_ALL = 0
CPU_SUBTYPE_ARM64E = 2

MH_OBJECT = 0x1

LC_SEGMENT_64 = 0x19
LC_DATA_IN_CODE = 0x29

VM_PROT_EXECUTE = 0x04

S_ZEROFILL = 0x1
S_GB_ZEROFILL = 0xc
S_THREAD_LOCAL_ZEROFILL = 0x12
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400
INSN_SECT_MASK = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
ZEROFILL_TYPES = (S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL)

STAT_NAMES = (
    'reta',
    'bra',
    'blra',
    'pac_aut_xpac',
    'ldra',
    'pacga_zeroed',
    'ldra_sleh',
    'pacga_sleh',
)

SUBCACHE_RE = re.compile(r'^(.*dyld_shared_cache_[a-z0-9_]+?)(\.\d+(?:\.[a-z]+)?|\.symbols|\.dylddata)$')


def load_fast_lib(force_pure_python=False):
    """Compile and load tools/de_pac_fast.c if a host C compiler is available."""
    if force_pure_python:
        return None
    c_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'de_pac_fast.c')
    if not os.path.isfile(c_path):
        return None
    src_hash = hashlib.sha256(open(c_path, 'rb').read()).hexdigest()[:12]
    ext = '.dll' if os.name == 'nt' else ('.dylib' if sys.platform == 'darwin' else '.so')
    so_path = os.path.join(tempfile.gettempdir(), f'de_pac_fast_{src_hash}{ext}')
    if not os.path.isfile(so_path):
        cc = next((c for c in ('cc', 'clang', 'gcc') if shutil.which(c)), None)
        if not cc:
            return None
        tmp_so = f'{so_path}.{os.getpid()}.tmp'
        cmd = [cc, '-O3', '-shared', '-fPIC', c_path, '-o', tmp_so]
        if os.name == 'nt':
            cmd = [cc, '-O3', '-shared', c_path, '-o', tmp_so]
        try:
            subprocess.run(cmd, check=True, capture_output=True)
            os.replace(tmp_so, so_path)
        except Exception:
            if os.path.exists(tmp_so):
                try:
                    os.unlink(tmp_so)
                except OSError:
                    pass
            return None
    try:
        lib = ctypes.CDLL(so_path)
        lib.de_pac_ranges.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint64),
        ]
        lib.de_pac_ranges.restype = ctypes.c_uint64
        return lib
    except Exception:
        return None


def rewrite_ranges_py(buf, ranges, zero_pacga=False, dry_run=False):
    """Pure-Python reference implementation matching tools/de_pac_fast.c bit-for-bit."""
    stats = [0] * 8
    rewritten = 0
    buf_len = len(buf)
    unpack_u32 = struct.unpack_from
    pack_u32 = struct.pack_into

    for off, length in ranges:
        if off >= buf_len:
            continue
        if length > buf_len - off:
            length = buf_len - off
        length &= ~3
        end = off + length
        pos = off
        while pos < end:
            insn, = unpack_u32('<I', buf, pos)
            top = insn >> 28
            if top in (0x9, 0xd, 0xf):
                out = insn
                changed = False
                if (insn & 0xfffffbff) == 0xd65f0bff:
                    stats[0] += 1
                    out = 0xd65f03c0
                    changed = True
                elif (insn & 0xfffff81f) == 0xd61f081f or (insn & 0xfffff800) == 0xd71f0800:
                    stats[1] += 1
                    out = 0xd61f0000 | (insn & 0x3e0)
                    changed = True
                elif (insn & 0xfffff81f) == 0xd63f081f or (insn & 0xfffff800) == 0xd73f0800:
                    stats[2] += 1
                    out = 0xd63f0000 | (insn & 0x3e0)
                    changed = True
                elif ((insn & 0xffffe000) == 0xdac10000 or
                      (insn & 0xffffe3e0) == 0xdac123e0 or
                      (insn & 0xfffffbe0) == 0xdac143e0):
                    stats[3] += 1
                    out = 0xd503201f
                    changed = True
                elif (insn & 0xff200400) == 0xf8200400:
                    if (insn & 0x00400800) == 0:
                        imm9 = (insn >> 12) & 0x1ff
                        stats[4] += 1
                        out = 0xf9400000 | (imm9 << 10) | (insn & 0x3ff)
                        changed = True
                    else:
                        stats[6] += 1
                elif (insn & 0xffe0fc00) == 0x9ac03000:
                    if zero_pacga:
                        stats[5] += 1
                        out = 0xaa1f03e0 | (insn & 0x1f)
                        changed = True
                    else:
                        stats[7] += 1
                if changed:
                    rewritten += 1
                    if not dry_run:
                        pack_u32('<I', buf, pos, out)
            pos += 4

    return rewritten, stats


def rewrite_ranges(buf, ranges, fast_lib=None, zero_pacga=False, dry_run=False):
    """Dispatch validated (offset, length) intervals to C fast-path or Python fallback."""
    if not ranges:
        return 0, [0] * 8
    if fast_lib is None:
        return rewrite_ranges_py(buf, ranges, zero_pacga=zero_pacga, dry_run=dry_run)

    nranges = len(ranges)
    flat = (ctypes.c_uint64 * (2 * nranges))()
    for i, (off, length) in enumerate(ranges):
        flat[2 * i] = off
        flat[2 * i + 1] = length
    c_stats = (ctypes.c_uint64 * 8)()
    c_buf = (ctypes.c_uint8 * len(buf)).from_buffer(buf)
    rewritten = fast_lib.de_pac_ranges(
        c_buf, len(buf), flat, nranges, int(bool(zero_pacga)), int(bool(dry_run)), c_stats
    )
    return int(rewritten), [int(c_stats[i]) for i in range(8)]


def merge_intervals(intervals):
    """Sort and merge overlapping/adjacent [start, end) intervals."""
    pts = sorted((s, e) for s, e in intervals if e > s)
    if not pts:
        return []
    merged = [pts[0]]
    for s, e in pts[1:]:
        ps, pe = merged[-1]
        if s <= pe:
            if e > pe:
                merged[-1] = (ps, e)
        else:
            merged.append((s, e))
    return merged


def subtract_intervals(base_intervals, excl_intervals):
    """Subtract excl_intervals (expanded to 4-byte instruction boundaries) from 4-byte-aligned base_intervals."""
    excl = merge_intervals((s & ~3, (e + 3) & ~3) for s, e in excl_intervals if e > s)
    excl_starts = [s for s, _ in excl]
    out = []
    for bs, be in merge_intervals(base_intervals):
        bs = (bs + 3) & ~3
        be = be & ~3
        if bs >= be:
            continue
        idx = bisect.bisect_right(excl_starts, bs)
        if idx > 0:
            idx -= 1
        cur = bs
        while idx < len(excl) and cur < be:
            es, ee = excl[idx]
            if ee <= cur:
                idx += 1
                continue
            if es >= be:
                break
            if es > cur:
                out.append((cur, es))
            cur = ee
            idx += 1
        if cur < be:
            out.append((cur, be))
    return out


def parse_macho_slice_intervals(buf, slice_off, slice_size):
    """Parse a standalone 64-bit ARM64 Mach-O slice and return file-relative (off, len) code ranges + header info."""
    if slice_size < 32:
        return None
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, slice_off)
    if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
        return None
    if 32 + sizeofcmds > slice_size:
        return None

    exec_sects = []  # (vm_start, vm_end, slice_rel_off)
    text_vmaddr = None
    first_seg_vmaddr = None
    dice_cmd = None

    off = slice_off + 32
    cmd_end = off + sizeofcmds
    for _ in range(ncmds):
        if off + 8 > cmd_end:
            break
        cmd, cmdsize = struct.unpack_from('<II', buf, off)
        if cmdsize < 8 or off + cmdsize > cmd_end:
            break
        if cmd == LC_SEGMENT_64 and cmdsize >= 72:
            segname = bytes(buf[off + 8:off + 24]).split(b'\0', 1)[0]
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _ = struct.unpack_from('<4Q4I', buf, off + 24)
            if first_seg_vmaddr is None:
                first_seg_vmaddr = vmaddr
            if segname == b'__TEXT':
                text_vmaddr = vmaddr
            seg_exec_ok = (ftype == MH_OBJECT) or (maxprot == 0) or bool(maxprot & VM_PROT_EXECUTE)
            if seg_exec_ok and off + 72 + nsects * 80 <= off + cmdsize:
                for i in range(nsects):
                    so = off + 72 + i * 80
                    s_addr, s_size, s_off, _, _, _, s_flags = struct.unpack_from('<2Q5I', buf, so + 32)
                    if (s_flags & INSN_SECT_MASK) and (s_flags & 0xff) not in ZEROFILL_TYPES and s_size > 0:
                        if s_off + s_size <= slice_size:
                            exec_sects.append((s_addr, s_addr + s_size, s_off))
        elif cmd == LC_DATA_IN_CODE and cmdsize >= 16:
            doff, dsize = struct.unpack_from('<II', buf, off + 8)
            if dsize > 0 and doff + dsize <= slice_size:
                dice_cmd = (doff, dsize)
        off += cmdsize

    base_vm = text_vmaddr if text_vmaddr is not None else (first_seg_vmaddr or 0)
    dice_vm = []
    if dice_cmd is not None:
        doff, dsize = dice_cmd
        abs_doff = slice_off + doff
        for j in range(0, dsize - (dsize % 8), 8):
            e_off, e_len, _ = struct.unpack_from('<IHH', buf, abs_doff + j)
            if e_len:
                dice_vm.append((base_vm + e_off, base_vm + e_off + e_len))

    file_ranges = []
    for s_vm_start, s_vm_end, s_rel_off in exec_sects:
        clean_vm = subtract_intervals([(s_vm_start, s_vm_end)], dice_vm)
        for cs, ce in clean_vm:
            f_start = slice_off + s_rel_off + (cs - s_vm_start)
            file_ranges.append((f_start, ce - cs))

    return {
        'cpusub': cpusub,
        'mh_off': slice_off,
        'ranges': file_ranges,
    }


def parse_standalone_file(mm):
    """Return list of slice dicts + optional fat_archcpusub patch locations for thin or fat Mach-O."""
    size = len(mm)
    if size < 32:
        return None
    le_magic, = struct.unpack_from('<I', mm, 0)
    be_magic, = struct.unpack_from('>I', mm, 0)

    if le_magic == MH_MAGIC_64:
        sl = parse_macho_slice_intervals(mm, 0, size)
        return [sl] if sl else None

    if be_magic in (FAT_MAGIC, FAT_MAGIC_64):
        _, nfat = struct.unpack_from('>II', mm, 0)
        entry_sz = 20 if be_magic == FAT_MAGIC else 32
        if nfat == 0 or nfat > 32 or 8 + nfat * entry_sz > size:
            return None
        slices = []
        for i in range(nfat):
            eo = 8 + i * entry_sz
            if be_magic == FAT_MAGIC:
                cputype, cpusub, soff, ssize, align = struct.unpack_from('>IIIII', mm, eo)
            else:
                cputype, cpusub, soff, ssize, align, _ = struct.unpack_from('>IIQQII', mm, eo)
            if align > 30 or soff < 8 + nfat * entry_sz or soff + ssize > size:
                return None
            if cputype == CPU_TYPE_ARM64:
                sl = parse_macho_slice_intervals(mm, soff, ssize)
                if sl is not None:
                    sl['fat_cpusub_off'] = eo + 4
                    slices.append(sl)
        return slices if slices else None

    return None


class DyldCacheSet:
    """Unified VM view across a dyld_shared_cache_arm64e and its split subcaches (.01, .02, ...)."""

    def __init__(self, main_path):
        self.main_path = os.path.abspath(main_path)
        self.files = []       # list of paths, index 0 = main_path
        self.mappings = []    # sorted by vm_start: (vm_start, vm_end, file_idx, file_off, max_prot)
        self._vm_starts = []
        self._fds = {}

        self._add_cache_file(self.main_path)
        hdr = self._pread(0, 0, 0x200)
        mapping_off, _ = struct.unpack_from('<II', hdr, 0x10)
        if mapping_off >= 0x190:
            sc_off, sc_cnt = struct.unpack_from('<II', hdr, 0x188)
            if sc_off and sc_cnt:
                entry_sz = 56 if mapping_off >= 0x1c8 else 24
                sc_buf = self._pread(0, sc_off, sc_cnt * entry_sz)
                for i in range(sc_cnt):
                    if entry_sz == 56:
                        suffix = sc_buf[i * 56 + 24:i * 56 + 56].split(b'\0', 1)[0].decode('ascii', errors='ignore')
                    else:
                        suffix = f'.{i + 1}'
                    sc_path = self.main_path + suffix
                    if os.path.isfile(sc_path):
                        self._add_cache_file(sc_path)
        sym_path = self.main_path + '.symbols'
        if os.path.isfile(sym_path) and os.path.abspath(sym_path) not in self.files:
            self._add_cache_file(sym_path)

        self.mappings.sort(key=lambda m: m[0])
        self._vm_starts = [m[0] for m in self.mappings]

    def close(self):
        for fd in self._fds.values():
            try:
                os.close(fd)
            except OSError:
                pass
        self._fds.clear()

    def _pread(self, file_idx, off, size):
        fd = self._fds.get(file_idx)
        if fd is None:
            fd = os.open(self.files[file_idx], os.O_RDONLY | getattr(os, 'O_BINARY', 0))
            self._fds[file_idx] = fd
        if hasattr(os, 'pread'):
            return os.pread(fd, size, off)
        os.lseek(fd, off, os.SEEK_SET)
        return os.read(fd, size)

    def _add_cache_file(self, path):
        apath = os.path.abspath(path)
        if apath in self.files:
            return
        file_idx = len(self.files)
        self.files.append(apath)
        fsize = os.path.getsize(apath)
        if fsize < 0x20:
            return
        hdr = self._pread(file_idx, 0, min(0x200, fsize))
        if not hdr[:8] == b'dyld_v1 ':
            return
        map_off, map_cnt = struct.unpack_from('<II', hdr, 0x10)
        if map_off == 0 or map_cnt == 0 or map_off + map_cnt * 32 > fsize:
            return
        mbuf = self._pread(file_idx, map_off, map_cnt * 32)
        for i in range(map_cnt):
            addr, sz, foff, max_prot, _ = struct.unpack_from('<3Q2I', mbuf, i * 32)
            if sz > 0 and foff + sz <= fsize:
                self.mappings.append((addr, addr + sz, file_idx, foff, max_prot))

    def read_vm(self, vm_addr, size):
        """Read `size` contiguous bytes starting at unslid `vm_addr`."""
        idx = bisect.bisect_right(self._vm_starts, vm_addr) - 1
        if idx < 0:
            return None
        ms, me, file_idx, foff, _ = self.mappings[idx]
        if vm_addr < ms or vm_addr + size > me:
            return None
        return self._pread(file_idx, foff + (vm_addr - ms), size)

    def vm_to_exec_file_ranges(self, vm_start, vm_end):
        """Map an executable [vm_start, vm_end) interval to (file_idx, file_off, length) tuples."""
        out = []
        idx = bisect.bisect_right(self._vm_starts, vm_start)
        if idx > 0:
            idx -= 1
        cur = vm_start
        while idx < len(self.mappings) and cur < vm_end:
            ms, me, file_idx, foff, max_prot = self.mappings[idx]
            if me <= cur:
                idx += 1
                continue
            if ms >= vm_end:
                break
            seg_s = max(cur, ms)
            seg_e = min(vm_end, me)
            if (max_prot & VM_PROT_EXECUTE) and seg_e > seg_s:
                out.append((file_idx, foff + (seg_s - ms), seg_e - seg_s))
            cur = seg_e
            idx += 1
        return out

    def image_header_vms(self):
        """Return deduplicated unslid Mach-O header VAs for all cached dylibs, embedded dyld, and branch pools."""
        hdr = self._pread(0, 0, min(0x200, os.path.getsize(self.files[0])))
        map_off, _ = struct.unpack_from('<II', hdr, 0x10)
        seen = set()
        vms = []

        def add(va):
            if va and va not in seen:
                seen.add(va)
                vms.append(va)

        # 1. Modern imagesOffset / imagesCount (0x1c0) or legacy imagesOffsetOld / imagesCountOld (0x18)
        img_off, img_cnt = (0, 0)
        if map_off >= 0x1c8:
            img_off, img_cnt = struct.unpack_from('<II', hdr, 0x1c0)
        if not img_cnt:
            img_off, img_cnt = struct.unpack_from('<II', hdr, 0x18)
        if img_off and img_cnt:
            ibuf = self._pread(0, img_off, img_cnt * 32)
            for i in range(img_cnt):
                va, = struct.unpack_from('<Q', ibuf, i * 32)
                add(va)

        # 2. imagesTextOffset / imagesTextCount (0x88)
        if map_off >= 0x98:
            it_off, it_cnt = struct.unpack_from('<QQ', hdr, 0x88)
            if it_off and it_cnt:
                tbuf = self._pread(0, it_off, it_cnt * 32)
                for i in range(it_cnt):
                    va, = struct.unpack_from('<Q', tbuf, i * 32 + 16)
                    add(va)

        # 3. Embedded dyld Mach-O in cache (dyldInCacheMH at 0x78)
        if map_off >= 0x80:
            dyld_mh, = struct.unpack_from('<Q', hdr, 0x78)
            add(dyld_mh)

        # 4. Branch island pools (branchPoolsOffset / branchPoolsCount at 0x70)
        if map_off >= 0x78:
            bp_off, bp_cnt = struct.unpack_from('<II', hdr, 0x70)
            if bp_off and bp_cnt:
                bbuf = self._pread(0, bp_off, bp_cnt * 8)
                for i in range(bp_cnt):
                    va, = struct.unpack_from('<Q', bbuf, i * 8)
                    add(va)

        return vms

    def collect_exec_ranges_by_file(self):
        """Parse every Mach-O in the cache set, exclude LC_DATA_IN_CODE, and group ranges by file_idx."""
        by_file = {i: [] for i in range(len(self.files))}
        mh_patches = []  # (file_idx, file_off, cpusub)

        for mh_va in self.image_header_vms():
            mh = self.read_vm(mh_va, 32)
            if not mh or len(mh) < 32:
                continue
            magic, cputype, cpusub, _, ncmds, sizeofcmds, _, _ = struct.unpack_from('<8I', mh, 0)
            if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
                continue
            full_hdr = self.read_vm(mh_va, 32 + sizeofcmds)
            if not full_hdr or len(full_hdr) < 32 + sizeofcmds:
                continue

            # Record Mach-O header location in case --set-subtype-arm64 is requested
            idx = bisect.bisect_right(self._vm_starts, mh_va) - 1
            if idx >= 0:
                ms, me, fidx, foff, _ = self.mappings[idx]
                if ms <= mh_va and mh_va + 32 <= me:
                    mh_patches.append((fidx, foff + (mh_va - ms), cpusub))

            exec_vm = []
            text_vmaddr = mh_va
            le_vmaddr = None
            le_fileoff = None
            dice_cmd = None

            off = 32
            cmd_end = 32 + sizeofcmds
            for _ in range(ncmds):
                if off + 8 > cmd_end:
                    break
                cmd, cmdsize = struct.unpack_from('<II', full_hdr, off)
                if cmdsize < 8 or off + cmdsize > cmd_end:
                    break
                if cmd == LC_SEGMENT_64 and cmdsize >= 72:
                    segname = full_hdr[off + 8:off + 24].split(b'\0', 1)[0]
                    vmaddr, vmsize, fileoff, filesize, maxprot, _, nsects, _ = struct.unpack_from(
                        '<4Q4I', full_hdr, off + 24
                    )
                    if segname == b'__TEXT':
                        text_vmaddr = vmaddr
                    elif segname == b'__LINKEDIT':
                        le_vmaddr = vmaddr
                        le_fileoff = fileoff
                    if (maxprot == 0 or (maxprot & VM_PROT_EXECUTE)) and off + 72 + nsects * 80 <= off + cmdsize:
                        for i in range(nsects):
                            so = off + 72 + i * 80
                            s_addr, s_size, _, _, _, _, s_flags = struct.unpack_from('<2Q5I', full_hdr, so + 32)
                            if (s_flags & INSN_SECT_MASK) and (s_flags & 0xff) not in ZEROFILL_TYPES and s_size > 0:
                                exec_vm.append((s_addr, s_addr + s_size))
                elif cmd == LC_DATA_IN_CODE and cmdsize >= 16:
                    doff, dsize = struct.unpack_from('<II', full_hdr, off + 8)
                    if dsize >= 8:
                        dice_cmd = (doff, dsize)
                off += cmdsize

            dice_vm = []
            if dice_cmd is not None and le_vmaddr is not None and le_fileoff is not None:
                doff, dsize = dice_cmd
                if doff >= le_fileoff:
                    dice_va = le_vmaddr + (doff - le_fileoff)
                    dbuf = self.read_vm(dice_va, dsize)
                    if dbuf and len(dbuf) == dsize:
                        for j in range(0, dsize - (dsize % 8), 8):
                            e_off, e_len, _ = struct.unpack_from('<IHH', dbuf, j)
                            if e_len:
                                dice_vm.append((text_vmaddr + e_off, text_vmaddr + e_off + e_len))

            for cs, ce in subtract_intervals(exec_vm, dice_vm):
                for fidx, foff, flen in self.vm_to_exec_file_ranges(cs, ce):
                    by_file[fidx].append((foff, flen))

        # Merge adjacent/overlapping intervals per file (e.g. if branch pool / imagesText overlapped)
        for fidx, rlist in by_file.items():
            merged = merge_intervals((fo, fo + fl) for fo, fl in rlist)
            by_file[fidx] = [(s, e - s) for s, e in merged]

        return by_file, mh_patches


def open_rw_mmap(path):
    """Open `path` read-write for in-place mmap, temporarily adding u+w if mode is 0555/0444."""
    orig_mode = None
    try:
        f = open(path, 'r+b')
    except PermissionError:
        st_mode = stat.S_IMODE(os.lstat(path).st_mode)
        os.chmod(path, st_mode | stat.S_IWUSR)
        orig_mode = st_mode
        f = open(path, 'r+b')
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_WRITE)
    return f, mm, orig_mode


def close_rw_mmap(path, f, mm, orig_mode):
    try:
        mm.flush()
    finally:
        mm.close()
        f.close()
        if orig_mode is not None:
            os.chmod(path, orig_mode)


def process_macho_file(path, fast_lib=None, zero_pacga=False, set_subtype=False, dry_run=False):
    """Scan (and if needed, rewrite in-place) a thin or universal Mach-O file."""
    with open(path, 'rb') as rf:
        fsize = os.fstat(rf.fileno()).st_size
        if fsize < 32:
            return None
        with mmap.mmap(rf.fileno(), 0, access=mmap.ACCESS_COPY) as ro_mm:
            slices = parse_standalone_file(ro_mm)
            if not slices:
                return None
            all_ranges = [r for sl in slices for r in sl['ranges']]
            subtype_patches = [
                sl for sl in slices if set_subtype and (sl['cpusub'] & 0xff) == CPU_SUBTYPE_ARM64E
            ]
            rewritten, stats = rewrite_ranges(
                ro_mm, all_ranges, fast_lib=fast_lib, zero_pacga=zero_pacga, dry_run=True
            )

    if dry_run or (rewritten == 0 and not subtype_patches):
        return {
            'kind': 'macho',
            'files': [os.path.abspath(path)],
            'rewritten': rewritten,
            'subtype_patched': len(subtype_patches),
            'stats': stats,
        }

    f, mm, orig_mode = open_rw_mmap(path)
    try:
        rewritten, stats = rewrite_ranges(
            mm, all_ranges, fast_lib=fast_lib, zero_pacga=zero_pacga, dry_run=False
        )
        for sl in subtype_patches:
            struct.pack_into('<I', mm, sl['mh_off'] + 8, CPU_SUBTYPE_ARM64_ALL)
            if 'fat_cpusub_off' in sl:
                struct.pack_into('>I', mm, sl['fat_cpusub_off'], CPU_SUBTYPE_ARM64_ALL)
    finally:
        close_rw_mmap(path, f, mm, orig_mode)

    return {
        'kind': 'macho',
        'files': [os.path.abspath(path)],
        'rewritten': rewritten,
        'subtype_patched': len(subtype_patches),
        'stats': stats,
    }


def resolve_dsc_main_and_filter(path):
    """If `path` is a subcache (e.g. .01) whose main cache exists alongside it, return (main_path, only_path)."""
    apath = os.path.abspath(path)
    with open(apath, 'rb') as f:
        hdr = f.read(0x200)
    if len(hdr) < 0x20 or hdr[:8] != b'dyld_v1 ':
        return None, None
    map_off, _ = struct.unpack_from('<II', hdr, 0x10)
    img_cnt = struct.unpack_from('<I', hdr, 0x1c4)[0] if map_off >= 0x1c8 else 0
    img_cnt_old = struct.unpack_from('<I', hdr, 0x1c)[0]
    it_cnt = struct.unpack_from('<Q', hdr, 0x90)[0] if map_off >= 0x98 else 0
    if img_cnt or img_cnt_old or it_cnt:
        return apath, None

    m = SUBCACHE_RE.match(os.path.basename(apath))
    if m:
        candidate = os.path.join(os.path.dirname(apath), m.group(1))
        if os.path.isfile(candidate):
            return os.path.abspath(candidate), apath
    return apath, None


def process_dyld_cache(path, fast_lib=None, zero_pacga=False, set_subtype=False, dry_run=False):
    """Rewrite a dyld_shared_cache_arm64e (and all present subcaches, or a single subcache if targeted)."""
    main_path, only_file = resolve_dsc_main_and_filter(path)
    if main_path is None:
        return None

    dsc = DyldCacheSet(main_path)
    try:
        by_file, mh_patches = dsc.collect_exec_ranges_by_file()
        files = list(dsc.files)
    finally:
        dsc.close()

    patches_by_file = {i: [] for i in range(len(files))}
    if set_subtype:
        for fidx, foff, cpusub in mh_patches:
            if (cpusub & 0xff) == CPU_SUBTYPE_ARM64E:
                patches_by_file[fidx].append(foff)

    total_rewritten = 0
    total_subtype = 0
    total_stats = [0] * 8
    touched_files = []

    for fidx, fpath in enumerate(files):
        if only_file is not None and fpath != only_file:
            continue
        touched_files.append(fpath)
        ranges = by_file.get(fidx, [])
        spatches = patches_by_file.get(fidx, [])
        if not ranges and not spatches:
            continue

        if dry_run:
            with open(fpath, 'rb') as rf:
                with mmap.mmap(rf.fileno(), 0, access=mmap.ACCESS_COPY) as ro_mm:
                    rw, st = rewrite_ranges(
                        ro_mm, ranges, fast_lib=fast_lib, zero_pacga=zero_pacga, dry_run=True
                    )
        else:
            f, mm, orig_mode = open_rw_mmap(fpath)
            try:
                rw, st = rewrite_ranges(
                    mm, ranges, fast_lib=fast_lib, zero_pacga=zero_pacga, dry_run=False
                )
                for mh_foff in spatches:
                    struct.pack_into('<I', mm, mh_foff + 8, CPU_SUBTYPE_ARM64_ALL)
            finally:
                close_rw_mmap(fpath, f, mm, orig_mode)

        total_rewritten += rw
        total_subtype += len(spatches)
        for i in range(8):
            total_stats[i] += st[i]

    return {
        'kind': 'dyld_shared_cache',
        'files': touched_files,
        'rewritten': total_rewritten,
        'subtype_patched': total_subtype,
        'stats': total_stats,
    }


def quick_classify(path):
    """Inspect the first 16 bytes of a regular file without reading non-Mach-O assets."""
    try:
        st = os.lstat(path)
        if not stat.S_ISREG(st.st_mode) or st.st_size < 32:
            return None, st
        with open(path, 'rb') as f:
            hdr = f.read(16)
    except OSError:
        return None, None
    if len(hdr) < 16:
        return None, st
    if hdr[:8] == b'dyld_v1 ' and b'arm64' in hdr:
        return 'dsc', st
    magic_le, = struct.unpack_from('<I', hdr, 0)
    magic_be, = struct.unpack_from('>I', hdr, 0)
    if magic_le == MH_MAGIC_64 or magic_be in (FAT_MAGIC, FAT_MAGIC_64):
        return 'macho', st
    return None, st


def iter_candidate_files(inputs, roots):
    """Yield (kind, path) skipping symlinks and deduplicating hardlinks + subcaches."""
    seen_inodes = set()
    candidates = []

    def consider_file(p, explicit=False):
        if os.path.islink(p):
            return
        kind, st = quick_classify(p)
        if kind is None:
            if explicit:
                print(f'warning: skipping unsupported or non-arm64 file: {p}', file=sys.stderr)
            return
        if st is not None and st.st_nlink > 1 and st.st_ino != 0:
            key = (st.st_dev, st.st_ino)
            if key in seen_inodes:
                return
            seen_inodes.add(key)
        candidates.append((kind, os.path.abspath(p), explicit))

    for p in inputs:
        if os.path.islink(p):
            continue
        if os.path.isdir(p):
            roots.append(p)
        else:
            consider_file(p, explicit=True)

    for root_dir in roots:
        for dirpath, dirnames, filenames in os.walk(root_dir, followlinks=False):
            dirnames[:] = [d for d in sorted(dirnames) if not os.path.islink(os.path.join(dirpath, d))]
            for fn in sorted(filenames):
                consider_file(os.path.join(dirpath, fn), explicit=False)

    # Process main dyld_shared_cache files before any subcaches so split subcaches aren't scanned twice
    dsc_Order = sorted(
        [c for c in candidates if c[0] == 'dsc'],
        key=lambda c: (bool(SUBCACHE_RE.match(os.path.basename(c[1]))), c[1]),
    )
    machos = [c for c in candidates if c[0] == 'macho']
    return dsc_Order + machos


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Offline ARMv8.3-A arm64e -> ARMv8.0-A executable PAC opcode rewriter.'
    )
    ap.add_argument('paths', nargs='*', help='Mach-O files, dyld_shared_cache_arm64e, or directories')
    ap.add_argument('--root', action='append', default=[], metavar='DIR',
                    help='recursively rewrite all Mach-Os and dyld_shared_cache_arm64e* under DIR (skips symlinks)')
    ap.add_argument('-n', '--dry-run', action='store_true',
                    help='scan and report opcode counts without modifying files')
    ap.add_argument('--zero-pacga', action='store_true',
                    help='rewrite pacga Xd, Xn, Xm -> mov Xd, xzr (default: leave untouched for XNU sleh)')
    ap.add_argument('--set-subtype-arm64', action='store_true',
                    help='rewrite CPU_SUBTYPE_ARM64E header field to CPU_SUBTYPE_ARM64_ALL (default: off)')
    ap.add_argument('--pure-python', action='store_true',
                    help='disable C shared-library acceleration and use pure Python')
    ap.add_argument('--json', action='store_true', help='print machine-readable JSON summary')
    ap.add_argument('-v', '--verbose', action='store_true', help='print per-file rewrite counts')
    args = ap.parse_args(argv)

    if not args.paths and not args.root:
        ap.error('at least one path or --root DIR is required')

    t0 = time.monotonic()
    fast_lib = load_fast_lib(force_pure_python=args.pure_python)
    handled_dsc_files = set()

    totals = {
        'engine': 'c' if fast_lib is not None else 'python',
        'dry_run': args.dry_run,
        'macho_files': 0,
        'dsc_sets': 0,
        'dsc_files': 0,
        'rewritten': 0,
        'subtype_patched': 0,
        'stats': dict.fromkeys(STAT_NAMES, 0),
    }

    for kind, path, explicit in iter_candidate_files(args.paths, list(args.root)):
        if kind == 'dsc':
            if path in handled_dsc_files and not explicit:
                continue
            res = process_dyld_cache(
                path, fast_lib=fast_lib, zero_pacga=args.zero_pacga,
                set_subtype=args.set_subtype_arm64, dry_run=args.dry_run
            )
            if res is None:
                continue
            for fp in res['files']:
                handled_dsc_files.add(fp)
            totals['dsc_sets'] += 1
            totals['dsc_files'] += len(res['files'])
        else:
            res = process_macho_file(
                path, fast_lib=fast_lib, zero_pacga=args.zero_pacga,
                set_subtype=args.set_subtype_arm64, dry_run=args.dry_run
            )
            if res is None:
                continue
            totals['macho_files'] += 1

        totals['rewritten'] += res['rewritten']
        totals['subtype_patched'] += res['subtype_patched']
        for name, val in zip(STAT_NAMES, res['stats']):
            totals['stats'][name] += val

        if args.verbose and (res['rewritten'] or res['subtype_patched'] or explicit):
            print(f"{res['kind']}: {path} -> rewrote {res['rewritten']} opcodes", file=sys.stderr)

    totals['elapsed_sec'] = round(time.monotonic() - t0, 3)

    if args.json:
        print(json.dumps(totals, indent=2))
    else:
        st = totals['stats']
        mode = 'DRY-RUN' if args.dry_run else 'REWRITTEN'
        print(
            f"[{mode}] engine={totals['engine']} elapsed={totals['elapsed_sec']:.3f}s "
            f"macho={totals['macho_files']} dsc_sets={totals['dsc_sets']} ({totals['dsc_files']} files) "
            f"rewrote={totals['rewritten']} "
            f"(reta={st['reta']} bra={st['bra']} blra={st['blra']} "
            f"pac_aut_xpac={st['pac_aut_xpac']} ldra={st['ldra']} pacga_zeroed={st['pacga_zeroed']}) "
            f"sleh_untouched=(ldra_neg_wb={st['ldra_sleh']} pacga={st['pacga_sleh']})"
            + (f" subtype_patched={totals['subtype_patched']}" if totals['subtype_patched'] else '')
        )
    return 0


if __name__ == '__main__':
    sys.exit(main())
