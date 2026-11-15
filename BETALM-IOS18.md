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
