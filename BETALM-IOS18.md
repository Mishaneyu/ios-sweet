# iOS 18 on the LG G8S (`betalm`, SM8150): working notes

Branch `ios18-betalm` of Mishaneyu/ios-sweet. Base: jackyhowh/UniXNU-raphael
`ff6a02f` (iOS 18.7.8 on the Redmi K20 Pro: same SM8150 + Adreno 640, home
screen with GPU compositing, see `docs/IOS18.md`). The G8S support from the
iOS 16 port (branch `betalm`, xnu-8796, patches 0082-0096) is carried over
as kernel patch `xnu/patches-11215/0263`.

## Status (2026-11-15)

- **Kernel built (2026-11-15):** CI run 43739194192 (workflow_dispatch,
  commit f4c0a0e), 9m21s, success. 0263 applied, no compile or link errors.
  `kernel.development.sweet` sha256 e6c7865c3ee323858709d88dedc94464a5e3e88b835e243a188389bf350350b1;
  contains `lge,betalm`, the S3706 touch, the G8S GPU board and the AOP cmd-db lookup.
  Note: pushes that touch many files do not trigger `xnu.yml` (GitHub's
  300-file path-filter limit); start it with `gh workflow run xnu.yml --ref ios18-betalm`.
- `tools/mkadt.py --board betalm` added (not run: needs Apple's D22AP tree).
- Nothing booted on the phone yet.

## Firmware: Renegade edk2-msm + UniXNU (boot1)

`firmware/renegade-betalm/build.sh OUT.img` builds Renegade's betalm UEFI
(edk2-msm e195262) with three extra files in its firmware volume: UniXNU.efi
(app), UniXNUDxe (puts UniXNU first in BootOrder) and DarwinBoot, prebuilt
with Mu_Basecore at the raphael pin b6ae99c + raphael's UniXNU/DarwinBoot
patches (Renegade's 2022 edk2 cannot compile UniXNUPkg). Reproduced from a
clean directory in 4m15s.

Renegade's betalm config: framebuffer 0x9D200000 1080x2248 (the panel buffer
the iOS 16 shim found; Windows ran on it, so the panel keeps refreshing), 6 GB
memory map with a free run 0x140000000 + 0xC0000000 (matches mkadt's window).

**boot1-v0.img** (2026-11-15, sha256 40c32893...8037): UEFI + UniXNU menu
only, no kernel, no payload. `fastboot boot` only (RAM, writes nothing).
Expected: Renegade splash and countdown, then the UniXNU menu (it lists
`\EFI\BOOT\BOOTAA64.EFI` entries it finds, i.e. GRUB on the Arch ESP, plus
its built-in actions). If GRUB/Arch boots directly instead, BootOrder was not
honoured: reboot, press a side button for SimpleInit and pick UniXNU in the
UEFI boot menu.

**boot1 on the phone (2026-11-15, user report):** UniXNU menu shown, keys work
(volume down and power confirmed), countdown ~1 s (no config: built-in
fallback timeout).

## boot2: first kernel start (ESP payload, same firmware)

UniXNU reads `\EFI\UniXNU\unixnu.toml` from any FAT volume, so the payload goes
into the phone's existing ESP (sda31, GRUB's) next to `EFI/BOOT`; the firmware
stays boot1-v0.img. `tools/betalm/mkesp-betalm.sh KERNEL ADT OUT.zip` packs
`EFI/UniXNU/{unixnu.toml,SHA256SUMS,Payloads/kernel.macho,kernel.macho.adt}`
(timeout 5 s). Default boot-args are read-only: no `ufsrw`/`rootrw` (the UFS
driver returns EROFS for every write), `sweetlog=0`, `sweetgpu=0` (no GPU
firmware yet); the script refuses writable args unless `BETALM_RW=1`.

**boot2-esp-v0.zip** = kernel run 43739194192 + `mkadt.py --board betalm`
without `/product` (Apple's d22ap.adt is only needed for MobileGestalt in
userland; the kernel start does not read it). There is no iOS root: expected
end is a wait/panic at mountroot (userdata holds Android). What to watch:
DarwinBoot's hand-off summary (window inside 0x140000000 + 3 GiB, video
0x9d200000 1080x2248), then the kernel console, 8 CPUs, keys, touch probe
lines (`SWEET TOUCH: ... rmi F01 ... product=`) after ~20 s.

`tools/adt.py` was missing from the UniXNU-raphael import (mkadt.py imports
it); it is identical to `scripts/adt.py`.

## What 0263 does

| Area | betalm |
| --- | --- |
| Board | `sweet_board_betalm()` = ADT compatible `lge,betalm`; `sweet_board_sm8150()` = raphael or betalm |
| Keys | PM8150 PON + volume-up PM8150 GPIO6 (SID 0 0xc500), same as raphael |
| RTC / battery | PM8150 RTC, PM8150B FG gen4 status (read-only), design 3550 mAh |
| GPU | A640 table as raphael; cmd-db base read from AOP msg RAM 0x0c3f000c (not raphael's 0x85f20000); zap region 0x99915000 |
| Touch | Synaptics S3706 RMI4 at 0x20, SE17 GPIO 55/56 bit-banged (GSI-only SE), reset 54, ATTN 122, rails GPIO 152/59, 1080x2248 |

Boot-args for the touch: `sweettouchflip=N` (bit0 mirror x, bit1 mirror y,
bit2 swap), `sweettouchfingers=N` (object slots read, default 2),
`sweettouchbb=N` (half period in us, default 2 on betalm).

## Open items (in order)

1. **CI kernel build** of 0263 (`xnu.yml`, push to this branch).
2. **Firmware:** the G8S Mu-Silicium UEFI (already boots Arch) must embed
   UniXNU, UniXNUDxe, DarwinBoot and UniXNURamDiskDxe (see `patches/`,
   `board.toml`, `firmware-only.yml`: raphael builds Aloha's SurfaceDuo1Pkg;
   betalm needs its own `[firmware]` section pointing at the Silicium tree).
3. **Verify before the first boot:**
   - GOP framebuffer base/size from the betalm UEFI (mkadt guesses
     0x9c000000 + 36 MiB, 1080x2248). The panel is DSI command mode: check
     that the UEFI keeps it refreshing after ExitBootServices (the iOS 16
     shim had to enable pingpong autorefresh, `shim/sde.c` on `betalm`).
   - Largest free UEFI memory run -> `mem` and `darwinboot_window` in
     `tools/mkadt.py` (raphael's 0x140000000 + 0xbe000000 is only a guess).
4. **Space for the iOS root:** the GPT is full (32 entries) since the Arch
   repartition (userdata 10 GiB, esp 512 MiB, linux ~90 GiB). Resize an
   existing entry; ask the user before any partition change.
5. GPU firmware from the G8S vendor image: `a640_gmu.bin`, `a630_sqe.fw`,
   `a640_zap.mdt/.b00-.b02` into `/usr/local/share/firmware` (see IOS18.md).
6. Later: CPU frequency (raphael has none; iOS 16 betalm patch 0092 used the
   OSM DCVS), Wi-Fi (0093).

Test images: number every one uniquely (boot1, boot2, ...), never reuse.

**boot2 on the phone (2026-11-15):** UniXNU ran the entry, DarwinBoot drew its
green bar (= DarwinBoot's own DarwinEnter breadcrumb, just before the jump), then
nothing more. Cause found: the ADT `sweet-fbcon` reg was 0x9c000000 (raphael's
GOP); on the G8S the scan-out buffer is 0x9d200000, so all kernel console output
went off screen. The early console is log-only unless `sweetearlyfb=1`.

**boot3-esp-v0.zip:** same kernel; ADT fb = 0x9d200000 + 0x1200000 (to the end
of Display Reserved; TGCM follows at 0x9e400000), boot-args + `sweetearlyfb=1`.
Note for the GUI stage: AppleCLCD::flipSetup assumes 36 MiB after the scan-out
buffer; on betalm buffer 2 would land in TGCM. Page flipping is off by default
(`sweetfb_flip`/`sweetfb_hw`), so this needs a betalm region end before enabling it.

**boot3 (2026-11-15):** still only the green bar, even with `sweetearlyfb=1`, so
the kernel stops before its first early mark. Likely cause: the kernel window.
The kernel takes gDramBase from ADT `/chosen dram-base` (0x140000000, raphael's
layout), but DarwinBoot takes the first run that fits. Raphael's Aloha map only
had a big run at 0x140000000; Renegade's 6 GB map chains 0xC0300000.. past
4 GiB to 0x200000000, so the window most likely landed near 0xC2004000,
outside the RAM the ADT describes.

**boot4-v0.img** (firmware, sha256 772de8f9...d602): DarwinBoot +
`patches/darwinboot-window-dram-base.patch`: the window starts at or above
`/chosen dram-base` of the kernel's ADT (0 = unchanged behaviour), so betalm gets
raphael's 0x141004000. Use with the boot3-esp-v0 payload (unchanged). Ask for a
photo of DarwinBoot's hand-off summary (pa, window, video) every time.

**boot5 (2026-11-15):** probe kernel (CI 43747900875, 0263+0264) + boot4
firmware: all five start.s squares appeared (entry, boot args, page tables,
MMU on, return into arm_init), so the cold-boot assembly runs through. No early
text because `sweet_early_set_cmdline` scans only the first 256 characters and
`sweetearlyfb=1` sat at ~330: early_screen stayed 0. mkesp-betalm.sh now puts
it first. Early marks: text rows from the top, a 48 px block per stage on the
bottom 32 rows.

