#!/usr/bin/env python3
# Stage backboardd for Chlorophyll: LC_LOAD_DYLIB bgmtl-env.dylib FIRST, then bgmtl-inject.dylib, re-sign ad hoc keeping
# identity and entitlements. usage: stage_bb.py RENEGADE_DIR STAGE_ROOT
import os, shutil, subprocess, sys
ren, stage = sys.argv[1:3]
sys.path.insert(0, ren)
import root_tweaks18 as rt
import pacstrip
ENV = '/usr/local/lib/bgmtl/bgmtl-env.dylib'
binp = os.path.join(stage, 'usr/libexec/backboardd')
shutil.copyfile(os.path.join(stage, 'orig', 'backboardd'), binp)
os.chmod(binp, 0o755)
ents, ident = rt.entitlements_of(binp), pacstrip.cs_identity(binp)
print('before: env', rt.macho_has_dylib(binp, ENV), 'inject', rt.macho_has_dylib(binp, rt.BGMTL_INJECT), 'ident', ident, 'ents', len(ents or {}))
for lib in (ENV, rt.BGMTL_INJECT):
    subprocess.run([sys.executable, '-I', rt.ADDLC, binp, lib], check=True)
pacstrip.sign(binp)
n_ents, n_ident = rt.entitlements_of(binp), pacstrip.cs_identity(binp)
assert n_ents == ents and n_ident[:2] == ident[:2], 'identity or entitlements changed'
print('after: env', rt.macho_has_dylib(binp, ENV), 'inject', rt.macho_has_dylib(binp, rt.BGMTL_INJECT), 'ident', n_ident, 'ents', len(n_ents or {}))
# load order check: the env dylib must come before the inject
import struct
b = open(binp, 'rb').read()
ncmds = struct.unpack_from('<I', b, 16)[0]
off, order = 32, []
for _ in range(ncmds):
    c, sz = struct.unpack_from('<II', b, off)
    if c in (0xc, 0x80000018):
        o = struct.unpack_from('<I', b, off + 8)[0]
        order.append(b[off + o:off + sz].split(b'\0')[0].decode())
    off += sz
print('dylib load order (last 4):', order[-4:])
assert order.index(ENV) < order.index(rt.BGMTL_INJECT)
