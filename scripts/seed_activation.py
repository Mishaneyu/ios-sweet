#!/usr/bin/env python3
"""Seed the activation state into an assembled iOS root.

The upstream bring-up tweaks skip this: Apple's OS.dmg ships no
mobileactivationd system container, so there is nothing for them to find and
they print "skipped data ark".  Without it mobileactivationd starts out
unactivated and the system can block on the activation check.

This creates the container the upstream bring-up image was seeded with by
hand and writes the same data_ark.plist into it.

    seed_activation.py /mnt/iosroot
"""
import os
import plistlib
import sys

# The container the upstream bring-up image used.
CONTAINER_ID = "D90C94C4-F94E-4FCF-8A28-A93BE3846D8E"
MOBILE_UID = 501
MOBILE_GID = 501

ARK = {
    "-ActivationState": "Activated",
    "-BrickState": False,
    "-BuildVersion": "20H392",
    "-ProductVersion": "16.7.16",
    "-DataMigrationCompleted": True,
    "-PreSydroDataMigrationCompleted": True,
}


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit(__doc__)
    root = sys.argv[1]

    idir = os.path.join(root, "private/var/containers/Data/System",
                        CONTAINER_ID, "Library/internal")
    os.makedirs(idir, exist_ok=True)
    os.chown(idir, MOBILE_UID, MOBILE_GID)
    os.chmod(idir, 0o755)

    path = os.path.join(idir, "data_ark.plist")
    with open(path, "wb") as f:
        plistlib.dump(ARK, f, fmt=plistlib.FMT_XML)
    os.chown(path, MOBILE_UID, MOBILE_GID)
    os.chmod(path, 0o600)
    print("data ark seeded in", idir)


if __name__ == "__main__":
    main()
