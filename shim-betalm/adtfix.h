#pragma once
#include <stdint.h>

/* Overwrite an existing property of the first node with that name. 1 = done. */
int adt_set_u32(uintptr_t adt, const char *node, const char *name, uint32_t value);
int adt_set_u64(uintptr_t adt, const char *node, const char *name, uint64_t value);
/* reg = <u64 base, u64 size> */
int adt_set_reg(uintptr_t adt, const char *node, uint64_t base, uint64_t size);
uint64_t adt_get_reg_size(uintptr_t adt, const char *node);
