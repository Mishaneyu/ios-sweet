# iOS 26.5.2 on the LG G8S (betalm) - experimental

IOS-26-Hot (Realme X2 Pro / Pixel 4, both SM8150 + Adreno 640) with the LG G8S
board changes from ios-betalm (iOS 16 on the G8S):

- `xnu/patches-26/0151-betalm-lg-g8s-board.patch`: framebuffer 0x9d200000,
  1080x2248 panel shown as a 1125x2342 canvas, Synaptics S3706 touch over GPIO
  I2C (GPIO 55/56, reset 54, ATTN 122, power GPIO 152/59), keys as flame,
  GPU zap region 0x99915000, GPU pool 0xb0400000+200 MB and 0xc0000000+768 MB
  (0xbcc00000-0xc0000000 is not RAM), VIG0-only panel stride, persistent log
  sync. The Realme battery/audio/torch/haptic/touch drivers are not started.
- `phone/wifi`: G8S modem region 0x8e000000+160 MB, radio pool 0xf0000000.
- `shim-betalm/`: the LG ABL shim (boot.img v2, `fastboot boot`), now taking
  the iOS 26 payload (v2 boot_args, 1024-byte boot line).
- `tools/mkiosroot26-wsl.sh`: the iOS 26 root image on WSL (AEA decryption,
  cryptex links, arm64e PAC rewrite, libguestfs fill).

## Root image (WSL)

```sh
mkdir -p ~/ipsw26 && cd ~/ipsw26
ipsw download ipsw --device iPhone12,3 --build 23F84      # iPhone 11 Pro, iOS 26.5.2
ipsw extract --dmg fs  iPhone12,3_26.5.2_23F84_Restore.ipsw
ipsw extract --dmg sys iPhone12,3_26.5.2_23F84_Restore.ipsw
ipsw extract --dmg app iPhone12,3_26.5.2_23F84_Restore.ipsw
EXTRA_TAR=/mnt/d/g8s-ios26-fw.tar ~/ios26/tools/mkiosroot26-wsl.sh OS.dmg.aea SystemOS.dmg.aea AppOS.dmg.aea ~/ios26-root.img 20G
```

The image goes to the G8S `iosroot` partition (TWRP: `adb exec-in "dd of=/dev/block/sda31 bs=4194304" < ios26-root.img`).

## Boot

Kernel (`xnu` workflow) + userland (`userland` workflow) artifacts, then
`tools/mkadt.py --highmem --canvas-height 2342`, `tools/make-ios26-overlay.py`,
`tools/mkpayload.py ... --overlay`, `shim-betalm/build.sh PAYLOAD`, and
`fastboot boot shim-betalm/out/shim-boot.img`. Root on `ufsroot=iosroot`.
