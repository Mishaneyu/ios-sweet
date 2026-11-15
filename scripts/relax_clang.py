"""Relax the clang diagnostics the older Mu trees cannot satisfy.

EDK II reads its compiler flags from Conf/tools_def.txt, which the UniXNU build
driver seeds from BaseTools/Conf/tools_def.template *only when the file is
absent*. So a pre-seeded, edited tools_def.txt survives and is what the build
uses.

The Aloha revisions that still carry the full raphael device assets predate
modern clang, and their code calls AllocatePool/FreePool/strtoul and friends
with no declaration in scope. clang makes an implicit declaration an error
since C99 mode became the default, and -Wno-error does not reach the target
toolchain, so the flags have to go here.

usage: python3 relax_clang.py <tools_def.txt>
"""
import sys
from pathlib import Path

EXTRA = ("-Wno-implicit-function-declaration -Wno-int-conversion "
         "-Wno-incompatible-pointer-types -Wno-return-type "
         "-Wno-implicit-int -Wno-strict-prototypes")

path = Path(sys.argv[1] if len(sys.argv) > 1 else "out/raphael/edk2-conf/tools_def.txt")
lines = path.read_text(encoding="utf-8").splitlines()
out = []
hit = 0
for line in lines:
    if "_CLANGPDB_AARCH64_CC_FLAGS" in line and "=" in line and "-Wno-int-conversion" not in line:
        line = line.rstrip() + " " + EXTRA
        hit += 1
    out.append(line)
path.write_text("\n".join(out) + "\n", encoding="utf-8")
print("relaxed %d CLANGPDB AARCH64 CC flag lines in %s" % (hit, path))
