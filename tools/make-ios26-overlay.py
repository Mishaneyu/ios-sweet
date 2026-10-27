#!/usr/bin/env python3
"""Build a POSIX ustar root overlay (work/boot/ios26-overlay.tar) for iOS 26 bring-up.

Reads /System/Library/xpc/launchd.plist strictly read-only from an HFS+/HFSX
root image (default /home/paink/ios26-root-rmx1931.img) or an extracted plist,
strips crash-looping / hardware-absent LaunchDaemons via sweet-root-tweaks.py,
configures dropbear/wland/sweetstored, and optionally stages de-PAC'd DSC stub
subcaches, SweetMetal2.bundle, and iOS 26 userland binaries/apps/drivers into a
deterministic POSIX ustar archive for mkpayload.py --overlay.
"""
import argparse
import importlib.util
import io
import os
import plistlib
import stat
import struct
import sys
import tarfile

KEEP_DAEMON_KEYS = (
    '/System/Library/LaunchDaemons/com.apple.lsd.plist',
    '/System/Library/LaunchDaemons/com.apple.mobile.installd.plist',
    '/System/Library/LaunchDaemons/com.apple.installcoordinationd.plist',
    '/System/Library/LaunchDaemons/com.apple.mobileactivationd.plist',
    '/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist',
    '/System/Library/LaunchDaemons/org.iosonpixel.wland.plist',
    '/System/Library/LaunchDaemons/org.iosonpixel.sweetstored.plist',
)

SKIP_USERLAND_BINS = {'m2vtest'}


def _load_tweaks_module():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location('sweet_root_tweaks', os.path.join(here, 'sweet-root-tweaks.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class HFSReadOnly:
    """Minimal read-only HFS+/HFSX catalog + data fork reader (never writes or mounts)."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(1024)
        vh = self.f.read(512)
        sig, _ = struct.unpack_from('>2sH', vh, 0)
        if sig not in (b'H+', b'HX'):
            raise ValueError(f'{path}: unexpected HFS+/HFSX volume signature {sig!r}')
        self.block_size, = struct.unpack_from('>I', vh, 40)
        self.extents_fork = self._parse_fork(vh, 192)
        self.catalog_fork = self._parse_fork(vh, 272)
        hdr0 = self._read_fork_bytes(self.catalog_fork, 0, 512)
        self.cat_node_size, = struct.unpack_from('>H', hdr0, 14 + 18)
        self.cat_first_leaf, = struct.unpack_from('>I', hdr0, 14 + 10)
        ext0 = self._read_fork_bytes(self.extents_fork, 0, 512)
        self.ext_node_size, = struct.unpack_from('>H', ext0, 14 + 18)
        self.ext_first_leaf, = struct.unpack_from('>I', ext0, 14 + 10)

    def close(self):
        self.f.close()

    @staticmethod
    def _parse_fork(buf, off):
        logical_size, _, total_blocks = struct.unpack_from('>QII', buf, off)
        extents = []
        for i in range(8):
            sb, bc = struct.unpack_from('>II', buf, off + 16 + i * 8)
            if bc:
                extents.append((sb, bc))
        return (logical_size, total_blocks, extents)

    def _overflow_extents(self, cnid, fork_type, start_block):
        out = []
        node_num = self.ext_first_leaf
        ns = self.ext_node_size
        while node_num != 0:
            node = self._read_fork_bytes(self.extents_fork, node_num * ns, ns)
            flink, _, kind, _, num_recs = struct.unpack_from('>IIbbH', node, 0)
            assert kind == -1
            for r in range(num_recs):
                rec_off, = struct.unpack_from('>H', node, ns - 2 * (r + 1))
                key_len, ftype, _, file_id, sb0 = struct.unpack_from('>HBBII', node, rec_off)
                if file_id == cnid and ftype == fork_type and sb0 >= start_block:
                    val_off = rec_off + 2 + key_len
                    for i in range(8):
                        sb, bc = struct.unpack_from('>II', node, val_off + i * 8)
                        if bc:
                            out.append((sb, bc))
            node_num = flink
        return out

    def _read_fork_bytes(self, fork, offset, size, cnid=None):
        logical_size, total_blocks, extents = fork
        if offset + size > logical_size:
            size = max(0, logical_size - offset)
        have_blocks = sum(bc for _, bc in extents)
        if cnid is not None and have_blocks < total_blocks:
            extents = list(extents) + self._overflow_extents(cnid, 0, have_blocks)
        out = bytearray()
        cur_off = 0
        bs = self.block_size
        for sb, bc in extents:
            ext_len = bc * bs
            if offset < cur_off + ext_len and len(out) < size:
                rel = max(0, offset - cur_off)
                take = min(size - len(out), ext_len - rel)
                self.f.seek(sb * bs + rel)
                out += self.f.read(take)
            cur_off += ext_len
            if len(out) >= size:
                break
        if len(out) != size:
            raise IOError(f'short HFS fork read ({len(out)} != {size})')
        return bytes(out)

    def _lookup_child(self, parent_cnid, target_name):
        node_num = self.cat_first_leaf
        ns = self.cat_node_size
        while node_num != 0:
            node = self._read_fork_bytes(self.catalog_fork, node_num * ns, ns)
            flink, _, kind, _, num_recs = struct.unpack_from('>IIbbH', node, 0)
            assert kind == -1
            for r in range(num_recs):
                rec_off, = struct.unpack_from('>H', node, ns - 2 * (r + 1))
                key_len, pid = struct.unpack_from('>HI', node, rec_off)
                if pid == parent_cnid:
                    nchars, = struct.unpack_from('>H', node, rec_off + 6)
                    name = node[rec_off + 8:rec_off + 8 + nchars * 2].decode('utf-16-be', errors='replace')
                    if name == target_name:
                        val_off = rec_off + 2 + key_len
                        if val_off & 1:
                            val_off += 1
                        rec_type, = struct.unpack_from('>h', node, val_off)
                        return rec_type, node[val_off:]
                elif pid > parent_cnid:
                    return None
            node_num = flink
        return None

    def read_file(self, path):
        parts = [p for p in path.strip('/').split('/') if p]
        cnid = 2
        for i, part in enumerate(parts):
            res = self._lookup_child(cnid, part)
            if res is None:
                raise FileNotFoundError(f'{path}: component {part!r} not found under CNID {cnid}')
            rec_type, val = res
            if i < len(parts) - 1:
                if rec_type != 1:
                    raise IsADirectoryError(f'{part}: expected folder (1), got {rec_type}')
                cnid, = struct.unpack_from('>I', val, 8)
            else:
                if rec_type != 2:
                    raise ValueError(f'{part}: expected file (2), got {rec_type}')
                file_cnid, = struct.unpack_from('>I', val, 8)
                uid, gid, _, _, mode = struct.unpack_from('>IIBBH', val, 32)
                data_fork = self._parse_fork(val, 88)
                return self._read_fork_bytes(data_fork, 0, data_fork[0], cnid=file_cnid), uid, gid, mode & 0o7777


def add_ustar_bytes(tf, rel_path, data, mode=0o644, uid=0, gid=0):
    rel_path = rel_path.lstrip('/')
    ti = tarfile.TarInfo(name=rel_path)
    ti.size = len(data)
    ti.mode = mode
    ti.uid = uid
    ti.gid = gid
    ti.uname = 'root'
    ti.gname = 'wheel'
    ti.mtime = 0
    ti.type = tarfile.REGTYPE
    tf.addfile(ti, io.BytesIO(data))


def add_ustar_dir(tf, rel_path, mode=0o755, uid=0, gid=0):
    rel_path = rel_path.strip('/')
    ti = tarfile.TarInfo(name=rel_path)
    ti.size = 0
    ti.mode = mode
    ti.uid = uid
    ti.gid = gid
    ti.uname = 'root'
    ti.gname = 'wheel'
    ti.mtime = 0
    ti.type = tarfile.DIRTYPE
    tf.addfile(ti)


def add_ustar_symlink(tf, rel_path, target):
    rel_path = rel_path.lstrip('/')
    ti = tarfile.TarInfo(name=rel_path)
    ti.type = tarfile.SYMTYPE
    ti.linkname = target
    ti.mode = 0o777
    ti.uid = 0
    ti.gid = 0
    ti.uname = 'root'
    ti.gname = 'wheel'
    ti.mtime = 0
    tf.addfile(ti)


def add_tree(tf, src_dir, dst_prefix, force_exec_names=()):
    dst_prefix = dst_prefix.strip('/')
    add_ustar_dir(tf, dst_prefix, 0o755)
    for cur_root, dirs, files in os.walk(src_dir):
        dirs.sort()
        files.sort()
        rel_sub = os.path.relpath(cur_root, src_dir)
        cur_dst = dst_prefix if rel_sub == '.' else f'{dst_prefix}/{rel_sub.replace(os.sep, "/")}'
        if rel_sub != '.':
            add_ustar_dir(tf, cur_dst, 0o755)
        for fn in files:
            full = os.path.join(cur_root, fn)
            st = os.lstat(full)
            if stat.S_ISLNK(st.st_mode):
                add_ustar_symlink(tf, f'{cur_dst}/{fn}', os.readlink(full))
            elif stat.S_ISREG(st.st_mode):
                is_exec = bool(st.st_mode & 0o111) or fn in force_exec_names
                with open(full, 'rb') as f:
                    add_ustar_bytes(tf, f'{cur_dst}/{fn}', f.read(), mode=0o755 if is_exec else 0o644)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-o', '--output', default='work/boot/ios26-overlay.tar', help='output POSIX ustar archive')
    ap.add_argument('--root-img', default='/home/paink/ios26-root-rmx1931.img',
                    help='read-only HFS+/HFSX rootfs image to extract launchd.plist from')
    ap.add_argument('--launchd-plist', help='optional pre-extracted launchd.plist (overrides --root-img)')
    ap.add_argument('--sweetmetal2', help='optional path to SweetMetal2.bundle to stage under /System/Library/Extensions')
    ap.add_argument('--userland-dir', help='optional extracted userland dir (Applications/, HAL/, bin/, lib/)')
    ap.add_argument('--dsc-stubs-dir', help='optional dir of de-PACd dyld_shared_cache_arm64e.* stub subcaches')
    ap.add_argument('--mtl2-clients', help='optional /private/var/local/mtl2-clients content')
    ap.add_argument('--selftest-bin', help='optional compiled arm64 self-test Mach-O for guarded test boots')
    ap.add_argument('--selftest-script', help='optional bring-up self-test shell script for guarded test boots')
    ap.add_argument('--extra', action='append', default=[],
                    help='extra file SRC:DST[:OCTAL_MODE] to include in the overlay (repeatable)')
    args = ap.parse_args()

    tweaks = _load_tweaks_module()

    if args.launchd_plist:
        with open(args.launchd_plist, 'rb') as f:
            raw_plist = f.read()
        uid, gid, mode = 0, 80, 0o644
    else:
        hfs = HFSReadOnly(args.root_img)
        try:
            raw_plist, uid, gid, mode = hfs.read_file('/System/Library/xpc/launchd.plist')
        finally:
            hfs.close()

    plist = plistlib.loads(raw_plist)
    before_count = len(plist['LaunchDaemons'])
    renamed, removed = tweaks.clean_launchd_plist(plist)

    jobs = plist['LaunchDaemons']
    # Dropbear v2024.86 is built without syslog (-E is invalid; stderr is default).
    db_key = '/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist'
    if db_key in jobs:
        jobs[db_key]['ProgramArguments'] = ['/var/local/bin/dropbearmulti', 'dropbear', '-F', '-R', '-p', '22']
        jobs[db_key]['ProcessType'] = 'Interactive'

    # Give wland Interactive QoS + non-throttled I/O + Nice -10 so MPSS GLINK/rmtfs/TFTP
    # handshakes never starve under Darwin-BG scheduling during boot.
    wl_key = '/System/Library/LaunchDaemons/org.iosonpixel.wland.plist'
    if wl_key in jobs:
        jobs[wl_key]['Nice'] = -10
        jobs[wl_key]['ProcessType'] = 'Interactive'
        jobs[wl_key]['LowPriorityIO'] = False
        jobs[wl_key]['LegacyTimers'] = True

    diag_key = '/System/Library/LaunchDaemons/org.iosonpixel.sweetdiag.plist'
    if diag_key in jobs:
        jobs[diag_key]['ProcessType'] = 'Interactive'

    # Ensure sweetstored root helper is registered for SweetStore.app (127.0.0.1:29170).
    jobs['/System/Library/LaunchDaemons/org.iosonpixel.sweetstored.plist'] = {
        'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
        'KeepAlive': True,
        'Label': 'org.iosonpixel.sweetstored',
        'ProcessType': 'Interactive',
        'ProgramArguments': ['/var/local/bin/sweetstored'],
        'RunAtLoad': True,
        'StandardErrorPath': '/var/local/sweetstored.log',
        'StandardOutPath': '/var/local/sweetstored.log',
        'UserName': 'root',
    }

    if args.selftest_bin or args.selftest_script:
        prog_args = (['/private/var/local/bin/sweet-selftest'] if args.selftest_bin
                     else ['/private/var/local/bin/sh', '/private/var/local/bin/sweet-selftest.sh'])
        jobs['/System/Library/LaunchDaemons/org.iosonpixel.selftest.plist'] = {
            'EnvironmentVariables': {'PATH': '/private/var/local/bin:/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
            'KeepAlive': False,
            'Label': 'org.iosonpixel.selftest',
            'ProcessType': 'Interactive',
            'ProgramArguments': prog_args,
            'RunAtLoad': True,
            'StandardErrorPath': '/var/local/selftest.log',
            'StandardOutPath': '/var/local/selftest.log',
            'UserName': 'root',
        }

    after_count = len(jobs)
    missing_keep = [k for k in KEEP_DAEMON_KEYS if k not in jobs]
    if missing_keep:
        raise RuntimeError(f'refusing to build overlay: required LaunchDaemons missing: {missing_keep}')

    cleaned_bytes = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)
    buddy_bytes = plistlib.dumps({
        'LastSystemVersion': '23F84',
        'SetupDone': True,
        'SetupFinishedAllSteps': True,
        'SetupVersion': 11,
        'daemonizeNeedsToRun': False,
    }, fmt=plistlib.FMT_BINARY)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with tarfile.open(args.output, 'w', format=tarfile.USTAR_FORMAT) as tf:
        add_ustar_bytes(tf, 'System/Library/xpc/launchd.plist', cleaned_bytes, mode=mode, uid=uid, gid=gid)
        add_ustar_bytes(tf, 'private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist',
                        buddy_bytes, mode=0o600, uid=501, gid=501)
        # Ensure all MCM system & user container class base directories exist with right owners
        for cdir, cuid in (
            ('private/var/containers', 0),
            ('private/var/containers/Bundle', 33),
            ('private/var/containers/Data', 0),
            ('private/var/containers/Data/System', 0),
            ('private/var/containers/Shared', 0),
            ('private/var/containers/Shared/SystemGroup', 0),
            ('private/var/containers/Temp', 0),
            ('private/var/mobile/Containers', 501),
            ('private/var/mobile/Containers/Bundle', 501),
            ('private/var/mobile/Containers/Data', 501),
            ('private/var/mobile/Containers/Data/Application', 501),
            ('private/var/mobile/Containers/Data/PluginKitPlugin', 501),
            ('private/var/mobile/Containers/Data/InternalDaemon', 501),
            ('private/var/mobile/Containers/Data/Temp', 501),
            ('private/var/mobile/Containers/Shared', 501),
            ('private/var/mobile/Containers/Shared/AppGroup', 501),
        ):
            add_ustar_dir(tf, cdir, mode=0o755, uid=cuid, gid=cuid)

        if args.dsc_stubs_dir:
            stub_files = sorted(fn for fn in os.listdir(args.dsc_stubs_dir)
                                if fn.startswith('dyld_shared_cache_arm64e.'))
            if not stub_files:
                raise RuntimeError(f'no dyld_shared_cache_arm64e.* files in {args.dsc_stubs_dir}')
            for fn in stub_files:
                src = os.path.join(args.dsc_stubs_dir, fn)
                sys_dst = f'System/Library/Caches/com.apple.dyld/{fn}'
                crypt_dst = f'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/{fn}'
                with open(src, 'rb') as f:
                    add_ustar_bytes(tf, sys_dst, f.read(), mode=0o644)
                add_ustar_symlink(tf, crypt_dst, '/' + sys_dst)

        if args.sweetmetal2:
            for req in ('SweetMetal2', 'libvulkan_freedreno.dylib', 'Info.plist'):
                p = os.path.join(args.sweetmetal2, req)
                if not os.path.isfile(p):
                    raise FileNotFoundError(f'incomplete SweetMetal2.bundle, missing {p}')
            add_tree(tf, args.sweetmetal2, 'System/Library/Extensions/SweetMetal2.bundle',
                     force_exec_names=('SweetMetal2', 'libvulkan_freedreno.dylib'))

        if args.userland_dir:
            app_dir = os.path.join(args.userland_dir, 'Applications', 'SweetStore.app')
            if os.path.isdir(app_dir):
                add_tree(tf, app_dir, 'Applications/SweetStore.app', force_exec_names=('SweetStore',))
            hal_dir = os.path.join(args.userland_dir, 'HAL', 'HakiosAudio.driver')
            if os.path.isdir(hal_dir):
                add_ustar_dir(tf, 'Library/Audio/Plug-Ins/HAL', 0o755)
                add_tree(tf, hal_dir, 'Library/Audio/Plug-Ins/HAL/HakiosAudio.driver',
                         force_exec_names=('HakiosAudio',))
            lib_dir = os.path.join(args.userland_dir, 'lib')
            if os.path.isdir(lib_dir):
                add_ustar_dir(tf, 'usr/local', 0o755)
                add_ustar_dir(tf, 'usr/local/lib', 0o755)
                for fn in sorted(os.listdir(lib_dir)):
                    p = os.path.join(lib_dir, fn)
                    if os.path.isfile(p):
                        with open(p, 'rb') as f:
                            add_ustar_bytes(tf, f'usr/local/lib/{fn}', f.read(), mode=0o755)
            bin_dir = os.path.join(args.userland_dir, 'bin')
            if os.path.isdir(bin_dir):
                add_ustar_dir(tf, 'private/var/local/bin', 0o755)
                for fn in sorted(os.listdir(bin_dir)):
                    if fn in SKIP_USERLAND_BINS:
                        continue
                    p = os.path.join(bin_dir, fn)
                    if os.path.isfile(p):
                        with open(p, 'rb') as f:
                            add_ustar_bytes(tf, f'private/var/local/bin/{fn}', f.read(), mode=0o755)
                if os.path.isfile(os.path.join(bin_dir, 'dropbearmulti')):
                    for link in ('dropbear', 'dbclient', 'dropbearkey', 'scp'):
                        add_ustar_symlink(tf, f'private/var/local/bin/{link}', 'dropbearmulti')

        if args.mtl2_clients is not None:
            add_ustar_bytes(tf, 'private/var/local/mtl2-clients',
                            (args.mtl2_clients.strip() + '\n').encode(), mode=0o644)
        add_ustar_bytes(tf, 'private/var/local/mtl2.log', b'', mode=0o666)
        add_ustar_bytes(
            tf,
            'private/var/local/inject.conf',
            b'/usr/local/lib/libsweet_media.dylib mediaserverd\n'
            b'/usr/local/lib/libsweet_sysfix.dylib containermanagerd\n'
            b'/usr/local/lib/libsweet_sysfix.dylib containermanagerd_system\n'
            b'/usr/local/lib/libsweet_sysfix.dylib SpringBoard\n',
            mode=0o644,
        )

        if args.selftest_bin:
            with open(args.selftest_bin, 'rb') as f:
                add_ustar_bytes(tf, 'private/var/local/bin/sweet-selftest', f.read(), mode=0o755)
            add_ustar_bytes(tf, 'private/var/local/selftest.log', b'', mode=0o666)
            add_ustar_bytes(tf, 'private/var/local/mcm.log', b'', mode=0o666)
        elif args.selftest_script:
            with open(args.selftest_script, 'rb') as f:
                add_ustar_bytes(tf, 'private/var/local/bin/sweet-selftest.sh',
                                f.read().replace(b'\r\n', b'\n'), mode=0o755)
            add_ustar_bytes(tf, 'private/var/local/selftest.log', b'', mode=0o666)
            add_ustar_bytes(tf, 'private/var/local/mcm.log', b'', mode=0o666)
        else:
            add_ustar_bytes(tf, '.sweet-rm',
                            b'private/var/local/bin/sweet-selftest\n'
                            b'private/var/local/bin/sweet-selftest.sh\n'
                            b'private/var/local/selftest.log\n'
                            b'private/var/local/mcm.log\n', mode=0o644)

        for spec in args.extra:
            parts = spec.split(':')
            if len(parts) not in (2, 3):
                raise ValueError(f'invalid --extra {spec!r}, expected SRC:DST[:OCTAL_MODE]')
            src, dst = parts[0], parts[1]
            fmode = int(parts[2], 8) if len(parts) == 3 else (0o755 if (os.stat(src).st_mode & 0o111) else 0o644)
            with open(src, 'rb') as f:
                add_ustar_bytes(tf, dst, f.read(), mode=fmode)

    print(f'launchd.plist: {before_count} -> {after_count} LaunchDaemons '
          f'({renamed} _PanicOnCrash renamed, {len(removed)} stripped)')
    print(f'kept intact: {[os.path.basename(k) for k in KEEP_DAEMON_KEYS]}')
    print(f'wrote {args.output} ({os.path.getsize(args.output)} bytes)')


if __name__ == '__main__':
    main()
