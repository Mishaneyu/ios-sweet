#!/usr/bin/env python3
# Link bgmtl-env.dylib into the staged MercuryPosterExtension (already linking bgmtl-inject) and re-sign it.
# usage: add_env.py RENEGADE_DIR STAGE_ROOT ENV_DYLIB
import os, shutil, subprocess, sys
ren, stage, envlib = sys.argv[1:4]
sys.path.insert(0, ren)
import root_tweaks18 as rt
import pacstrip
ENV = '/usr/local/lib/bgmtl/bgmtl-env.dylib'
binp = os.path.join(stage, rt.MERCURY_BIN)
ents, ident = rt.entitlements_of(binp), pacstrip.cs_identity(binp)
print('before: inject', rt.macho_has_dylib(binp, rt.BGMTL_INJECT), 'env', rt.macho_has_dylib(binp, ENV), 'ident', ident, 'ents', len(ents or {}))
if not rt.macho_has_dylib(binp, ENV):
    subprocess.run([sys.executable, '-I', rt.ADDLC, binp, ENV], check=True)
    pacstrip.sign(binp)
n_ents, n_ident = rt.entitlements_of(binp), pacstrip.cs_identity(binp)
assert n_ents == ents and n_ident[:2] == ident[:2], 'identity or entitlements changed'
print('after: inject', rt.macho_has_dylib(binp, rt.BGMTL_INJECT), 'env', rt.macho_has_dylib(binp, ENV), 'ident', n_ident, 'ents', len(n_ents or {}))
dst = os.path.join(stage, ENV.lstrip('/'))
shutil.copyfile(envlib, dst)
os.chmod(dst, 0o755)
open(os.path.join(stage, 'private/var/tmp/bgmtl.defer'), 'w').write('BGMTL_DEFER_SCHEDULED=1 for posters (bgmtl-env.dylib)\n')
