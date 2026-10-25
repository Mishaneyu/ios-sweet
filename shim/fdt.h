#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* Bytes from 'base' to the end of the /memory range containing it (0: none). */
uint64_t fdt_mem_from(uintptr_t fdt, uint64_t base);
