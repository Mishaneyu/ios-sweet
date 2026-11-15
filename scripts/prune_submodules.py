"""Align board.toml's submodule list with the Aloha revision in use.

board.toml's [firmware] submodules list was written for one revision of the
Project Aloha tree. Other revisions rename or drop submodules (2412.74 has no
Platforms/CranePkg and no MbedTlsPkg/mbedtls) and add ones the board never
listed (2412.74 needs Common/MU_TIANO and Silicon/Arm/MU_TIANO). `git submodule
update --init -- <paths>` aborts on the first path git does not know, and a
missing submodule leaves its package unresolvable, so both directions kill the
build before EDK II starts.

The tree's own .gitmodules is the only trustworthy source, so the list is
replaced by exactly what the checked-out revision declares.

usage: python3 prune_submodules.py <uefi_tree_root> [board.toml]
"""
import re
import sys
from pathlib import Path

uefi = Path(sys.argv[1] if len(sys.argv) > 1 else "uefi")
board = Path(sys.argv[2] if len(sys.argv) > 2 else "board.toml")

gitmodules = uefi / ".gitmodules"
known = sorted(set(re.findall(r"^\s*path\s*=\s*(\S+)\s*$",
                              gitmodules.read_text(encoding="utf-8"), re.M)))
print("submodules declared by the tree: %d" % len(known))

text = board.read_text(encoding="utf-8")
m = re.search(r"^submodules\s*=\s*\[(.*?)^\]", text, re.S | re.M)
if not m:
    print("  no [firmware] submodules block in board.toml - nothing to do")
    sys.exit(0)

entries = re.findall(r'"([^"]+)"', m.group(1))
print("  board.toml asked for        : %s" % ", ".join(entries))
print("  dropped (tree lacks them)   : %s"
      % (", ".join(e for e in entries if e not in known) or "(none)"))
print("  added   (tree needs them)   : %s"
      % (", ".join(e for e in known if e not in entries) or "(none)"))

body = "\n".join('  "%s",' % e for e in known)
text = text[:m.start()] + "submodules = [\n" + body + "\n]" + text[m.end():]
board.write_text(text, encoding="utf-8")
