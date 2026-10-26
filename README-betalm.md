# ios-sweet → LG G8S (`betalm`, SM8150)

This branch ports [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)
(XNU xnu-8796 + the unmodified iPhone X iOS 16.7.16 userland) from the Redmi
Note 10 Pro (SM7150) to the **LG G8S ThinQ** (LM-G810, `betalm`, SM8150).
Hardware facts come from LineageOS `android_kernel_lge_sm8150` (lineage-22.2,
`sm8150.dtsi`, `sm8150-sde.dtsi`, `lge/sm8150-beta/*`) and
`android_device_lge_sm8150-common`.

**Status:** shim tested on the phone (2026-10-25). It takes over from ABL,
draws on the panel, sees the framebuffer at `0x9d200000` (VIG0, 1080×2248,
stride 4320, 32-bit) with no tear check active (so ABL keeps the panel
refreshing and autorefresh is not needed), and reads the UFS GPT (userdata
on LUN 0). The XNU kernel is **not yet run on the phone**. The shim
builds, its boot image round-trips through `tools/unpack_bootv2.py`, the ADT
and FDT code was tested on the host, the new kernel code compiles with
`-Wall -Wextra -Werror`, and the patch series applies cleanly in CI order.
Everything that depends on the real hardware is listed under
[What only the phone can confirm](#what-only-the-phone-can-confirm).

## Why the G8S is close to sweet

| Block | sweet (SM7150) | G8S (SM8150) | Change |
|---|---|---|---|
| CPUs | 6×A55 + 2×A76, ARMv8.2, no PAC, MPIDR Aff1 = core | 4×A55 + 4×A76, same ISA, same MPIDR layout | ADT only |
| GIC / timer | GICv3 0x17a00000 / 0x17a60000, vtimer INTID 19 | identical | none |
| UFS | 0x1d84000 | identical | none |
| SPMI arbiter v5 | 0xc440000 | identical | none |
| PSHOLD | 0xc264000 | identical | none |
| Splash | 0x9c000000, 1080×2400, video mode | region 0x9c000000, buffer **0x9d200000**, 1080×2248; DT says command mode, but ABL leaves it refreshing continuously | shim reads it from the MDP |
| Touch | Goodix GT9896 on SE7, RPMh rail | **Synaptics S3706** at 0x20 on **SE17** (0xc80000), GPIO rails | new driver code |
| Vol-up | PM6150L GPIO2 | PM8150 GPIO6 | constants |
| Boot image | header v2, base 0, page 4096, DTB 0x1f00000 | identical | none |

## What this branch changes

**Shim** (`shim/`)
- `board.h`: G8S addresses (IMEM restart reason `0x146bf65c`, MDSS `0xae00000`, SDE blocks).
- `sde.c` (new): the G8S panel (RM69299) is **DSI command mode**, so the MDP
  only sends a frame when kicked and nothing drawn later would show. The shim
  finds the pipe ABL uses for the splash (SSPP `SRC0_ADDR/SIZE/YSTRIDE/FORMAT`),
  the active CTL and the pingpong with the tear check running, then enables
  **pingpong autorefresh** and kicks the CTL once. The panel then refreshes
  from RAM at 60 Hz like a video-mode panel, so every console and AppleCLCD
  work unchanged. The detected geometry replaces all hard-coded sizes.
- `fdt.c` + `loader.c`: RAM = phys_base to the end of ABL's `/memory` range
  (mkadt's 3 GiB is only the fallback). The detected framebuffer goes into
  `boot_args.Video`.
- `adtfix.c` (new): patches `dram-size`, `/pram` and `/sweet-fbcon` in the
  ADT in place at boot.
- The shim appends `shim.fb=… shim.sde=… shim.dram=…` to the boot-args, and
  the kernel logs them, so the persistent log shows what the shim saw even
  if the screen stays dark.
- `build.sh`: G8S layout; `DTB=` override; falls back to
  `tools/mkbootimg_v2.py` when `mkbootimg` is not installed.

**Kernel patches** (`xnu/patches/0082–0086`, on top of sweet's 0001–0081)

| # | What |
|---|---|
| 0082 | `SWEET_BOARD_BETALM`, panel constants for the early consoles (`start.s`, `sweet_early.c`, `arm_vm_init.c`) |
| 0083 | Touch HID takes the panel size from `boot_args.Video` instead of 1080×2400 |
| 0084 | Keys: PM8150 GPIO6 (vol-up) + PON KPDPWR/RESIN. sweet's SM7150 USB probe off by default (`sweetusb=1` to force) |
| 0085 | Touch: LG's QUP firmware runs SE17 GSI-only (FIFO interface disabled), so I²C is **bit-banged on its pins** (GPIO 55/56), as the Poco F1 port (Bergamota) does; a polled **GPI DMA** path is kept behind `sweettouchgsi=1` (its first try reset the SoC), SM8150 GCC clocks (HW-gated branches, DFS level scan), SOUTH-tile TLMM pins, GPIO power (vdd 152, vcl 59, reset 54, ATTN 122), **Synaptics RMI4**: PDT scan, F01 setup, F12 register-descriptor parsing, polled reports |
| 0086 | Log the boot-args (with the shim report) to the persistent log |
| 0087 | Console log strip (`sweetfb_logrows=N`): the log keeps the bottom N text rows, the GUI (boot logo, home screen) shows above it; a panic takes the whole screen |
| 0088 | `sweet_log_sync()`: write the log out now and wait; the GPI touch code logs each step through it, so a SoC reset without a panic still leaves the last step on disk |
| 0089 | Touch: the outer 120 px of the panel stretch to the edges of the presented 1125×2436 display, so the bottom edge (home gesture) is reachable despite the 94-row crop |
| 0090 | **Adreno 640 GPU from [ios-flame](https://github.com/NWylynko/ios-flame)** (Pixel 4, same SM8150; MIT, its XNU patches APSL 2.0): GPU bring-up (GPUCC, Adreno SMMU, GMU + HFI, SQE, zap shader through TrustZone), `/dev/pmem`, GPU present (scale-to-fit), `IOAcceleratorES` + Metal user client, AMFI trust query, `-mcpu=cortex-a55` (no EOR3). G8S changes: zap region 0x98715000, GPU-visible panel memory 0x9d200000, panel VIG0 only, GPU buffer pool 0xb0000000+1 GB, bring-up log synced step by step. Off unless `sweetgpu=1` |
| 0091 | `sweet_inject`: files packed into the boot payload (`mkpayload.py --inject DIR`, `DIR/.owners` for owners) are written into the iOS root after it mounts, before launchd: how the Metal bundle and its settings get onto the phone without SSH (`sweetinject=0` skips) |
| 0092 | CPU clusters to full speed through the OSM hardware DCVS (ios-flame's `sweet_cpufreq.c`; ABL leaves them slow). `sweetcpufreq=0` keeps ABL's speed, `=N` caps at N MHz |
| 0093 | **Wi-Fi from ios-flame** (Pixel 4, same SM8150 + WCN3990; ios-topaz/HackiOS code inside, APSL 2.0): `/dev/wlan` to the userland radio (wland), `en0` + Apple80211 ioctls for wifid, IO80211Controller/IONetworkInterface registry entries, IOKit quiesce, keystore wrap; 10-bit surfaces as BGRA for WebKit's content process. With `mkadt.py`'s `wlan` node launchd starts wifid |
| 0094 | Addresses from the **live** device tree (`/sys/firmware/fdt`, after LG's dtbo): zap shader at pil_gpu_region 0x99915000 (the boot image's base DT said 0x98715000, inside LG's SLPI region), GPU pool 0xb0400000 + 1020 MB (clear of LG's ramoops at 0xb0000000) |
| 0095 | Time of day from the PM8150 RTC (ios-flame 0099) plus boot-arg `sweetrtcoff` (Android: `date +%s` minus `/sys/class/rtc/rtc0/since_epoch`); publishing IORTC also removes IOKit's 30 s wait at every boot |

`tools/mkadt.py`: Kryo 485 topology and clocks, 1080×2248, `lge,betalm`
compatible, 3 GiB RAM default. `tools/sweet-log-pull.sh -p NAME`: read the
log from any partition.

## How to do it

You need: the G8S with the unlocked bootloader (Renegade engineering ABL is
fine), TWRP booted with `fastboot boot`, a Linux PC (your Arch box), and a
GitHub fork of ios-sweet for the kernel build.

### 0. Back up

From TWRP (adb root), pull at least `persist`, `modemst1`, `modemst2`,
`fsg`, `fsc`, `boot_a`, `boot_b`, `dtbo_a`, `dtbo_b`, `vbmeta_a`, `vbmeta_b`,
and your userdata contents. Nothing here writes those partitions, but the
root image goes onto `userdata` or a new partition carved from it.

### 1. Get your DTB

Use the boot image of the slot you boot (LineageOS is on slot b):

```
adb shell dd if=/dev/block/by-name/boot_b of=/tmp/boot.img
adb pull /tmp/boot.img backup/boot.img
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked   # -> dtb/boot_unpacked/dtb
```

### 2. First test: shim only (no kernel needed)

```
shim/build.sh                       # no payload
fastboot boot shim/out/shim-boot.img
```

Expected: the screen turns **red** with lines `FB base= sspp= / w= h= /
stride= fmt= / SDE cmd= pp= / ctl= autoref= / live=`, then `LOADER no
SWEETXNU payload`, then a UFS probe. It reboots to fastboot after about 3
minutes. **Take a photo of the screen and send it to me.** This one test
answers the biggest open question (display hand-off). If the screen keeps
showing the boot logo, autorefresh did not start; send that too.

### 3. Build the kernel

Push this branch to your fork and run CI on it:

```
git push YOUR_FORK betalm
gh workflow run xnu.yml -R YOU/ios-sweet --ref betalm
gh run download RUN_ID -R YOU/ios-sweet -n xnu-kernel -D xnu/out
```

Keep macOS 14 / Xcode 15 (newer `iig` does not match the xnu-8796 IORPC ABI).

### 4. Build the iOS root image

Same as sweet: the iPhone X (`iPhone10,3`) iOS 16.7.16 (20H392) IPSW, then

```
sudo bash tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

**On WSL2 (no HFS+ in Microsoft's kernel):** use `tools/mkiosroot-wsl.sh`,
which does the same as `mkiosroot.sh` + `sweet-root-tweaks.py` but fills the
HFS+ image through libguestfs (a small VM with a stock Ubuntu kernel). Keep
everything under `~`, not `/mnt/c`:

```
sudo apt install -y hfsprogs rsync python3 libguestfs-tools linux-generic \
    android-sdk-libsparse-utils
sudo chmod 0644 /boot/vmlinuz-*          # libguestfs reads the kernel as you
export LIBGUESTFS_BACKEND=direct
libguestfs-test-tool | tail -3           # must end in TEST FINISHED OK
tools/mkiosroot-wsl.sh OS.dmg SystemOS.dmg AppOS.dmg ~/ios16-root.img 16G
img2simg ~/ios16-root.img ~/ios16-root.simg
```

`apfs-fuse` is not packaged on newer Ubuntu: build it from
[sgan81/apfs-fuse](https://github.com/sgan81/apfs-fuse) with
`cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5`, after adding
`#include <cstdint>` to `ApfsLib/PList.h` (GCC 15).

### 5. Choose where the root goes

**A. Dual boot (keeps Android).** Make a 16 GiB+ partition named `iosroot`
on LUN 0 (`/dev/block/sda`, where `userdata` is) and boot with
`ufsroot=iosroot`. userdata is f2fs, which cannot shrink, so recreating it
smaller **formats Android's data once**. After that both systems live side by
side, and Android is untouched by every iOS boot. Send me
`sgdisk --print /dev/block/sda` from TWRP and I'll give you the exact
commands for your layout.

**B. Like sweet.** Flash the root to `userdata` (erases Android data) and
boot with `ufsroot=userdata` (the default).

```
fastboot -S 512M flash iosroot ios16-root.simg     # or: flash userdata
```

### 6. Build the payload and boot

First boot: one CPU, touch delayed, everything logged.

```
python3 tools/mkadt.py shim/out/betalm.adt --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/betalm.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=iosroot unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30'
shim/build.sh ../shim/out/payload.bin
fastboot boot shim/out/shim-boot.img
```

(`ufsroot=iosroot` → leave it out for option B.) Expect the red shim screen
for 3 s, XNU console text, then the Apple logo for about 4–5 minutes on first
boot. Hold Power + Vol-down to get back to fastboot.

When the lock screen appears: `cpus=4` (the A55s), then `cpus=8` (adds the
A76s), and add `sweetfb_noconsole=1` to stop drawing kernel text over the UI.

### 7. Read the log

```
fastboot boot twrp.img
tools/sweet-log-pull.sh -p iosroot logs/boot.txt     # -p userdata for option B
```

Lines to look for: `SWEET: boot-args` (has the shim's `shim.*` report),
`SWEET UFS`, `SWEET KEYS`, `SWEET TOUCH`, `SWEET HID`, `SWEET FB`.

## Useful boot-args

| Boot-arg | Effect |
|---|---|
| `cpus=1/4/8` | 1 core / the four A55s / all eight |
| `ufsroot=NAME` | GPT partition on LUN 0 holding the root (default `userdata`) |
| `shim.noautoref` | Shim leaves the MDP alone (debug only: the screen will not update) |
| `sweetfb_logrows=N` | From the first GUI frame, kernel log only in the bottom N text rows (16 px each); GUI above |
| `sweetgpu=1` | Power up the Adreno 640 and use it for presenting frames (firmware: `mkpayload.py --gpufw DIR` with the phone's own a640_gmu.bin, a630_sqe.fw, a640_zap.mdt/.b02) |
| `sweetgpufb=0` | With `sweetgpu=1`: bring the GPU up but keep the CPU copy for frames |
| `sweetmtl=1/2` | Publish the Metal accelerator (2: ios-flame's SweetMetal2, metal2vulkan + Turnip; needs the bundle in the root) |
| `sweetfb_hz=60` | Present up to 60 frames/s (the old images used 30) |
| `sweetcpufreq=N` | 1 (default) full speed, 0 leave ABL's speed, N: at most N MHz |
| `sweetrtcoff=N` | Seconds to add to the PM8150 RTC for the time of day |
| `sweettouch=0` | Touch driver off |
| `sweettouchi2cus=N` | Bit-banged I²C half clock period in µs (default 2, about 200 kHz) |
| `sweettouchfingers=N` | Object slots read per report when bit-banging (default 2; up to 10) |
| `sweettouchgsi=1` | Touch I²C through GPI DMA instead of bit-banging (experimental: reset the phone once) |
| `sweettouchdelay=N` | Start touch N s after boot (default 45) |
| `sweettouchflip=N` | Touch orientation: bit0 mirror X, bit1 mirror Y, bit2 swap X/Y |
| `sweettouchirq=1` | Read the IC only when ATTN is low (plus every 100 ms) |
| `sweetkeys=0` | Key driver off |
| `sweetfb_native=1` | Present the real 1080×2248 panel instead of the iPhone X canvas |

## What only the phone can confirm

1. **Display hand-off.** ABL leaves MDSS powered with a pipe fetching from
   the splash region, the format is 32-bit, and pingpong autorefresh keeps
   it on screen (`shim.sde=…,cmd,autoref,live`). Step 2 tests exactly this.
2. **Screen fit.** The 1125×2436 iPhone X canvas is centred on the 1080×2248
   panel, so about 94 px are cut at the top and bottom (status bar and home
   indicator partly hidden), as sweet crops 22 px per side. A scaled copy is
   the next improvement once it boots.
3. **Touch IC variant.** The G8S DT lists Synaptics S3706 (0x20) and SiW
   SW42000 (0x28), chosen by GPIO 7. Only Synaptics is implemented, and the log
   says which one answered.
4. **SE17 I²C firmware.** The I²C firmware for SE17 must have been loaded by
   the boot chain (`fw proto 3` in the log), as sweet requires for SE7.
5. **QUP wrapper 2 core clock vote bits** (0 and 3 of `0x52014`). These are
   inferred: the LGE GCC driver only lists AHB (bits 1, 2) and SE0 (bit 4).
6. **Touch orientation.** The first point of each touch is logged
   (`SWEET TOUCH: down x= y=`); fix it with `sweettouchflip`.
7. **Battery.** The G8S has PM8150B with an FG-gen4 gauge, which sweet's QG
   reader does not match, so it shows a static 100 %.

## Known gaps carried from sweet

No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPU (CPU rendering
at about 20–30 fps) or SEP. You can't leave an app with the home gesture yet.
Security is mostly off: don't put anything personal on it. The Wi-Fi and
Adreno work from ios-topaz/HackiOS is the next layer to port (G8S: WCN3998,
Adreno 640).

## GPU and Metal (2026-10-25)

Working on the G8S: the Adreno 640 comes up (GMU with LG's firmware, zap shader through
TrustZone, 545 ms), frames are scaled onto the panel by the GPU, and backboardd and
SpringBoard draw through Metal (metal2vulkan + Turnip, ios-flame's SweetMetal2) with every
blur and transparency. Recipe:

1. Firmware from the phone's own `/vendor/firmware` (`adb root`): a640_gmu.bin, a630_sqe.fw,
   a640_zap.mdt, a640_zap.b02 (keep them out of git).
2. The "sweetmetal2" artifact of `.github/workflows/metal.yml`; stage it with
   `tools/mkmetal-inject.sh SweetMetal2.bundle.tar.gz inj [CLIENTS]`.
3. `tools/mkpayload.py KERNEL ADT payload.bin --gpufw FWDIR --inject inj --cmdline "... sweetgpu=1 sweetmtl=2 sweetfb_hz=60 sweetfb_noconsole=1"`,
   then `shim/build.sh`.

The on-screen kernel log must stay off with the GPU (`sweetfb_noconsole=1`): the panel's
row stride changes to 4352 bytes. The persistent log is unaffected.

## Wi-Fi (in progress)

The radio runs in userland (`wland`, ios-flame's phone/wifi, built by `.github/workflows/metal.yml`
with `phone/patches/wifi/`): it boots the modem from the phone's own firmware, serves rmtfs/TFTP/
pd-mapper, brings the WCN3990 up and hands frames to the kernel's en0 over /dev/wlan. The G8S has
the same Wi-Fi supplies (L1A, L7A, L2C, L11C) and MSA region (0x8bc00000) as the Pixel 4.
The modem region is 0x8e000000 + 160 MB (live device tree; the boot image's base DT has 0x8d800000, which TrustZone refuses).
Files it needs on the phone (from Android, kept out of git): /var/local/fw/modem = modem.mdt/bNN,
modemr.jsn, modemuw.jsn, wlanmdsp.mbn, bdwlan.* from /vendor/firmware_mnt/image;
/var/local/fw/rmtfs = modemst1/modemst2/fsg/fsc images (IMEI and calibration: this phone only).
