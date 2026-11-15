# iOS 18 on the Redmi K20 Pro / Mi 9T Pro (raphael): the Bergamota route

Port of [Bergamota](https://github.com/ry-diffusion/Bergamota) (iOS 18.7.8 on the Poco F1 / SDM845) to `raphael`
(Qualcomm SM8150, 6 GB, Adreno 640). The kernel patch series in `xnu/patches-11215` is upstream's, plus the raphael
patches 0260-0262; it was developed in a private copy and imported here (see NOTICE.md). The Poco F1 parts of the
series stay untouched (they check for `xiaomi,beryllium`).

## What is raphael-specific

| Piece | File |
| --- | --- |
| Board, volume-up key, GT9886 touch | `xnu/patches-11215/0260-raphael-board-keys-touch.patch` |
| Touch host test (109 checks) | `tools/raphael/touch-test/run.sh <patched sweet_touch.c>` |
| Apple Device Tree | `tools/mkadt.py --board raphael` |
| Boot image | `tools/raphael/mkboot-raphael.sh BASE.img KERNEL ADT OUT.img` |

The ADT root compatible is `D22AP / iPhone10,3 / xiaomi,raphael / xiaomi,sweet / qcom,sm8150`.
Every driver without a raphael check takes the sweet (SM7150) defaults, which already booted
raphael with the xnu-8796 SWEET kernel (UFS root, USB, framebuffer, 8 CPUs). The Poco F1-only
drivers (MDP, SPMI charger/battery writes, A630 GPU, WLAN/BT, cpufreq, reboot) check for
`xiaomi,beryllium` and stay off.

## Boot chain

Not Enxerto: raphael boots Project Aloha UEFI (SM8150) with UniXNU and DarwinBoot, built by this
repository's `firmware-only.yml` (variant `full`). Its boot image is an
Android boot header v0 whose ramdisk is a 64 MiB FAT32 with `EFI/UniXNU/unixnu.toml` and
`EFI/UniXNU/Payloads/kernel.macho{,.adt}`. `mkboot-raphael.sh` swaps the kernel, the ADT and the
boot-args in that FAT and keeps the firmware. DarwinBoot reserves `/chosen/darwinboot-memory-size`
(0xbe000000) for the kernel; it needs `darwinboot-window-split.patch` and the 6 GB window size
from this repository (`patches/`, `board.toml`).

## Hardware facts used

- Touch: Goodix GT9886 (Normandy) at 0x5d on QUP19 (wrapper 2 SE2, 0xc88000), SDA/SCL 57/58
  function 2, reset 54, IRQ 122, load switches 98/100, TLMM SOUTH tile 0x03d00000; GCC vote
  0x52014 bits 1/2/6, RCG 0x1e3a8.
- Keys: PM8150 PON (SID 0, 0x0810) and volume-up PM8150 GPIO6 (SID 0, 0xc500).
- Framebuffer: UEFI GOP 1080x2340 at 0x9c000000 (cont_splash 36 MiB).
- CPUs: 4x Kryo 485 Silver + 3 Gold + 1 Prime, MPIDR 0x000-0x700; ADT cpu compatible must be
  `qcom,kryo470` (SweetCPU's match), else boot stalls in `ml_wait_max_cpus`.

## USB debug link (SSH and kernel log)

The kernel's USB gadget (`sweetusb=2`, 1209:0001) is CDC-ECM. Windows 10 has no driver for it, so the
device goes to WSL: `usbipd bind --busid 1-1 --force` once (admin; a third-party USB filter needs
`--force`), then `usbipd attach --wsl --busid 1-1` per phone boot with a WSL session running. In WSL
(root): `tools/raphael/usblink.sh up` (host 192.168.7.1, phone 192.168.7.2, verified 2026-11-06),
`usblink.sh log FILE` (kernel UDP log, port 5140), `usblink.sh ssh [CMD]` (dropbear, key-only; key
`~/.ssh/raphael_ios_ed25519`, whose public half the root build installs). iOS has no `dmesg`: boot-time
kernel lines are only in the UDP log, so start the logger right after `fastboot boot`.

## GPU (Adreno 640)

Kernel patch `0261-raphael-gpu-a640.patch` extends the Poco F1 GPU driver (`bsd/dev/sweet_a630.c`) with a board
table and the A640 path: SM8150 addresses, AOP QMP + host rail votes for the CX power-up, MMU-500, the
block-format GMU firmware with icache/dcache regions, HFI v2, the A640 CP init values, and the raphael zap
carve-out 0x98f15000. Firmware comes from raphael's own vendor image and goes into the ADT:

```sh
python3 tools/adt-gpufw.py research/raphael/raphael-3x.adt research/raphael/raphael-3x-gpu.adt \
    --fw FIRMWARE_DIR --zap FIRMWARE_DIR --gpu a640     # a640_gmu.bin, a630_sqe.fw, a640_zap.mdt/.b00-.b02
```

Bring-up is staged with `sweetgpu=N` (1 read-only report, 2 CX power + SMMU/TCM checks, 3 GMU boot held,
4 HFI v2, 5 GX on, 6 zap, 7 CP + submission test, 8 kept up + SweetAdreno630 published). `sweetgpuvote=0`
skips the host rail votes. Watch it with `tools/raphael/usblink.sh log` from the start of the boot.

Status (2026-11-06, kernel run 42175939164, f399ef3): `sweetgpu=8` reaches the end on the phone. The GMU boots,
all HFI v2 messages are acknowledged, GX comes up at 257 MHz, the zap is authenticated, the CP NOP and fill tests
pass, and SweetAdreno630 is published. The firmware can also be read late from `/usr/local/share/firmware`
(`sweetgpuwait=1800` keeps the driver waiting for it). `sweetgmuvar=N` restores older GMU choices for
experiments (see the patch header).

### Metal in the wallpaper (Chlorophyll)

Upstream's prebuilt Chlorophyll dylibs (release `bgmtl-20261025`: Gallium freedreno + AIR front end, kernel
ABI 1) run unchanged on the A640 (Mesa reports FD640). Renegade's `bgmtl()` installs them into
MercuryPosterExtension only, and the stock iOS 18 wallpaper (`sweetposterset com.apple.MercuryPoster
fluidity.two`) then renders on the GPU.

raphael's backboardd composites on the CPU. Mercury hands its IOSurface to iOS right after committing (its
`waitUntilScheduled` returns at once outside backboardd), so the CPU compositor could read a frame the GPU was
still drawing: a diagonal cut (one of the two triangles of the last full-screen draw) and flicker. The fix is
synchronous commits in that process: `/private/var/tmp/bgmtl.sync` on the phone.

### Chlorophyll as the system compositor (backboardd)

Working on the phone since 2026-11-06: the whole UI is composited on the A640, noticeably smoother than the CPU
compositor (backboardd went from up to 465 % CPU to ~0-25 %), touch unaffected, 0 GPU hangs. Same prebuilt dylibs.

Upstream starts backboardd on Chlorophyll from its `bgl-launch` wrapper (built on a Mac with the GL payload). Here
backboardd links `bgmtl-env.dylib` and then `bgmtl-inject.dylib` (two extra LC_LOAD_DYLIBs, re-signed ad hoc with
its identity and all 105 entitlements), and `bgmtl-env.dylib` does bgl-launch's Metal gate before the inject's
constructor runs: `bgmtl-bb.arm` present, `bgmtl-bb.off` absent, libraries readable, `kern.sweet_gpu` reports
`state=on` (at boot backboardd starts before the driver has its firmware: that launch is stock and not counted), and
the previous Metal launch of this boot (`kern.bootsessionuuid`) became healthy (otherwise `bgmtl-bb.off`). Then
`BGMTL_BB=1`; the inject marks the launch healthy 10 s after its first completed command buffer. Its own watchdog
(GPU lost, 3 failed command buffers, no completion within 60 s) writes `bgmtl-bb.off` and exits. An image without
`sweetgpu=8` always gives a stock backboardd.

Scripts (WSL; the staging root holds `orig/` copies pulled from the phone and the release dylibs):

```sh
export BGMTL_STAGE=/path/to/stage
tools/raphael/bgmtl-env/build.sh                                     # bgmtl-env.dylib, no SDK
python3 tools/raphael/chlorophyll/stage_mercury.py tools/renegade $BGMTL_STAGE DYLIBS   # Renegade bgmtl()
python3 tools/raphael/chlorophyll/stage_mercury_env.py tools/renegade $BGMTL_STAGE bgmtl-env.dylib
python3 tools/raphael/chlorophyll/stage_backboardd.py tools/renegade $BGMTL_STAGE
tools/raphael/chlorophyll/push_mercury.sh install       # wallpaper; then on the phone: touch /private/var/tmp/bgmtl.sync
tools/raphael/chlorophyll/push_backboardd.sh install    # inert until: push_backboardd.sh arm; push_backboardd.sh restart
tools/raphael/chlorophyll/push_backboardd.sh status     # disarm / revert put the CPU compositor back
```

The stock binaries are kept on the phone in `/var/root/bgmtl-backup/`.

Boot setup that works (image T11: kernel run 42185225498 / fc830fa, plain `raphael-3x.adt`, firmware read from
`/usr/local/share/firmware`): `sweetgpu=8 sweetgpuwait=1800 sweetgpufreq=g sweetfb_hz=60`. The GPU comes up while
backboardd starts; `bgmtl-env.dylib` waits for it, and backboardd composites on the GPU from boot.
- `sweetfb_hz=30` (used before) copies a frame to the panel only every second vsync: about 30 fps at most.
- `sweetgpufreq=g`: the DCVS governor (257-585 MHz); the default is the lowest level, fixed.
- GPU firmware in the ADT (`raphael-3x-gpu.adt`) does not work yet: the driver then starts very early and the GMU
  took a translation fault (CB0 FSR 0x402, write at GMU VA 0x57308) on the one boot tried.

Home-screen swipes, frames reaching the panel (`tools/raphael/chlorophyll/fps.sh`, kern.sweet_fb counters), 60 Hz:
CPU compositing 10/s with backboardd at 524 % CPU; GPU compositing 32/s with backboardd at 40 %, GPU 427 MHz 64 % busy.

`tools/raphael/bgmtl-env/` builds `bgmtl-env.dylib` without Apple's SDK (clang + ld64.lld in WSL: no imports,
no fixups, arm64e header, ad hoc signature). Besides the backboardd gate above, in the poster it turns
`/private/var/tmp/bgmtl.defer` into `BGMTL_DEFER_SCHEDULED=1` and the first line of
`/private/var/tmp/bgmtl.fddebug` into `FD_MESA_DEBUG`, and logs each start to `/private/var/tmp/bgmtl-env.log`.
It was used for the A/B tests (`nobin` and `sysmem,nolrz,noubwc` did not change the flicker).

## Clock and battery (patch 0262)

Checked on the phone 2026-11-06 (kernel run 42190048551, image T12):
- RTC: PM8150 RTC enabled (`0x6046` = 0x80), raw seconds counting (0x3bcf7 at the test); the alarm is disabled, so
  its data bytes hold the offset. The time has to be set once (no network time without Wi-Fi): as root
  `date -u MMDDhhmmYYYY.ss` over SSH, or Settings > General > Date & Time. The kernel then logs
  `SWEET RTC: offset 0 -> N` and stores N in 0x6140-0x6143; the next boot started 1 s from the PC's clock.
- Battery: `SWEET BATTERY: PM8150B FG SOC, 100% 4396mV external=1 charging=0`. MSOC 0x4009 = 0xff, VBATT
  0x41a0 at 122.07 uV/bit (4.39 V), charger status 0x1006 = 0x45 (state 5, terminated), USBIN 0x1310 bit 4 set.
- Register dumps: `sysctl -w kern.sweet_spmi_addr=$(( (SID << 16) | 0xADDR ))`, then `sysctl kern.sweet_spmi_dump`.
- Time zone without Settings (its Date & Time page hangs on this phone): as root
  `ln -sf /var/db/timezone/zoneinfo/Asia/Kuala_Lumpur /var/db/timezone/localtime`, then restart SpringBoard.
