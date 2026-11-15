"""Idempotently inject the UniXNU build hooks into a Project Aloha tree.

`patches/aloha-guacamole.patch` carries two hunks whose context moved between
Aloha releases (the marker comment changed from "## aloha patch" to
"## woa-msmnile patch", and SurfaceDuo1NoSb.dsc lost the CranePkg lines that
anchored the UniXNULib hunk). Without them git apply aborts the whole patch and
the build never starts. Those two edits are pure *additions* though, so they
can be re-done here without depending on any context.

usage: python3 fix_aloha_tree.py <uefi_tree_root>
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else "uefi")


def report(path, changed, what):
    print(("  patched  " if changed else "  ok       ") + what + "  [" + str(path) + "]")


# --------------------------------------------------------------------------
# 1. PlatformBuildNoSb.py - add the UniXNU / DarwinBoot package search paths
# --------------------------------------------------------------------------
py = root / "Platforms/SurfaceDuo1Pkg/PlatformBuildNoSb.py"
if py.exists():
    text = py.read_text(encoding="utf-8")
    need = ["../build-tree/package", "../build-tree/drivers/DarwinBoot"]
    missing = [n for n in need if n not in text]
    if missing:
        # Anchor on the Silicon/QC entry, which every revision has.
        m = re.search(r'^[ \t]*"Silicon/QC/"\s*\+\s*SiliconName[ \t]*,?[ \t]*$',
                      text, re.M)
        if m is None:
            sys.exit("FATAL: cannot find the Silicon/QC entry in %s" % py)
        indent = re.match(r"^[ \t]*", m.group(0)).group(0)
        line = m.group(0).rstrip()
        if not line.endswith(","):
            line += ","
        add = "\n".join(indent + '"%s",' % n for n in missing)
        text = text[:m.start()] + line + "\n" + add + text[m.end():]
        py.write_text(text, encoding="utf-8")
    report(py, bool(missing), "package search paths " + ",".join(need))
else:
    print("  skip      PlatformBuildNoSb.py not present")

# --------------------------------------------------------------------------
# 1c. LibUfdt - give it the headers and the strtoul it assumes
# --------------------------------------------------------------------------
# QcomModulePkg/Library/LibUfdt/sysdeps/libufdt_sysdeps_vendor.c calls
# AllocatePool/FreePool/strtoul with no declaration in scope; clang makes an
# implicit declaration an error, so the DXE build dies on this single file.
# Later revisions fixed it by including DebugLib/MemoryAllocationLib and by
# providing a local strtoul built on BaseLib. Replacing the whole file is too
# invasive (the newer one also redefines strcmp, which clashes with this
# revision's string.h), so only those two things are added here.
# Deliberately self-contained: the library's INF lists MemoryAllocationLib
# (the file already calls AllocatePool) but not necessarily BaseLib, so a
# strtoul built on AsciiStrDecimalToUintnS would compile and then fail to link.
STRToul = """
#ifdef strtoul
#undef strtoul
#endif

static unsigned long dto_local_strtoul(const char *nptr, char **endptr, int base)
{
  unsigned long value = 0;
  const char *p = nptr;

  while (*p == ' ' || *p == '\\t') {
    p++;
  }
  if (base == 0) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0') { base = 8; }
    else { base = 10; }
  }
  for (; *p != '\\0'; p++) {
    int digit;
    if (*p >= '0' && *p <= '9')       { digit = *p - '0'; }
    else if (*p >= 'a' && *p <= 'f')  { digit = *p - 'a' + 10; }
    else if (*p >= 'A' && *p <= 'F')  { digit = *p - 'A' + 10; }
    else { break; }
    if (digit >= base) { break; }
    value = value * (unsigned long)base + (unsigned long)digit;
  }
  if (endptr != NULL) {
    *endptr = (char *)p;
  }
  return value;
}
"""

ufdt = root / "Platforms/QcomModulePkg/Library/LibUfdt/sysdeps/libufdt_sysdeps_vendor.c"
if ufdt.is_file():
    text = ufdt.read_text(encoding="utf-8")
    changed = False
    if "<Library/MemoryAllocationLib.h>" not in text:
        text = text.replace('#include "libufdt_sysdeps.h"',
                            '#include "libufdt_sysdeps.h"\n'
                            '#include <Library/MemoryAllocationLib.h>', 1)
        changed = True
    if "dto_local_strtoul" not in text:
        anchor = "unsigned long int dto_strtoul"
        if anchor in text:
            text = text.replace(anchor, STRToul.strip() + "\n\n" + anchor, 1)
            text = text.replace("return strtoul(nptr, endptr, base);",
                                "return dto_local_strtoul(nptr, endptr, base);")
            changed = True
        else:
            print("  WARN     dto_strtoul anchor missing; cannot add strtoul")
    if changed:
        ufdt.write_text(text, encoding="utf-8")
    print(("  patched  " if changed else "  ok       ") + "LibUfdt sysdeps (headers + strtoul)")
else:
    print("  skip     LibUfdt sysdeps not present")

# --------------------------------------------------------------------------
# 1d. MdePkg.dec - declare gFdtTableGuid if this mu_basecore predates it
# --------------------------------------------------------------------------
# UniXNULib.inf consumes gFdtTableGuid and only pulls in MdePkg.dec. Newer
# mu_basecore revisions added that GUID to MdePkg.dec; the one Aloha 2412.74
# pins does not have it, so every UniXNU module fails with
# "Value of Guid [gFdtTableGuid] is not found under [Guids] section".
GUID_LINE = "  gFdtTableGuid = { 0xb1b621d5, 0xf19c, 0x41a5, " \
            "{ 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }"

dec = root / "MU_BASECORE/MdePkg/MdePkg.dec"
if dec.is_file():
    text = dec.read_text(encoding="utf-8")
    if "gFdtTableGuid" not in text:
        m = re.search(r"^\[Guids[^\]]*\][ \t]*$", text, re.M)
        if m is None:
            print("  WARN     no [Guids] section in %s" % dec)
        else:
            dec.write_text(text[:m.end()] + "\n" + GUID_LINE + text[m.end():],
                           encoding="utf-8")
            print("  patched  MdePkg.dec (gFdtTableGuid added)")
    else:
        print("  ok       MdePkg.dec already declares gFdtTableGuid")
else:
    print("  skip     MdePkg.dec not present")

# --------------------------------------------------------------------------
# 1e. MdePkg/Include/Guid/Fdt.h - the header that declares the GUID above
# --------------------------------------------------------------------------
# Declaring the GUID in MdePkg.dec is not enough: UniXNULib/Boot.c does
# #include <Guid/Fdt.h>, and that header only exists in the newer mu_basecore
# revisions, so the compile died with "'Guid/Fdt.h' file not found".
FDT_H = """/** @file
  GUID for the Flattened Device Tree (FDT) table.

  The mu_basecore revision pinned by some Aloha releases predates this header,
  while UniXNULib includes it unconditionally.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __FDT_GUID_H__
#define __FDT_GUID_H__

#define EFI_FDT_GUID \\
  { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }

extern EFI_GUID gFdtTableGuid;

#endif
"""

fdt_h = root / "MU_BASECORE/MdePkg/Include/Guid/Fdt.h"
if dec.is_file():
    if fdt_h.is_file():
        print("  ok       MdePkg/Include/Guid/Fdt.h already present")
    else:
        fdt_h.parent.mkdir(parents=True, exist_ok=True)
        fdt_h.write_text(FDT_H, encoding="utf-8")
        print("  patched  MdePkg/Include/Guid/Fdt.h added")
else:
    print("  skip     MdePkg not present")

# --------------------------------------------------------------------------
# 1f. BaseFdtLib - upgrade to the revision that exports the whole API
# --------------------------------------------------------------------------
# UniXNU's ShengBootMemoryDxe calls FdtTotalSize/FdtPathOffset/FdtGetProp.
# The mu_basecore that Aloha 2412.74 pins stops at FdtGetProperty, so the DXE
# links with "undefined symbol: FdtTotalSize". Both revisions lay the sources
# out identically (libfdt/libfdt + the same two .c files), so swap them in
# place and leave the DSC mapping pointing at the same INF path.
fixes = Path(__file__).resolve().parent / "fixes"
fdt_lib_h = root / "MU_BASECORE/MdePkg/Include/Library/FdtLib.h"
if fdt_lib_h.is_file():
    if "FdtTotalSize" in fdt_lib_h.read_text(encoding="utf-8"):
        print("  ok       BaseFdtLib already exports the full Fdt* API")
    else:
        src = fixes / "FdtLib.h"
        if src.is_file():
            fdt_lib_h.write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
            libdir = root / "MU_BASECORE/MdePkg/Library/BaseFdtLib"
            for name in ("BaseFdtLib.inf", "FdtLib.c", "LibFdtWrapper.c",
                         "LibFdtSupport.h"):
                s = fixes / "BaseFdtLib" / name
                if s.is_file() and libdir.is_dir():
                    (libdir / name).write_text(s.read_text(encoding="utf-8"),
                                               encoding="utf-8")
            print("  patched  BaseFdtLib upgraded (full Fdt* API)")
        else:
            print("  WARN     no vendored BaseFdtLib to install")
else:
    print("  skip     MdePkg/Include/Library/FdtLib.h not present")

# --------------------------------------------------------------------------
# 1g. MdePkg/Include/AsmMacroLib.h - the ARM64 assembly macro header
# --------------------------------------------------------------------------
# DarwinBoot's Entry.S does #include <AsmMacroLib.h>. In this mu_basecore the
# file lives under MdePkg/Include/AArch64/ (or is missing outright), so the
# bare include cannot resolve and the assembly step dies. Drop a copy at the
# Include root, where it is on every module's include path.
asm_macro = fixes / "AsmMacroLib.h"
inc = root / "MU_BASECORE/MdePkg/Include"
if asm_macro.is_file() and inc.is_dir():
    if (inc / "AsmMacroLib.h").is_file():
        print("  ok       MdePkg/Include/AsmMacroLib.h already present")
    else:
        (inc / "AsmMacroLib.h").write_text(
            asm_macro.read_text(encoding="utf-8"), encoding="utf-8")
        (inc / "AArch64").mkdir(exist_ok=True)
        if not (inc / "AArch64/AsmMacroLib.h").is_file():
            (inc / "AArch64/AsmMacroLib.h").write_text(
                asm_macro.read_text(encoding="utf-8"), encoding="utf-8")
        print("  patched  MdePkg/Include/AsmMacroLib.h added")
else:
    print("  skip     AsmMacroLib header not applicable")

# --------------------------------------------------------------------------
# 2. SurfaceDuo1NoSb.dsc - add the FdtLib / UniXNULib library class mappings
# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
# 1b. board.toml - make sure the Mu package trees are on the packages path
# --------------------------------------------------------------------------
# board.toml's [firmware] packages list was written for a revision where
# ArmPkg still lived inside mu_basecore. Newer Mu trees moved it (and more)
# into the mu_tiano_plus / mu_silicon_arm_tiano submodules, so the DSC fails
# with "ArmPkg/Drivers/ArmGic/ArmGicDxe.inf is not found in packages path".
board = Path("board.toml")
if board.is_file():
    text = board.read_text(encoding="utf-8")
    m = re.search(r"^packages\s*=\s*\[(.*?)^\]", text, re.S | re.M)
    if m:
        have = re.findall(r'"([^"]+)"', m.group(1))
        add = ['{uefi}/%s' % p for p in ("Common/MU_TIANO", "Silicon/Arm/MU_TIANO")
               if (root / p).is_dir() and "{uefi}/%s" % p not in have]
        if add:
            body = "\n".join('  "%s",' % e for e in have + add)
            text = text[:m.start()] + "packages = [\n" + body + "\n]" + text[m.end():]
            board.write_text(text, encoding="utf-8")
        print("  added to packages: %s" % (", ".join(add) or "(nothing)"))

dsc = root / "Platforms/SurfaceDuo1Pkg/SurfaceDuo1NoSb.dsc"
if dsc.exists():
    text = dsc.read_text(encoding="utf-8")
    libs = [
        ("FdtLib", "MdePkg/Library/BaseFdtLib/BaseFdtLib.inf"),
        ("UniXNULib", "UniXNUPkg/Library/UniXNULib/UniXNULib.inf"),
    ]
    missing = [(k, v) for k, v in libs if (k + "|") not in text]
    if missing:
        m = re.search(r"^\[LibraryClasses\.common\][ \t]*$", text, re.M)
        if m is None:
            sys.exit("FATAL: cannot find [LibraryClasses.common] in %s" % dsc)
        add = "\n".join("  %s|%s" % (k, v) for k, v in missing)
        text = text[:m.end()] + "\n" + add + text[m.end():]
        dsc.write_text(text, encoding="utf-8")
    report(dsc, bool(missing), "library mappings " + ",".join(k for k, _ in missing))
else:
    print("  skip      SurfaceDuo1NoSb.dsc not present")
