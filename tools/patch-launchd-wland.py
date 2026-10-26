#!/usr/bin/env python3
"""Add the Wi-Fi radio daemon to iOS's launchd job cache (G8S).

  patch-launchd-wland.py IN.plist OUT.plist

IN is the root's /System/Library/xpc/launchd.plist (binary; launchd reads a
modified one when booted with launchd_unsecure_cache=1). Adds
org.betalm.wland = `/var/local/bin/wland auto` as root at load (ios-flame's
org.iosonpixel.wland), and drops com.apple.fairplayd.H2, which exits right
after start and was relaunched every 10 s (ios-flame found the same).
OUT goes into the root with mkpayload.py --inject.
"""
import plistlib, sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        plist = plistlib.load(f)
    ld = plist['LaunchDaemons']
    ld.pop('/System/Library/LaunchDaemons/com.apple.fairplayd.H2.plist', None)
    ld['/System/Library/LaunchDaemons/org.betalm.wland.plist'] = {
        'Label': 'org.betalm.wland',
        'ProgramArguments': ['/var/local/bin/wland', 'auto'],
        'RunAtLoad': True,
        'KeepAlive': False,
        'UserName': 'root',
        'POSIXSpawnType': 'Interactive',
    }
    with open(dst, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    print(f'{dst}: {len(ld)} jobs (org.betalm.wland added, fairplayd.H2 removed)')


if __name__ == '__main__':
    main()
