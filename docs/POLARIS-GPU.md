# Adreno 630 GPU + Metal on the Mi MIX 2S (experimental)

`xnu/patches/0094-polaris-gpu-adreno630.patch` ports the working Adreno 640
GPU driver of the LG G8S port (ios-betalm, itself from
[ios-flame](https://github.com/NWylynko/ios-flame), Pixel 4) to the
SDM845 / Adreno 630v2. **Not tested on a Mi MIX 2S yet.** With `sweetgpu=1`
off (the default) the kernel behaves as before (CPU drawing).

What it does, in order (`bsd/dev/sweet_gpu.c`, `bsd/dev/sweet_a6xx.c`):

1. CX power and GMU clocks (GCC 0x71004/0x44038/0x7100c/0x71018, GPUCC
   0x5090000: CX GDSC 0x106c, GMU RCG 0x1120 at 19.2 MHz, CX_GMU, CXO).
2. GMU in its own SMMU context bank (kgsl-smmu 0x5040000, SID 5; banks 0-4
   only, never the HYP-owned 5-7), RSCC/PDC microcode (PDC 0xb280000, SEQ
   0xb480000, address offset 0x30080), flat `a630_gmu.bin` into ITCM.
3. Legacy GMU boot: BOOT_SLUMBER OOB (22/30) with the highest GX/MX ARC vote
   from cmd-db, SPTPRAC on, HFI queues (ids 0/4/5), HFI v1: init, version,
   perf table v1, the fixed SDM845 bandwidth table, test.
4. Frequency through the DCVS registers (boot-arg `sweetgpufreq=1..5`:
   257/342/414/520/596 MHz, default 4), GPU_SET OOB (16/24) keeps GX on.
5. GPU context bank (SID 0, mask 0x400: never the secure SIDs 1/2), A630
   hw_init (VBIF QOS, ROQ 0x010000c0, UBWC 2), `a630_sqe.fw`, CP init, zap
   shader through PAS 13 into `gpu_region` 0x8c415000.
6. The panel: every source pipe scanning the boot framebuffer gets a
   4352-byte stride (2D engine pitch), active CTL paths flushed; frames are
   scaled to fit by the GPU and touch follows the fitted rectangle.
7. Metal: SweetAccelerator (Turnip's NativeAdreno ABI) reports chip id
   0x06030001; GPU memory from 0xb0400000-0x100000000 (1276 MB, below XNU's
   RAM, no holes in the live memory node); all GPU addresses below 4 GB
   (A630: ADRENO_QUIRK_4GB_VA), ten 192 MB client windows.

Every step is logged as `SWEET GPU:` and synced to the persistent log while
the GPU comes up (a bad access can reset the SoC without a panic).

## Build and boot

1. Firmware from the phone (TWRP, vendor mounted): `/vendor/firmware/a630_gmu.bin`,
   `a630_sqe.fw`, `a630_zap.mdt`, `a630_zap.b02` into one directory.
   **Never commit them.**
2. Kernel: the xnu workflow (patches 0001-0094).
3. Metal userland: the `sweetmetal2` artifact (SweetMetal2.bundle.tar.gz) of
   the ios-flame / ios-betalm Metal workflow, staged into the iOS root before
   the image is built: `sudo sh tools/polaris-metal-stage.sh STAGE SweetMetal2.bundle.tar.gz`.
4. Payload: `tools/mkpayload.py KERNEL ADT OUT --gpufw FWDIR --cmdline "... sweetgpu=1 sweetmtl=2"`.

Useful first test: `sweetgpu=1 sweetgpufb=0 sweetmtl=0` (GPU up, nothing
uses it yet): the log should reach `SWEET GPU: bring-up done, GPU ready`.
Then `sweetgpufb=1` (the kernel presents with the GPU), then `sweetmtl=2`
(backboardd/apps on Metal).
