/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "adtfix.h"
#include "sde.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* Append " text" to the boot-args if it fits (the kernel logs them). */
static void cmdline_append(volatile char *cl, const char *text)
{
	int n = 0;
	while (n < BOOT_LINE_LENGTH && cl[n])
		n++;
	int t = 0;
	while (text[t])
		t++;
	if (n + 1 + t >= BOOT_LINE_LENGTH)
		return;
	cl[n++] = ' ';
	for (int i = 0; i <= t; i++)
		cl[n + i] = text[i];
}

static char *put_hex(char *o, uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int d = 1;
	while (d < 16 && (v >> (4 * d)))
		d++;
	*o++ = '0';
	*o++ = 'x';
	for (int i = d - 1; i >= 0; i--)
		*o++ = hex[(v >> (4 * i)) & 0xf];
	return o;
}

static char *put_str(char *o, const char *s)
{
	while (*s)
		*o++ = *s++;
	return o;
}

const char *loader_boot(uintptr_t dtb, const struct sde_fb *fb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "no SWEETXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	/*
	 * RAM: everything from phys_base to the end of the /memory range ABL
	 * reports (mkadt's dram-size is only the fallback). The panic log stays
	 * carved off the top, as mkadt.py does.
	 */
	uint64_t mem_size = p->mem_size;
	uint64_t avail = fdt_mem_from(dtb, p->phys_base);
	con_hex2("FDTMEM", "hi=", avail >> 32, "lo=", (uint32_t)avail);
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	uint64_t span = avail > p->mem_size ? avail : p->mem_size;
	if (rd_end > p->phys_base && rd_start < p->phys_base + span)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);

	uint64_t plog = adt_get_reg_size(adt_pa, "pram");
	if (avail > plog + (512UL << 20) && plog) {
		mem_size = avail - plog;
		adt_set_u64(adt_pa, "chosen", "dram-size", mem_size);
		adt_set_reg(adt_pa, "pram", p->phys_base + mem_size, plog);
	}
	con_hex2("DRAM", "hi=", mem_size >> 32, "lo=", (uint32_t)mem_size);

	/* The framebuffer sde.c found (or the board fallback). */
	adt_set_reg(adt_pa, "sweet-fbcon", fb->base, (uint64_t)fb->stride * fb->height);
	adt_set_u32(adt_pa, "sweet-fbcon", "width", fb->width);
	adt_set_u32(adt_pa, "sweet-fbcon", "height", fb->height);
	adt_set_u32(adt_pa, "sweet-fbcon", "stride", fb->stride);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = fb->base;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = fb->stride;
	ba->Video.v_width = fb->width;
	ba->Video.v_height = fb->height;
	ba->Video.v_depth = fb->bpp * 8;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = mem_size;
	/* What the shim saw, for the persistent log (no serial port here). */
	char info[256], *o = info;
	o = put_str(o, "shim.fb=");
	o = put_hex(o, fb->base);
	o = put_str(o, ",");
	o = put_hex(o, fb->width);
	o = put_str(o, "x");
	o = put_hex(o, fb->height);
	o = put_str(o, ",");
	o = put_hex(o, fb->stride);
	o = put_str(o, ",fmt");
	o = put_hex(o, fb->format);
	o = put_str(o, " shim.sde=sspp");
	o = put_hex(o, (uint32_t)fb->sspp);
	o = put_str(o, ",ctl");
	o = put_hex(o, (uint32_t)fb->ctl);
	o = put_str(o, ",pp");
	o = put_hex(o, (uint32_t)fb->pp);
	o = put_str(o, fb->cmd_mode ? ",cmd" : ",video");
	o = put_str(o, fb->autorefresh ? ",autoref" : ",noautoref");
	o = put_str(o, fb->refreshing ? ",live" : ",still");
	o = put_str(o, " shim.wdt=");
	o = put_hex(o, shim_wdt.en_before);
	o = put_str(o, ",");
	o = put_hex(o, shim_wdt.en_after);
	o = put_str(o, ",bark");
	o = put_hex(o, shim_wdt.bark);
	o = put_str(o, " shim.dram=");
	o = put_hex(o, mem_size);
	*o = 0;
	cmdline_append(ba->CommandLine, info);
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
