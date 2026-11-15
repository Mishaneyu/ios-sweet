#!/bin/sh
# WSL: frames that reached the panel over N seconds (kern.sweet_fb counters), plus compositor CPU and GPU state.
#   fps.sh [SECONDS]   (default 10; swipe on the phone while it runs)
U=$(dirname "$0")/../usblink.sh
N=${1:-10}
sh $U ssh "S=/var/jb/usr/sbin/sysctl
get() { \$S -n kern.sweet_fb | tr ' ' '\n' | grep \"^\$1=\" | cut -d= -f2; }
s0=\$(get swaps); c0=\$(get copy_frames); z0=\$(get zc_frames)
sleep $N
s1=\$(get swaps); c1=\$(get copy_frames); z1=\$(get zc_frames)
echo \"over $N s: swaps \$(( (s1 - s0) / $N ))/s, frames to the panel \$(( (c1 - c0 + z1 - z0) / $N ))/s\"
ps -A -o %cpu,comm | grep -e backboardd -e SpringBoard | grep -v grep
\$S -n kern.sweet_gpu | cut -c1-60"
