/*
 * SM8150 display hand-off for the LG G8S.
 *
 * The G8S panel (RM69299, 1080x2248) is a DSI *command mode* panel: the MDP
 * only sends a frame to the panel's own RAM when software kicks the CTL, so
 * anything written to the splash buffer after ABL's last kick never shows.
 * sweet's panel is video mode and is scanned out continuously, which every
 * console and AppleCLCD in this port assumes.
 *
 * Fix, once, before anything draws: turn on the pingpong block's
 * AUTOREFRESH (the hardware then starts a new frame transfer on every
 * tear-effect pulse from the panel, i.e. continuous refresh at 60 Hz, as the
 * Linux SDE driver does during its own continuous-splash hand-off) and kick
 * the active CTL once. After that the panel behaves like video mode.
 *
 * The framebuffer geometry is read from the source pipe ABL left pointing
 * into the splash region (SSPP SRC0_ADDR / SRC_SIZE / YSTRIDE0 / FORMAT), so
 * nothing about the buffer is hard-coded except the fallback.
 *
 * Register layout: drivers/gpu/drm/msm/sde/sde_hw_{sspp,ctl,pingpong}.c and
 * sm8150-sde.dtsi from the LGE SM8150 kernel. MMU off: 32-bit Device reads.
 */
#include <stdint.h>
#include "board.h"
#include "sde.h"

#define SSPP_SRC_SIZE		0x00
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT		0x30

#define CTL_TOP			0x014
#define CTL_FLUSH		0x018
#define CTL_START		0x01c
#define CTL_INTF_ACTIVE		0x0f4

#define PP_TEAR_CHECK_EN	0x000
#define PP_LINE_COUNT		0x02c
#define PP_AUTOREFRESH_CONFIG	0x030

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void wait_us(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;	/* 19.2 MHz */
	while (ticks() < end)
		;
}

void sde_handoff(struct sde_fb *fb, int allow_autorefresh)
{
	fb->base = SPLASH_BASE;
	fb->width = PANEL_W;
	fb->height = PANEL_H;
	fb->stride = PANEL_W * 4;
	fb->bpp = 4;
	fb->sspp = fb->ctl = fb->pp = -1;
	fb->cmd_mode = fb->autorefresh = fb->refreshing = 0;
	fb->format = 0;

	/* 1. The pipe ABL fetches the splash image with. */
	for (int i = 0; i < SDE_NUM_SSPP; i++) {
		uintptr_t s = MDSS_BASE + SDE_SSPP_OFF(i);
		uint32_t addr = rd32(s + SSPP_SRC0_ADDR);
		uint32_t size = rd32(s + SSPP_SRC_SIZE);
		if (addr < SPLASH_BASE || addr >= SPLASH_BASE + SPLASH_SIZE || !size)
			continue;
		uint32_t w = size & 0xffff, h = size >> 16;
		uint32_t stride = rd32(s + SSPP_SRC_YSTRIDE0) & 0xffff;
		uint32_t fmt = rd32(s + SSPP_SRC_FORMAT);
		uint32_t bpp = ((fmt >> 9) & 3) + 1;
		if (w < 320 || w > 4096 || h < 320 || h > 4096 || stride < w * bpp)
			continue;
		fb->sspp = i;
		fb->format = fmt;
		fb->base = addr;
		fb->width = w;
		fb->height = h;
		fb->stride = stride;
		fb->bpp = bpp;
		break;
	}

	/* 2. Command mode = a pingpong block with the tear check running. */
	for (int i = 0; i < SDE_NUM_PP; i++) {
		if (rd32(MDSS_BASE + SDE_PP_OFF(i) + PP_TEAR_CHECK_EN) & 1) {
			fb->pp = i;
			fb->cmd_mode = 1;
			break;
		}
	}

	/* 3. The CTL path that drives the DSI interface. */
	for (int i = 0; i < SDE_NUM_CTL; i++) {
		uintptr_t c = MDSS_BASE + SDE_CTL_OFF(i);
		if (rd32(c + CTL_INTF_ACTIVE) || rd32(c + CTL_TOP)) {
			fb->ctl = i;
			break;
		}
	}

	if (!fb->cmd_mode || fb->pp < 0)
		return;		/* video mode: already continuous */

	uintptr_t pp = MDSS_BASE + SDE_PP_OFF(fb->pp);
	uint32_t l0 = rd32(pp + PP_LINE_COUNT);
	if (allow_autorefresh) {
		/* Enable, one frame per TE; then start the first transfer. */
		wr32(pp + PP_AUTOREFRESH_CONFIG, (1U << 31) | 1);
		fb->autorefresh = (rd32(pp + PP_AUTOREFRESH_CONFIG) >> 31) & 1;
		if (fb->ctl >= 0)
			wr32(MDSS_BASE + SDE_CTL_OFF(fb->ctl) + CTL_START, 1);
	}
	/* Refresh check: the line counter moves while frames are sent. */
	for (int t = 0; t < 10 && !fb->refreshing; t++) {
		wait_us(5000);
		if (rd32(pp + PP_LINE_COUNT) != l0)
			fb->refreshing = 1;
	}
}

/* Manual kick (no autorefresh): one frame of whatever is in the buffer. */
void sde_kick(const struct sde_fb *fb)
{
	if (fb->cmd_mode && fb->ctl >= 0 && !fb->autorefresh)
		wr32(MDSS_BASE + SDE_CTL_OFF(fb->ctl) + CTL_START, 1);
}
