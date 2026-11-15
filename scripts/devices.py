#!/usr/bin/env python3
"""Per-device hardware profiles for the SWEET XNU port.

One entry per phone: everything that changes when the same kernel/shim is
brought up on a different Qualcomm board. The values are the ones the early
boot path reads (see NOTES.md); they come from the live FDT / stock DTB of
the device (reserved-memory, /memory, /soc MMIO nodes) unless noted.

`guacamole` is the OnePlus 7 Pro (SM8150) port and `raphael` is the Redmi K20
Pro / Mi 9T Pro (SM8150) -- the SAME silicon. They share GIC/UFS/CPU MMIO
ranges; they differ in the board compatible string and the panel resolution
(raphael's panel is 1080x2340). `sweet` is the original SM7150 bring-up and is
kept for reference.

Used by tools/mkadt.py (ADT) and mirrored by shim/boards/*.h (framebuffer,
GIC, probe addresses). Keep the two in sync when changing a value.
"""

DEVICES = {
    'sweet': {
        # Redmi Note 10 Pro (SM7150 / Snapdragon 732G)
        'name': 'Redmi Note 10 Pro (sweet, SM7150)',
        'board_compat': 'xiaomi,sweet',
        'soc_compat': 'qcom,sm7150',
        'phys_base': 0xb0000000,
        'mem_size': 0x4d000000,
        'high_phys_base': 0x100000000,
        'high_mem_size': 0xfcc00000,
        'gicd': 0x17a00000,
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,
        'gicr_size': 0x100000,
        'timebase': 19200000,
        'vtimer_intid': 19,
        'fb': 0x9c000000,
        'fb_size': 0x1700000,
        'fb_w': 1080,
        'fb_h': 2400,
        'fb_stride': 1080 * 4,
        'ufs': 0x01d84000,
        'ufs_size': 0x3000,
        'cpu_compat': 'qcom,kryo470',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1804800000] * 6 + [2304000000] * 2,
        'memory_frequency': 1866000000,
        'bus_frequency': 100000000,
    },
    'guacamole': {
        # OnePlus 7 Pro (guacamole, SM8150). Board facts from the LineageOS
        # oneplus_sm8150 DTS and the mainline sm8150-oneplus-guacamole.dts;
        # audited against the GM1910's own live FDT.
        'name': 'OnePlus 7 Pro (guacamole, SM8150)',
        'board_compat': 'oneplus,guacamole',
        'soc_compat': 'qcom,sm8150',
        'phys_base': 0xc0000000,
        'mem_size': 0xc0000000,
        'high_phys_base': 0x180000000,
        'high_mem_size': 0x100000000,
        'gicd': 0x17a00000,          # confirmed on the live FDT
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,          # confirmed
        'gicr_size': 0x100000,
        'timebase': 19200000,        # confirmed (clock-frequency)
        'vtimer_intid': 19,          # confirmed (GIC_PPI 3 numbering)
        # reserved-memory cont_splash_region: 0x9C000000, 0x02400000 (36 MiB).
        'fb': 0x9c000000,
        'fb_size': 0x2400000,
        'fb_w': 1440,
        'fb_h': 3120,
        'fb_stride': 1440 * 4,
        'ufs': 0x01d84000,           # confirmed (ICE at 0x1d90000 too)
        'ufs_size': 0x3000,
        'cpu_compat': 'qcom,kryo485',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1785600000] * 4 + [2419200000] * 3 + [2841600000],
        'memory_frequency': 2133000000,
        'bus_frequency': 100000000,
    },
    'raphael': {
        # Redmi K20 Pro / Mi 9T Pro (raphael, SM8150) -- SAME SoC as guacamole.
        # Board facts from the LineageOS xiaomi_sm8150 DTS (device_xiaomi_raphael)
        # and the previous live-FDT audit (adb feac7571). The SM8150 MMIO ranges
        # (GIC/UFS/timer) are identical to guacamole; only the panel resolution
        # (1080x2340) and the board compatible differ.
        'name': 'Redmi K20 Pro / Mi 9T Pro (raphael, SM8150)',
        'board_compat': 'xiaomi,raphael',
        # The kernel is built with MACHINE_CONFIG=SWEET, whose built-in
        # platform expert only matches IONameMatch 'xiaomi,sweet'. The tree has
        # to carry that string or the platform expert never binds.
        'platform_match': 'xiaomi,sweet',
        'soc_compat': 'qcom,sm8150',
        # SM8150 DDR (audited on the raphael live FDT): reuse the documented
        # 3 GiB mid bank (carve-out free) so QEMU/lowmem runs have a valid window.
        'phys_base': 0xc0000000,
        'mem_size': 0xc0000000,
        'high_phys_base': 0x180000000,
        'high_mem_size': 0x100000000,
        'gicd': 0x17a00000,          # confirmed on the live FDT
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,          # confirmed
        'gicr_size': 0x100000,
        'timebase': 19200000,        # confirmed (clock-frequency)
        'vtimer_intid': 19,          # confirmed (GIC_PPI 3 numbering)
        # reserved-memory cont_splash_region: 0x9C000000, 0x02400000 (36 MiB);
        # large enough for the 1080x2340x4 framebuffer and matches the SoC range.
        'fb': 0x9c000000,
        'fb_size': 0x2400000,
        'fb_w': 1080,
        'fb_h': 2340,
        'fb_stride': 1080 * 4,
        'ufs': 0x01d84000,           # confirmed (ICE at 0x1d90000 too)
        'ufs_size': 0x3000,
        # Kryo 485: 4x A55 (silver) + 3x A76 (gold) + 1x A76 (prime) -- same as guacamole.
        # The compatible must still say 'qcom,kryo470': the SWEET kernel's
        # built-in SweetCPU personality is IONameMatch 'qcom,kryo470'. With
        # 'qcom,kryo485' no driver binds the cpu nubs, max_cpus is never set,
        # and the boot thread sleeps forever in PE_lockdown_iokit() ->
        # ml_wait_max_cpus() right after "SWEET FB" (verified on raphael
        # 2026-11-05: with kryo470 the kernel goes on and USB enumerates).
        'cpu_compat': 'qcom,kryo470',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1785600000] * 4 + [2419200000] * 3 + [2841600000],
        'memory_frequency': 2133000000,
        'bus_frequency': 100000000,
    },
}


def get(name):
    try:
        return DEVICES[name]
    except KeyError:
        raise SystemExit(f'unknown device "{name}"; known: {", ".join(sorted(DEVICES))}')
