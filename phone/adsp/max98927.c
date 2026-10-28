/*
 * SPDX-License-Identifier: MIT
 *
 * max98927.c: Maxim MAX98927L (@0x39, earpiece/top, rst=gpio148 SOUTH) and
 * MAX98927R (@0x3a, bottom speaker, rst=gpio37 NORTH) stereo Class-DG
 * amplifier driver for RMX1931 (48 kHz 16-bit I2S on Quaternary MI2S SD1).
 */
#include <stdio.h>
#include <unistd.h>

#include "adspd.h"

#define MAX98927_R0018_PCM_RX_EN_A            0x0018
#define MAX98927_R001A_PCM_TX_EN_A            0x001A
#define MAX98927_R001B_PCM_TX_EN_B            0x001B
#define MAX98927_R001C_PCM_TX_HIZ_CTRL_A      0x001C
#define MAX98927_R001D_PCM_TX_HIZ_CTRL_B      0x001D
#define MAX98927_R0020_PCM_MODE_CFG           0x0020
#define MAX98927_R0021_PCM_MASTER_MODE        0x0021
#define MAX98927_R0022_PCM_CLK_SETUP          0x0022
#define MAX98927_R0023_PCM_SR_SETUP1          0x0023
#define MAX98927_R0024_PCM_SR_SETUP2          0x0024
#define MAX98927_R0025_PCM_TO_SPK_MONOMIX_A   0x0025
#define MAX98927_R0026_PCM_TO_SPK_MONOMIX_B   0x0026
#define MAX98927_R0035_PDM_RX_CTRL            0x0035
#define MAX98927_R0036_AMP_VOL_CTRL           0x0036
#define MAX98927_R0037_AMP_DSP_CFG            0x0037
#define MAX98927_R0039_DRE_CTRL               0x0039
#define MAX98927_R003A_AMP_EN                 0x003A
#define MAX98927_R003B_SPK_SRC_SEL            0x003B
#define MAX98927_R003C_SPK_GAIN               0x003C
#define MAX98927_R003E_MEAS_EN                0x003E
#define MAX98927_R003F_MEAS_DSP_CFG           0x003F
#define MAX98927_R0040_BOOST_CTRL0            0x0040
#define MAX98927_R0042_BOOST_CTRL1            0x0042
#define MAX98927_R007F_BROWNOUT_LVL4_AMP1_C1  0x007F
#define MAX98927_R0082_ENV_TRACK_HEADROOM     0x0082
#define MAX98927_R0086_ENV_TRACK_CTRL         0x0086
#define MAX98927_R00FF_GLOBAL_SHDN            0x00FF
#define MAX98927_R0100_SOFT_RESET             0x0100
#define MAX98927_R01FF_REV_ID                 0x01FF

/* Safe default digital volume (~ -1.5 dB below 0 dB = 0x38) + analog gain */
#define AMP_DEFAULT_DIG_VOL                   0x36
#define AMP_SPK_GAIN_12DB                     0x03
#define AMP_RCV_GAIN_6DB                      0x01

static bool cur_voice_call;
static bool cur_speakerphone;

static void
max98927_hw_reset_both(void)
{
	/* Active-low hardware reset on MAX98927L (SOUTH gpio148) & MAX98927R (NORTH gpio37) */
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_MAX98927L_RESET, 0);
	tlmm_gpio_drive(TLMM_NORTH_PA, GPIO_MAX98927R_RESET, 0);
	usleep(1500);
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_MAX98927L_RESET, 1);
	tlmm_gpio_drive(TLMM_NORTH_PA, GPIO_MAX98927R_RESET, 1);
	usleep(6000);
}

static int
max98927_init_one(uint8_t addr7, bool is_right, uint8_t *rev_out)
{
	uint8_t rev = 0;

	if (spk_i2c_read_reg16(addr7, MAX98927_R01FF_REV_ID, &rev) != 0 || rev == 0x00 || rev == 0xff) {
		adsp_log("max98927@0x%02x: probe failed (rev=0x%02x)", addr7, rev);
		return -1;
	}
	*rev_out = rev;

	/* Soft reset then configure while in global shutdown */
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0100_SOFT_RESET, 0x01);
	usleep(2000);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R00FF_GLOBAL_SHDN, 0x00);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R003A_AMP_EN, 0x00);

	/* Keep all PCM TX slots Hi-Z so L and R never contend on SD0 */
	(void)spk_i2c_write_reg16(addr7, MAX98927_R001A_PCM_TX_EN_A, 0x00);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R001B_PCM_TX_EN_B, 0x00);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R001C_PCM_TX_HIZ_CTRL_A, 0xff);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R001D_PCM_TX_HIZ_CTRL_B, 0xff);

	/* 16-bit I2S slave, BSEL=2 (32 BCLK/LRCLK = 1.536 MHz), 48 kHz */
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0020_PCM_MODE_CFG, 0x40);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0021_PCM_MASTER_MODE, 0x00);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0022_PCM_CLK_SETUP, 0x02);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0023_PCM_SR_SETUP1, 0x08);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0024_PCM_SR_SETUP2, 0x88);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0018_PCM_RX_EN_A, 0x03);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0035_PDM_RX_CTRL, 0x00);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R003B_SPK_SRC_SEL, 0x00);

	/* Left (@0x39, top/earpiece) = CH0; Right (@0x3a, bottom) = CH1 */
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0025_PCM_TO_SPK_MONOMIX_A,
	    is_right ? 0x40 : 0x00);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0026_PCM_TO_SPK_MONOMIX_B, 0x01);

	/* DSP DC blocker + volume ramp, DRE, boost & envelope tracker (max98927_probe defaults) */
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0036_AMP_VOL_CTRL, AMP_DEFAULT_DIG_VOL);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0037_AMP_DSP_CFG, 0x03);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0039_DRE_CTRL, 0x01);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R003C_SPK_GAIN, AMP_SPK_GAIN_12DB);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R003E_MEAS_EN, 0x03);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R003F_MEAS_DSP_CFG, 0x0f);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0040_BOOST_CTRL0, 0x1c);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0042_BOOST_CTRL1, 0x3e);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R007F_BROWNOUT_LVL4_AMP1_C1, 0x06);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0082_ENV_TRACK_HEADROOM, 0x08);
	(void)spk_i2c_write_reg16(addr7, MAX98927_R0086_ENV_TRACK_CTRL, 0x01);

	adsp_log("max98927@0x%02x (%s): initialized (rev=0x%02x, I2S 48kHz 16-bit)",
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

static void
amp_power_one(uint8_t addr7, bool on)
{
	if (on) {
		(void)spk_i2c_write_reg16(addr7, MAX98927_R003A_AMP_EN, 0x01);
		(void)spk_i2c_write_reg16(addr7, MAX98927_R00FF_GLOBAL_SHDN, 0x01);
	} else {
		(void)spk_i2c_write_reg16(addr7, MAX98927_R00FF_GLOBAL_SHDN, 0x00);
		(void)spk_i2c_write_reg16(addr7, MAX98927_R003A_AMP_EN, 0x00);
	}
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
	adsp_log("max98927: power -> %s (L=%d R=%d)",
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
		/* Handset earpiece mode: Top amp @0x39 mixes (CH0+CH1)/2 at +6 dB, Bottom @0x3a off */
		if (g_status.amp_right_ok) {
			amp_power_one(MAX98927R_I2C_ADDR, false);
		}
		if (g_status.amp_left_ok) {
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98927_R0025_PCM_TO_SPK_MONOMIX_A, 0x80);
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98927_R003C_SPK_GAIN, AMP_RCV_GAIN_6DB);
			amp_power_one(MAX98927L_I2C_ADDR, true);
		}
		g_status.amp_enabled = true;
	} else {
		/* Restore stereo speaker routing (+12 dB) */
		if (g_status.amp_left_ok) {
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98927_R0025_PCM_TO_SPK_MONOMIX_A, 0x00);
			(void)spk_i2c_write_reg16(MAX98927L_I2C_ADDR,
			    MAX98927_R003C_SPK_GAIN, AMP_SPK_GAIN_12DB);
		}
		if (g_status.amp_right_ok) {
			(void)spk_i2c_write_reg16(MAX98927R_I2C_ADDR,
			    MAX98927_R0025_PCM_TO_SPK_MONOMIX_A, 0x40);
			(void)spk_i2c_write_reg16(MAX98927R_I2C_ADDR,
			    MAX98927_R003C_SPK_GAIN, AMP_SPK_GAIN_12DB);
		}
		if (voice_call && speakerphone) {
			(void)max98927_set_enable(true);
		}
	}
	return 0;
}
