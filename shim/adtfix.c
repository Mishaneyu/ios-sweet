/*
 * In-place edits of the Apple Device Tree that mkadt.py built, after the
 * loader copied it behind the kernel: values the shim only knows at boot
 * (RAM size from ABL's /memory, the framebuffer from the MDP) replace
 * mkadt's defaults. Sizes never change, so nothing moves.
 *
 * ADT: node = {u32 nprops, u32 nchildren, props..., children...};
 * prop = {char name[32], u32 size (bit31 = placeholder), value padded to 4}.
 * MMU off: 32-bit aligned accesses only, byte loads for names.
 */
#include <stdint.h>
#include "adtfix.h"

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static int name_eq(uintptr_t field, const char *want)
{
	for (int i = 0; i < 32; i++) {
		char c = *(volatile char *)(field + i);
		if (c != want[i])
			return 0;
		if (!c)
			return 1;
	}
	return 0;
}

/* Value address of 'prop' in the first node named 'node' (depth-first). */
static uintptr_t find(uintptr_t n, const char *node, const char *prop,
		      uint32_t *len, uintptr_t *end)
{
	uint32_t nprops = rd32(n), nchildren = rd32(n + 4);
	uintptr_t p = n + 8, hit = 0, hit_len = 0;
	int is_node = 0;

	for (uint32_t i = 0; i < nprops; i++) {
		uint32_t size = rd32(p + 32) & 0x7fffffff;
		uintptr_t val = p + 36;
		if (name_eq(p, "name") && name_eq(val, node))
			is_node = 1;
		if (name_eq(p, prop)) {
			hit = val;
			hit_len = size;
		}
		p = val + ((size + 3) & ~3U);
	}
	uintptr_t found = 0;
	if (is_node && hit) {
		found = hit;
		*len = hit_len;
	}
	for (uint32_t i = 0; i < nchildren; i++) {
		uintptr_t child_end;
		uint32_t child_len = 0;
		uintptr_t f = find(p, node, prop, &child_len, &child_end);
		if (!found && f) {
			found = f;
			*len = child_len;
		}
		p = child_end;
	}
	*end = p;
	return found;
}

static uintptr_t prop(uintptr_t adt, const char *node, const char *name, uint32_t want_len)
{
	uint32_t len = 0;
	uintptr_t end;
	uintptr_t v = find(adt, node, name, &len, &end);
	return v && len == want_len ? v : 0;
}

int adt_set_u32(uintptr_t adt, const char *node, const char *name, uint32_t value)
{
	uintptr_t v = prop(adt, node, name, 4);
	if (!v)
		return 0;
	wr32(v, value);
	return 1;
}

static void put64(uintptr_t v, uint64_t value)
{
	wr32(v, (uint32_t)value);
	wr32(v + 4, (uint32_t)(value >> 32));
}

int adt_set_u64(uintptr_t adt, const char *node, const char *name, uint64_t value)
{
	uintptr_t v = prop(adt, node, name, 8);
	if (!v)
		return 0;
	put64(v, value);
	return 1;
}

int adt_set_reg(uintptr_t adt, const char *node, uint64_t base, uint64_t size)
{
	uintptr_t v = prop(adt, node, "reg", 16);
	if (!v)
		return 0;
	put64(v, base);
	put64(v + 8, size);
	return 1;
}

uint64_t adt_get_reg_size(uintptr_t adt, const char *node)
{
	uintptr_t v = prop(adt, node, "reg", 16);
	return v ? (uint64_t)rd32(v + 8) | (uint64_t)rd32(v + 12) << 32 : 0;
}
