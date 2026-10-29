#pragma once
#include <stdint.h>

struct sde_fb {
	uintptr_t base;
	uint32_t width, height, stride, bpp, format;
	int sspp, ctl, pp;
	int cmd_mode, autorefresh, refreshing;
};

/* Find ABL's splash pipe and, on a command-mode panel, start autorefresh. */
void sde_handoff(struct sde_fb *fb, int allow_autorefresh);
/* One frame transfer when autorefresh is off. */
void sde_kick(const struct sde_fb *fb);
