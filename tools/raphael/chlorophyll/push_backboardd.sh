#!/bin/sh
# WSL: Chlorophyll as the system compositor on the phone (backboardd linking bgmtl-env.dylib + bgmtl-inject.dylib,
# staged in native/bgmtl-stage by stage_bb.py). Backup of the stock binary: /var/root/bgmtl-backup/backboardd.
#   push_bb.sh install   helper + modified backboardd (inert until armed)
#   push_bb.sh arm       write bgmtl-bb.arm, clear bgmtl-bb.off/state
#   push_bb.sh disarm    remove bgmtl-bb.arm (next backboardd start is stock)
#   push_bb.sh restart   restart backboardd (SpringBoard follows)
#   push_bb.sh revert    stock backboardd back, disarmed
#   push_bb.sh status    processes, logs, GPU counters
set -e
U=$(dirname "$0")/../usblink.sh
S=${BGMTL_STAGE:?set BGMTL_STAGE to the staging root}
B=/usr/libexec/backboardd
E=/usr/local/lib/bgmtl/bgmtl-env.dylib
T=/private/var/tmp
put() {	# put LOCAL REMOTE MODE: temp name, then rename
	sh $U ssh "cat > '$2.tmp' && chmod $3 '$2.tmp' && mv '$2.tmp' '$2'" < "$1"
}
case ${1:-} in
install)
	sh $U ssh "mkdir -p /var/root/bgmtl-backup; [ -e /var/root/bgmtl-backup/backboardd ] || cp -p $B /var/root/bgmtl-backup/backboardd"
	put "$S$E" "$E" 755
	put "$S$B" "$B" 755
	sh $U ssh "sha256sum $E $B /var/root/bgmtl-backup/backboardd"
	;;
arm)
	sh $U ssh "rm -f $T/bgmtl-bb.off $T/bgmtl-bb.state; echo armed > $T/bgmtl-bb.arm; chmod 644 $T/bgmtl-bb.arm; ls -la $T | grep bgmtl-bb"
	;;
disarm)
	sh $U ssh "rm -f $T/bgmtl-bb.arm; ls -la $T | grep bgmtl-bb || true"
	;;
restart)
	sh $U ssh 'set -- $(ps -A -o pid= -o comm= | grep /usr/libexec/backboardd | grep -v grep); echo "backboardd was $1"; kill $1; sleep 6; ps -A -o pid,etime,comm | grep -e backboardd -e SpringBoard | grep -v grep'
	;;
revert)
	sh $U ssh "rm -f $T/bgmtl-bb.arm; cp -p /var/root/bgmtl-backup/backboardd $B.tmp && mv $B.tmp $B && sha256sum $B"
	;;
status)
	sh $U ssh "ps -A -o pid,%cpu,etime,comm | grep -e backboardd -e SpringBoard -e MercuryPoster | grep -v grep; tail -4 $T/bgmtl-env.log; grep backboardd $T/bgmtl.log | tail -8; cat $T/bgmtl-bb.state $T/bgmtl-bb.off 2>/dev/null; /var/jb/usr/sbin/sysctl -n kern.sweet_gpu | tr ' ' '\n' | grep -e ^state -e ^level -e clients -e submitted -e completed -e hangs -e recoveries"
	;;
*) sed -n '2,10p' "$0"; exit 2 ;;
esac
