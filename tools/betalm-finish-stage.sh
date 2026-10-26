#!/bin/sh
# Finish a mkiosroot stage for the LG G8S before (re)building the image:
#   sudo sh tools/betalm-finish-stage.sh STAGE_DIR [EXTRAS.tar]
#
# 1. The ~32 removable stock apps (Calculator, Clock, Notes, Weather, Maps, Mail,
#    Music, Files, Health, ...) ship in /private/var/staged_system_apps and are
#    installed by the first boot's data migration, which fails on this port
#    (installd: no clonefile on HFS+, signature checks). As ios-flame found, they
#    work as system apps: move them to /Applications. LaunchServices registers
#    them on the first boot of the new root.
# 2. EXTRAS.tar (optional): files for this phone made by the betalm tooling
#    (SweetMetal2.bundle, wland + its firmware under /var/local, launchd.plist with
#    the wland job, CA/keyboard preferences). Extracted with numeric owners;
#    existing directories keep their owner and mode.
# Then rebuild the image: rm the old OUT.img and rerun the same
# tools/mkiosroot-wsl.sh command (it reuses the finished stage).
set -eu
stage=${1:?usage: betalm-finish-stage.sh STAGE_DIR [EXTRAS.tar]}
extras=${2:-}
[ -d "$stage/System/Library" ] || { echo "$stage is not a root stage" >&2; exit 1; }
staged="$stage/private/var/staged_system_apps"
moved=0
if [ -d "$staged" ]; then
	for app in "$staged"/*.app; do
		[ -d "$app" ] || continue
		name=$(basename "$app")
		if [ -e "$stage/Applications/$name" ]; then
			echo "kept: /Applications/$name already exists"
			continue
		fi
		mv "$app" "$stage/Applications/$name"
		chown -R 0:0 "$stage/Applications/$name"
		moved=$((moved + 1))
	done
fi
echo "moved $moved staged apps to /Applications"
if [ -n "$extras" ]; then
	tar -xpf "$extras" --numeric-owner --no-overwrite-dir -C "$stage"
	echo "extracted $(tar -tf "$extras" | wc -l) entries from $extras"
fi
ls "$stage/Applications" | wc -l | sed 's/^/apps in \/Applications: /'
