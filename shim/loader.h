#pragma once
#include <stdint.h>

struct sde_fb;
/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb, const struct sde_fb *fb);
