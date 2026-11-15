#!/usr/bin/env python3
# Apply Renegade's bgmtl() (MercuryPosterExtension + Chlorophyll inject dylibs) to a staging root on the host.
# usage: run_bgmtl.py RENEGADE_DIR STAGE_ROOT DYLIB_DIR
import os, shutil, sys
ren, stage, dylibs = sys.argv[1:4]
os.environ['RENEGADE_BGMTL_DIR'] = dylibs
sys.path.insert(0, ren)
import root_tweaks18 as rt
binp = os.path.join(stage, rt.MERCURY_BIN)
os.makedirs(os.path.dirname(binp), exist_ok=True)
shutil.copyfile(os.path.join(stage, 'orig', 'MercuryPosterExtension'), binp)
os.chmod(binp, 0o755)
print('before: links inject =', rt.macho_has_dylib(binp, rt.BGMTL_INJECT), 'entitlements', len(rt.entitlements_of(binp) or {}))
ok = rt.bgmtl(stage)
print('bgmtl() returned', ok)
print('after: links inject =', rt.macho_has_dylib(binp, rt.BGMTL_INJECT), 'entitlements', len(rt.entitlements_of(binp) or {}))
