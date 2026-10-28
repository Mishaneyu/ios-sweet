# Credits and Licences

`IOS-26-Hot` builds on [ios-sweet](https://github.com/dipa-studio/ios-sweet) and retains its [MIT License](LICENSE). Original additions in this repository (`HotStore 2.0`, `HotWebApp`, `libhot_appfix`, `HotAuth`, `HotAudio.driver`, `HotMetal2` iOS 26 extensions, `cellulard`, `adspd`, `RMX1931` platform drivers, and `IOS-26-Hot` host tooling) are under the MIT License (Copyright (c) 2026 the IOS-26-Hot & ios-flame contributors), with these file-level exceptions:

- `xnu/patches/` and `xnu/patches-26/` modify Apple's XNU kernel, which is licensed under the **Apple Public Source License 2.0 (`APSL-2.0`)**. All patched XNU files remain under `APSL-2.0`.
- Files adapted from third-party open-source projects retain their upstream licences, listed below.

Nothing from Apple's iOS IPSWs, Google's factory images, Oppo's firmware, or Qualcomm's proprietary firmware is included in this repository; see [README.md](README.md) for what you fetch from your own device.

## Projects This Builds On

- **XNU** (Apple, <https://github.com/apple-oss-distributions/xnu>, `APSL-2.0`): `xnu-12377.121.6` (Darwin `25.5.0` / iOS 26 generation) and `xnu-8796.141.3` (iOS 16 generation).
- **ios-sweet** (dipa-studio, <https://github.com/dipa-studio/ios-sweet>, `MIT`): the foundational XNU port to the Redmi Note 10 Pro (`SM7150`) that patches `0001`–`0081`, `shim/`, and early `tools/` originated from.
- **UniXNU** (<https://github.com/UniXNU/UniXNU>, `BSD-2-Clause-Patent` / `MIT`) & **UniXNU-zeta** (none24smu, <https://github.com/none24smu/UniXNU-zeta>, `BSD-2-Clause-Patent`):
  - `phone/metal2/NativeBridgeMetalFrontend.mm`: NativeMetal (`MIT`), extended for iOS 26 Liquid Glass (`MTLLinkedFunctions`, `bitCodeHash`, transient render-target pooling) with runtime optimizations adapted from `UniXNU-zeta` (`BSD-2-Clause-Patent`).
  - `phone/metal2/bridge/` (`NativeMetalBridge` / `NativeMetalObjects`): NativeMetal (`MIT`; see `phone/metal2/bridge/LICENSE`).
  - `phone/turnip/NativeAdrenoABI.h`: NativeAdreno (`MIT`).
- **metal2vulkan** (steelbrain, <https://github.com/steelbrain/metal2vulkan>, `GPL-2.0-or-later OR LGPL-3.0-or-later`): fetched at build time by `tools/build-m2v-bridge.sh`; `phone/metal2/bridge/patches/` fall under its licence.
- **Mesa / Turnip** (<https://gitlab.freedesktop.org/mesa/mesa>, `MIT`): freedreno Vulkan 1.3 driver for Adreno 640, fetched at build time by `tools/build-turnip-ios.sh` with `phone/turnip/patches/`.
- **ios-topaz** (MakrSas, <https://github.com/MakrSas/ios-topaz>) & **HackiOS** (OOSEX GANG, <https://github.com/oosexgang/hackios-xnu-kernel>, `APSL-2.0`):
  - `phone/wifi/radio.c` (userland port of `TopazModem`), `RedwoodWLAN` / `MiniIONetworkStack` `Apple80211` `en0` bridge, `HakiosAudio` CoreAudio HAL structure, and initial `containermanagerd`/`SpringBoard` interpose concepts.
- **Linux Kernel & Qualcomm Open-Source Tools**: `phone/wifi/`, `phone/cellular/`, and `phone/adsp/` follow the protocols documented in upstream Linux `ath10k` (`ISC`), Qualcomm `SMEM`/`SMP2P`/`GLINK`/`QRTR`/`Q6AFE`/`Q6ADM`/`Q6ASM` drivers (`GPL-2.0`), and Bjorn Andersson's `rmtfs`, `pd-mapper`, and `tqftpserv` (`BSD-3-Clause`).
- **blacktop/darwin-xnu-build** (<https://github.com/blacktop/darwin-xnu-build>, `MIT`): macOS XNU cross-build scaffolding used in `.github/workflows/xnu.yml`.
