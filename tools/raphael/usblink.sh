#!/bin/sh
# raphael USB debug link from WSL (or any Linux host): the phone's kernel exposes a CDC-ECM
# gadget (sweetusb=2, 1209:0001); on Windows attach it to WSL first:
#   usbipd bind --busid 1-1 --force      (once, admin; a USB filter driver needs --force)
#   usbipd attach --wsl --busid 1-1      (each boot of the phone; keep a WSL session running)
#
#   usblink.sh up                 bring the link up: host 192.168.7.1/24, phone 192.168.7.2
#   usblink.sh log [FILE]         append the kernel's UDP log (port 5140) to FILE (default stdout)
#   usblink.sh ssh [CMD...]       ssh root@192.168.7.2 with $RAPHAEL_SSH_KEY (default ~/.ssh/raphael_ios_ed25519)
set -eu
PHONE=192.168.7.2 HOST=192.168.7.1/24 PORT=5140
KEY=${RAPHAEL_SSH_KEY:-$HOME/.ssh/raphael_ios_ed25519}

iface() {
	# the cdc_ether interface of the phone (MAC 02:00:5e:77:ee:01 from the kernel's ECM descriptor)
	for d in /sys/class/net/*; do
		[ "$(basename "$(readlink -f "$d/device/driver" 2>/dev/null)")" = cdc_ether ] && { basename "$d"; return; }
	done
	echo "no cdc_ether interface: is the phone attached (usbipd attach --wsl --busid 1-1)?" >&2
	return 1
}

case ${1:-} in
up)
	i=$(iface)
	ip link set "$i" up
	ip addr replace "$HOST" dev "$i"
	ip -br addr show "$i"
	ping -c 2 -W 2 "$PHONE"
	;;
log)
	out=${2:-/dev/stdout}
	exec python3 -u -c '
import socket, sys, time
out = open(sys.argv[1], "a", buffering=1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", int(sys.argv[2])))
out.write("=== usblink log start %s ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
while True:
    d, _ = s.recvfrom(65535)
    out.write(d.decode(errors="replace"))
' "$out" "$PORT"
	;;
ssh)
	shift
	exec ssh -i "$KEY" -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$HOME/.ssh/known_hosts_raphael_ios" \
		-o ConnectTimeout=10 root@"$PHONE" "$@"
	;;
*)
	sed -n '2,10p' "$0"; exit 2 ;;
esac
