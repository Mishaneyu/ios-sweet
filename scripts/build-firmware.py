#!/usr/bin/env python3
"""Build the Aloha NOSB firmware volume with the UniXNU/DarwinBoot INFs for raphael.

The Project Aloha tree already ships a first-class ``xiaomi-raphael`` device
directory (Device/xiaomi-raphael with its own DeviceTreeBlob), so unlike
guacamole we do NOT have to clone another vendor's device dir. The DSC selects
it purely via ``TARGET_DEVICE=xiaomi-raphael`` and PlatformBuildNoSb derives
``FDT=raphael.dtb`` from it, both of which board.toml already sets.

The aloha-guacamole.patch creates ``Resources/Configs/guacamole.toml``; we
mirror it to ``raphael.toml`` because board.toml sets ``[firmware] config``
to "raphael".
"""
import os
import shutil
import sys
import tomllib
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unixnu" / "scripts"))
sys.path.insert(0, str(ROOT / "scripts"))

from unixnu import firmware  # noqa: E402
from tree import assemble  # noqa: E402

UEFI = ROOT / "uefi"
EDK2 = UEFI / "MU_BASECORE"
WORK = ROOT / "out" / "raphael"
TREE = assemble(WORK, ROOT / "unixnu", ROOT)
LOGS = WORK / "logs"
LOGS.mkdir(parents=True, exist_ok=True)

DEVICE_ROOT = UEFI / "Platforms" / "SurfaceDuo1Pkg" / "Device"
DST = DEVICE_ROOT / "xiaomi-raphael"
SRC = DEVICE_ROOT / "oneplus-guacamole"

# xiaomi-raphael is shipped by the Aloha tree; only synthesise it as a fallback
# (same SM8150 silicon) if a future revision drops it.
if not DST.is_dir():
    if not SRC.is_dir():
        raise SystemExit(
            f"firmware: neither {DST} nor {SRC} exists in the Aloha tree; "
            "cannot build the raphael device."
        )
    print(f"firmware: {DST.name} missing; cloning {SRC.name} (same SM8150 silicon)")
    shutil.copytree(SRC, DST)
    for old, new in (("guacamole.dtb", "raphael.dtb"), ("guacamole.toml", "raphael.toml")):
        p = DST / old
        if p.is_file():
            p.rename(DST / new)
else:
    print(f"firmware: using native Aloha device dir {DST.name}")

# The UniXNU firmware driver reads Resources/Configs/<config>.toml; the SM8150
# patch only provides the guacamole one.
cfg_src = UEFI / "Resources" / "Configs" / "guacamole.toml"
cfg_dst = UEFI / "Resources" / "Configs" / "raphael.toml"
if cfg_src.is_file() and not cfg_dst.is_file():
    shutil.copy(cfg_src, cfg_dst)
    print(f"firmware: created {cfg_dst.name}")

board = tomllib.loads((ROOT / "board.toml").read_text())
ctx = types.SimpleNamespace(
    board=board,
    tree=TREE,
    work=WORK,
    logs=LOGS,
    jobs=8,
    args=types.SimpleNamespace(skip_preflight=False),
    uefi=UEFI,
)
# DEBUG builds enable EDK II ASSERTs; a driver that trips one dead-loops
# (black screen until the bootloader watchdog resets the phone).
TARGET = os.environ.get("FW_TARGET", "DEBUG")
firmware.build(ctx, None, UEFI, EDK2, TARGET)
