# UniXNU-raphael: iOS on the Redmi K20 Pro / Mi 9T Pro (`raphael`)

Apple's XNU and iOS on the Redmi K20 Pro / Mi 9T Pro (`raphael`, Snapdragon 855 / SM8150, Adreno 640). This fork
combines two projects:

- **[UniXNU](https://github.com/UniXNU/UniXNU) boot chain** (from the
  [original UniXNU-raphael](https://github.com/JohnLeeeeeee678/UniXNU-raphael)): Project Aloha UEFI for SM8150 with the
  UniXNU menu and DarwinBoot, packed as an Android boot image (`firmware-only.yml`, `patches/`, `board.toml`, `scripts/`).
- **[Bergamota](https://github.com/ry-diffusion/Bergamota) iOS 18 route**: the ios-sweet "Heartwood" kernel
  (xnu-11215.81.4) with upstream's patch series, ported to raphael (patches 0260-0262), and the tools around it
  (`xnu/`, `tools/`, `xnu.yml`). Details: **[docs/IOS18.md](docs/IOS18.md)**.

The earlier iOS 16.7.16 route (UniXNU / xnu-8796 SWEET kernel, home screen + touch, CPU rendering) is archived on
branch [`ios16-sweet`](https://github.com/jackyhowh/UniXNU-raphael/tree/ios16-sweet), still buildable from there.

## Status (2026-11-06)

Tested on a Mi 9T Pro, 6 GB RAM. **iOS 18.7.8 (iPhone XR userland, PAC-stripped) boots from the phone's own boot
partition to the home screen with GPU compositing.**

| Area | State |
| --- | --- |
| Boot chain | Android ABL → Aloha UEFI → UniXNU → DarwinBoot → Heartwood xnu-11215 (`SWEET`, 8 CPUs); flashed to `boot` or `fastboot boot` |
| Root | iOS 18.7.8 HFS+ root (built with Bergamota's Renegade) on `userdata`, read-write |
| Touch, buttons | GT9886 (Normandy) touch, PON / volume keys (patch 0260) |
| GPU | Adreno 640 kernel driver (0261: GMU, HFI v2, GX, zap, CP, governor 257-585 MHz). Chlorophyll (Bergamota's Metal driver over Mesa freedreno, upstream's prebuilt dylibs) as the system compositor and for the iOS 18 wallpaper: home-screen swipes 32 fps (CPU compositing: 10 fps) |
| Clock, battery | Wall clock from the PM8150 RTC (set once, kept across reboots); live charge level from the PM8150B fuel gauge (0262) |
| Debug | USB CDC-ECM link to WSL: SSH and the kernel log over UDP (`tools/raphael/usblink.sh`) |
| Not working | Wi-Fi, Bluetooth, cellular, audio, camera, sensors; Settings > Date & Time hangs |

## Layout

| Path | What |
| --- | --- |
| `.github/workflows/xnu.yml` | Kernel build (macOS runner, ~20 min): `xnu/patches-11215` on xnu-11215.81.4, artifact `xnu-kernel` |
| `.github/workflows/firmware-only.yml` | UEFI + UniXNU + DarwinBoot firmware and a base boot image (`variant=full`) |
| `xnu/patches-11215/` | Heartwood patch series: upstream Bergamota 0001-0259, raphael 0260 (board, keys, touch), 0261 (A640 GPU), 0262 (RTC, battery) |
| `xnu/gpu-abi/` | Kernel/userland GPU interface header (SweetAdreno user client) |
| `tools/raphael/` | `mkboot-raphael.sh` (pack kernel + ADT + boot-args into the base image), `usblink.sh`, touch host test, `bgmtl-env` helper and Chlorophyll staging scripts |
| `tools/mkadt.py`, `tools/adt-gpufw.py` | Apple Device Tree for raphael (`--board raphael`, from Apple's D22AP tree) and GPU firmware into it |
| `tools/renegade/` | Bergamota's Renegade (iOS root builder, `pacstrip`); used here for the Chlorophyll staging |
| `tools/xnu-cc.py` | Re-run single kernel compiles from a CI build log (local syntax checks) |
| `patches/`, `board.toml`, `scripts/` | UniXNU firmware build (Aloha + UniXNU + DarwinBoot patches, root flashing helpers) |

## Building

1. **Kernel:** `gh workflow run xnu.yml -R jackyhowh/UniXNU-raphael --ref main` (also runs on pushes to `main` that
   touch `xnu/patches-11215`). Download the `xnu-kernel` artifact (`kernel.development.sweet`).
2. **Firmware / base image:** `firmware-only.yml` with `variant=full` (reuses a kernel artifact from an earlier run;
   that kernel is replaced in step 4 anyway).
3. **ADT:** `tools/mkadt.py --board raphael` from Apple's D22AP device tree (taken from Apple's public IPSW, never
   committed).
4. **Boot image:** `tools/raphael/mkboot-raphael.sh BASE.img kernel.development.sweet raphael.adt OUT.img` in
   Linux/WSL, with the boot-args from [docs/IOS18.md](docs/IOS18.md). Try it with `fastboot boot OUT.img` before
   flashing it to `boot`.
5. **iOS root:** built with Renegade from Apple's IPSW, privately (it is Apple's operating system and is not
   published here).

Not in this repository and never published: Apple IPSW contents, iOS root images, Apple device trees, the macOS SDK's
private headers, and Qualcomm firmware (GPU GMU / SQE / zap, taken from the phone's own vendor image).

## Credits and licences

See [NOTICE.md](NOTICE.md). Upstream Bergamota and the original UniXNU-raphael carry no licence; their code is
included here with attribution, and the authors keep all rights to it.
