#pragma once
#include <stdint.h>

struct sde_fb;

/* APSS watchdog state the shim found and left (shim.c), for the boot-args. */
struct shim_wdt {
	uint32_t en_before, en_after, bark, bite;
};
extern struct shim_wdt shim_wdt;

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb, const struct sde_fb *fb);
