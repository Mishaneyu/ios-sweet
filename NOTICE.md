# Notices and credits

This fork combines work from several projects. Everything not listed below as this fork's own work belongs to its
authors.

| Part | From | Licence |
| --- | --- | --- |
| Boot chain build (`firmware-only.yml`, `patches/` firmware patches, `board.toml`, `scripts/`) | [JohnLeeeeeee678/UniXNU-raphael](https://github.com/JohnLeeeeeee678/UniXNU-raphael), built on [UniXNU](https://github.com/UniXNU/UniXNU), [Project Aloha](https://github.com/Project-Aloha) (mu_aloha_platforms) and Microsoft Project Mu | none stated for UniXNU-raphael; see the respective upstream projects |
| Kernel patch series `xnu/patches-11215` 0001-0259, `xnu/gpu-abi`, `tools/renegade`, `tools/sweetnet/addlc.py`, `tools/mkadt.py`, `tools/adt-gpufw.py`, `tools/zap-split.py`, `tools/xnu-cc.py`, `tools/xnu-local-build.sh`, `xnu.yml` | [ry-diffusion/Bergamota](https://github.com/ry-diffusion/Bergamota), itself a fork of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) | none stated; imported from Bergamota `develop` lineage at the private port's commit 638d2cb, with attribution |
| XNU | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) xnu-11215.81.4, built with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) | APSL 2.0 (xnu) |
| Chlorophyll (used on the phone, not in this repository) | [ry-diffusion/Chlorophyll](https://github.com/ry-diffusion/Chlorophyll), prebuilt dylibs from Bergamota release `bgmtl-20261025` | MIT (with the hackios / Darwin on redwood AIR front end, MIT) and Mesa (MIT) |

This fork's own work (jackyhowh, 2026): the raphael patches `xnu/patches-11215/0260`-`0262`, `tools/raphael/`, the
changes to `xnu.yml` and `tools/mkadt.py` for raphael, `docs/IOS18.md`, and the earlier UniXNU fixes listed on
branch `ios16-sweet` (firmware window split, raphael CPU compatible, GT9886 touch, diagnostics).

Hardware facts (register numbers and values) were taken from Linux, the downstream Qualcomm kernel and the
phone's own device trees and vendor image; no code from them is copied.

Not included and never published: Apple IPSW contents, iOS root images, Apple device trees, the macOS SDK's private
headers, Apple's KDK, Qualcomm firmware.
