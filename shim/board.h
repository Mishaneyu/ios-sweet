#pragma once
/*
 * LG G8S ThinQ ("betalm", LM-G810, Qualcomm SM8150). Addresses from the
 * LineageOS android_kernel_lge_sm8150 device tree (sm8150.dtsi,
 * sm8150-sde.dtsi, lge/sm8150-beta). GIC, UFS, PSHOLD and the splash
 * region are at the same addresses as on sweet (SM7150).
 */

#define BOARD_NAME		"betalm (LG G8S, SM8150)"

#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem@146bf000 restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL	/* restart@c264000 */

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL	/* 8 x 0x20000 */

#define UFS_BASE		0x01d84000UL	/* ufshc@1d84000 "ufs_mem" */

/* Splash: reserved-memory splash_region 0x9c000000 + 0x2400000. ABL's pipe
 * (VIG0) scans out of FB_FALLBACK inside it, a 1080x2248 a8r8g8b8 buffer
 * (shim photo, 2026-10-25); sde.c reads it from the MDP, this is the fallback. */
#define SPLASH_BASE		0x9c000000UL
#define SPLASH_SIZE		0x02400000UL
#define FB_FALLBACK		0x9d200000UL
#define PANEL_W			1080
#define PANEL_H			2248

/* MDSS / SDE (DPU 5.0) on SM8150, offsets relative to MDSS_BASE. */
#define MDSS_BASE		0x0ae00000UL
#define SDE_NUM_CTL		6
#define SDE_CTL_OFF(i)		(0x2000UL + 0x200UL * (i))
#define SDE_NUM_PP		6
#define SDE_PP_OFF(i)		(0x71000UL + 0x800UL * (i))	/* 0x71000..0x73800 */
#define SDE_NUM_SSPP		8
/* VIG0-3 at 0x5000 + 0x2000*i, DMA0-3 at 0x25000 + 0x2000*i */
#define SDE_SSPP_OFF(i)		((i) < 4 ? 0x5000UL + 0x2000UL * (i) : 0x25000UL + 0x2000UL * ((i) - 4))

/* DDR: low bank 0x80000000, high bank from 0x1_00000000 (6 GB phone). */
#define HIGH_BANK_BASE		0x100000000UL
