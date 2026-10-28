/*
 * SPDX-License-Identifier: MIT
 *
 * max98927.c: Maxim MAX98937L (@0x39, earpiece/top, rst=gpio148 SOUTH) and
 * MAX98937R (@0x3a, bottom speaker, rst=gpio37 NORTH) stereo Class-DG smart
 * amplifier driver for RMX1931 (48 kHz 16-bit I2S on Quaternary MI2S SD1).
 *
 * Hardware-verified on live RMX1931: both smart amps are Maxim MAX98937
 * (reg 0x0600 REV_ID == 0x43) on SOUTH GPIO51/52 (qup4_i2c), clocked directly
 * from Quaternary MI2S BCLK = 1.536 MHz / LRCLK = 48 kHz (no external MCLK).
 */
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include "adspd.h"

#define MAX98937_R0028_PCM_TO_SPK_MONOMIX_A   0x0028
#define MAX98937_R0029_PCM_TO_SPK_MONOMIX_B   0x0029
#define MAX98937_R0036_AMP_VOL_CTRL           0x0036
#define MAX98937_R004B_AMP_EN                 0x004B
#define MAX98937_R0500_GLOBAL_SHDN            0x0500
#define MAX98937_R0600_REV_ID                 0x0600
#define MAX98927_R01FF_REV_ID_FALLBACK        0x01FF

/* Hardware-verified digital volumes on MAX98937 (0x38 = 0 dB speaker, 0x2e = handset receiver) */
#define MAX98937_DIG_VOL_SPK                  0x38
#define MAX98937_DIG_VOL_RCV                  0x2e

static bool cur_voice_call;
static bool cur_speakerphone;

static const struct {
	uint16_t reg;
	uint8_t  val;
} max98937_common_init[] = {
	{ 0x0005, 0x40 },
	{ 0x000a, 0xf0 },
	{ 0x000b, 0x18 },
	{ 0x0010, 0x01 },
	{ 0x0011, 0x02 },
	{ 0x0014, 0x75 },
	{ 0x0015, 0x8c },
	{ 0x0016, 0x08 },
	{ 0x0017, 0x55 },
	{ 0x0018, 0x03 },
	{ 0x001b, 0x00 },
	{ 0x001d, 0xff },
	{ 0x001f, 0x03 },
	{ 0x0023, 0x40 },
	{ 0x0024, 0x1c },
	{ 0x0025, 0x42 },
	{ 0x0026, 0x08 },
	{ 0x0027, 0x82 },
	{ 0x0036, MAX98937_DIG_VOL_SPK },
	{ 0x0037, 0x0f },
	{ 0x0039, 0x80 },
	{ 0x003b, 0x55 },
	{ 0x003c, 0x85 },
	{ 0x003e, 0xf7 },
	{ 0x003f, 0x1c },
	{ 0x0041, 0x40 },
	{ 0x0042, 0x07 },
	{ 0x0044, 0xff },
	{ 0x004e, 0x10 },
	{ 0x004f, 0xff },
	{ 0x0050, 0xff },
	{ 0x0052, 0x02 },
	{ 0x005c, 0x1f },
	{ 0x005e, 0x16 },
	{ 0x0060, 0x0c },
	{ 0x0061, 0x0c },
	{ 0x0062, 0x12 },
	{ 0x0064, 0x0c },
	{ 0x0065, 0x0c },
	{ 0x0066, 0x0e },
	{ 0x0067, 0x03 },
	{ 0x0068, 0x18 },
	{ 0x0069, 0x18 },
	{ 0x006a, 0x0a },
	{ 0x006c, 0x80 },
	{ 0x0073, 0x0f },
	{ 0x0074, 0x80 },
	{ 0x0075, 0x07 },
	{ 0x0076, 0x07 },
	{ 0x0077, 0x01 },
	{ 0x0079, 0x45 },
	{ 0x007a, 0x2b },
	{ 0x007b, 0x04 },
	{ 0x007d, 0x03 },
	{ 0x007f, 0x07 },
	{ 0x0081, 0x03 },
};

static void
max98927_hw_reset_both(void)
{
	/* Active-low hardware reset on MAX98937L (SOUTH gpio148) & MAX98937R (NORTH gpio37) */
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_MAX98927L_RESET, 0);
	tlmm_gpio_drive(TLMM_NORTH_PA, GPIO_MAX98927R_RESET, 0);
	usleep(1500);
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_MAX98927L_RESET, 1);
	tlmm_gpio_drive(TLMM_NORTH_PA, GPIO_MAX98927R_RESET, 1);
	usleep(6000);
}

static void
amp_power_one(uint8_t addr7, bool on)
{
	if (on) {
		(void)spk_i2c_write_reg16(addr7, MAX98937_R004B_AMP_EN, 0x9d);
		(void)spk_i2c_write_reg16(addr7, MAX98937_R0500_GLOBAL_SHDN, 0x01);
	} else {
		(void)spk_i2c_write_reg16(addr7, MAX98937_R0500_GLOBAL_SHDN, 0x00);
		(void)spk_i2c_write_reg16(addr7, MAX98937_R004B_AMP_EN, 0x00);
	}
}

static int
max98927_init_one(uint8_t addr7, bool is_right, uint8_t *rev_out)
{
	uint8_t rev = 0;

	if (spk_i2c_read_reg16(addr7, MAX98937_R0600_REV_ID, &rev) != 0 ||
	    rev == 0x00 || rev == 0xff) {
		if (spk_i2c_read_reg16(addr7, MAX98927_R01FF_REV_ID_FALLBACK, &rev) != 0 ||
		    rev == 0x00 || rev == 0xff) {
			adsp_log("max98937@0x%02x: probe failed (rev=0x%02x)", addr7, rev);
			return -1;
		}
	}
	*rev_out = rev;

	/* Place amplifier cleanly into global shutdown before applying register profile */
	amp_power_one(addr7, false);

	for (size_t i = 0; i < sizeof(max98937_common_init) / sizeof(max98937_common_init[0]); i++) {
		(void)spk_i2c_write_reg16(addr7, max98937_common_init[i].reg,
		    max98937_common_init[i].val);
	}

	/* Per-amplifier slot, Hi-Z, monomix, and boost sync routing (0x39 Left/Top vs 0x3a Right/Bottom) */
	(void)spk_i2c_write_reg16(addr7, 0x001a, is_right ? 0x02 : 0x01);
	(void)spk_i2c_write_reg16(addr7, 0x001c, is_right ? 0xfd : 0xfe);
	(void)spk_i2c_write_reg16(addr7, 0x001e, is_right ? 0x41 : 0x40);
	(void)spk_i2c_write_reg16(addr7, MAX98937_R0028_PCM_TO_SPK_MONOMIX_A,
	    is_right ? 0x81 : 0x80);
	(void)spk_i2c_write_reg16(addr7, MAX98937_R0029_PCM_TO_SPK_MONOMIX_B,
	    is_right ? 0x01 : 0x00);
	(void)spk_i2c_write_reg16(addr7, 0x0040, is_right ? 0x09 : 0x01);

	adsp_log("max98937@0x%02x (%s): initialized (rev=0x%02x, I2S 48kHz 16-bit)",
	    addr7, is_right ? "Bottom/Right" : "Top/Left", rev);
	return 0;
}

int
max98927_init_stereo(void)
{
	max98927_hw_reset_both();
	g_status.amp_left_ok =
	    (max98927_init_one(MAX98927L_I2C_ADDR, false, &g_status.amp_left_rev) == 0);
	g_status.amp_right_ok =
	    (max98927_init_one(MAX98927R_I2C_ADDR, true, &g_status.amp_right_rev) == 0);
	g_status.amp_enabled = false;
	cur_voice_call = false;
	cur_speakerphone = false;
	return (g_status.amp_left_ok || g_status.amp_right_ok) ? 0 : -1;
}

int
max98927_set_enable(bool enable)
{
	bool enable_left = enable;
	bool enable_right = enable && (!cur_voice_call || cur_speakerphone);

	if (g_status.amp_enabled == enable && !cur_voice_call) {
		return 0;
	}
	if (g_status.amp_left_ok) {
		amp_power_one(MAX98927L_I2C_ADDR, enable_left);
	}
	if (g_status.amp_right_ok) {
		amp_power_one(MAX98927R_I2C_ADDR, enable_right);
	}
	g_status.amp_enabled = enable;
	adsp_log("max98937: power -> %s (L=%d R=%d)",
	    enable ? "ON" : "OFF", enable_left ? 1 : 0, enable_right ? 1 : 0);
	adsp_status_write();
	return 0;
}

int
max98927_set_voice_mode(bool voice_call, bool speakerphone)
{
	cur_voice_call = voice_call;
	cur_speakerphone = speakerphone;

	if (voice_call && !speakerphone) {
		/* Handset earpiece mode: Top amp @0x39 plays CH0 at comfortable receiver level, Bottom @0x3a off */
		if (g_status.amp_right_ok) {
			amp_power_one(MAX98927R_I2C_ADDR, false);
		}
		if (g_status.amp_left_ok) {
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98937_R0028_PCM_TO_SPK_MONOMIX_A, 0x80);
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98937_R0036_AMP_VOL_CTRL, MAX98937_DIG_VOL_RCV);
			amp_power_one(MAX98927L_I2C_ADDR, true);
		}
		g_status.amp_enabled = true;
		adsp_status_write();
	} else {
		/* Restore stereo speaker routing (0 dB digital gain) */
		if (g_status.amp_left_ok) {
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98937_R0028_PCM_TO_SPK_MONOMIX_A, 0x80);
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98937_R0036_AMP_VOL_CTRL, MAX98937_DIG_VOL_SPK);
		}
		if (g_status.amp_right_ok) {
			(void)spk_i2c_write_reg16(MAX98927R_I2C_ADDR,
			    MAX98937_R0028_PCM_TO_SPK_MONOMIX_A, 0x81);
			(void)spk_i2c_write_reg16(MAX98927R_I2C_ADDR,
			    MAX98937_R0036_AMP_VOL_CTRL, MAX98937_DIG_VOL_SPK);
		}
		if (voice_call && speakerphone) {
			(void)max98927_set_enable(true);
		}
	}
	return 0;
}
