# Target Devices

`IOS-26-Hot` supports two Qualcomm Snapdragon 855 (`SM8150` / Adreno 640) phones: **Realme X2 Pro (`RMX1931` / `samurai`)** as the primary iOS 26.5.2 target, and **Google Pixel 4 (`flame`)**. Device serials, IMEIs, and calibration partitions are never published in this repository.

---

## 1. Primary Target: Realme X2 Pro (`RMX1931` / `samurai`)

| Field | Value |
| --- | --- |
| **Model** | Realme X2 Pro (`RMX1931`, codename `samurai`) |
| **SoC** | Qualcomm Snapdragon 855+ / 855 (`SM8150`, `4x Cortex-A55` + `3x Cortex-A76` + `1x Cortex-A76 Prime`) |
| **GPU** | Qualcomm Adreno 640 (`675 MHz`, Mesa Turnip Vulkan + `HotMetal2.bundle`) |
| **RAM** | 8 GB LPDDR4X (`<0x80000000 0x3bb00000>`, `<0xc0000000 0x1c0000000>`) |
| **Storage** | UFS 3.0 (`1d84000.ufshc`, `userdata` on GPT `sda16`) |
| **Display** | Samsung `SOFEF03F-M` `1080x2400` (`20:9`) AMOLED DSC command-mode panel (`cont_splash` at `0x9d000000`) |
| **Touch** | Synaptics `S3706` RMI4 on QUPv3 GENI I2C (`0xc80000`, addr `0x20`, `reset` GPIO 54, `irq` GPIO 122) |
| **Wi-Fi / Cellular** | Qualcomm `WCN3990` 802.11a/b/g/n/ac + Snapdragon X24 LTE modem (`MPSS` + `QRTR`/`QMI`) |
| **Audio** | SM8150 LPASS ADSP + dual Maxim `MAX98937` stereo speaker amplifiers + earpiece + handset mic |
| **Boot Image Format** | Android Boot Image v1 (`Image.gz-dtb`, page `4096`, AVB v1.0 hash footer padded to `96 MiB` = `100,663,296` B); TWRP lives separately in `recovery` (`sde18`) |

---

## 2. Secondary Target: Google Pixel 4 (`flame`)

| Field | Value |
| --- | --- |
| **Model** | Google Pixel 4 (`flame`, SKU `G020M`) |
| **SoC / RAM** | Qualcomm Snapdragon 855 (`SM8150`), 6 GB LPDDR4X |
| **Storage** | UFS 3.0 (`userdata` on GPT `sda15`) |
| **Display** | `NT37280` `1080x2280` command-mode DSC OLED (`cont_splash` at `0x9c400000`) |
| **Touch** | ST `ftm5` (`fts_touch`) over QUPv3 GENI SPI (`0x890000` CS0) |
| **Android Baseline** | Android 13 (`TP1A.221005.002.B2`, bootloader `c2f2-0.5-8906123`, unlocked) |
| **Boot Image Format** | Android Boot Image v2 (LZ4 frame kernel `text_offset 0x80000` + AVB footer padded to `64 MiB`), supports direct `fastboot boot` |

---

## Button Combos & Recovery

| Action | How |
| --- | --- |
| **Bootloader (Fastboot)** | Power off, then hold **Power + Volume Down** (or `adb reboot bootloader`) |
| **Recovery (TWRP)** | Select **Recovery mode** in Fastboot with Volume keys + Power (or `adb reboot recovery`) |
| **Force Restart** | Hold **Power + Volume Up** (~10 s on `RMX1931`) or hold **Power** (~30 s on `flame`) |

See [RECOVERY.md](RECOVERY.md) for restoring stock Android safely.
