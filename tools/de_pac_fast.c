/*
 * Fast C core for tools/de-pac-arm64e.py: in-place ARMv8.3-A arm64e non-hint
 * PAC -> ARMv8.0-A opcode rewriter over validated executable byte ranges.
 *
 * Only non-data-in-code ranges from sections marked S_ATTR_PURE_INSTRUCTIONS
 * or S_ATTR_SOME_INSTRUCTIONS are passed into de_pac_ranges().
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	STAT_RETA         = 0, /* retaa / retab -> ret */
	STAT_BRA          = 1, /* braaz / brabz / braa / brab -> br Xn */
	STAT_BLRA         = 2, /* blraaz / blrabz / blraa / blrab -> blr Xn */
	STAT_PAC_AUT_XPAC = 3, /* pacia..autdb, paciza..autdzb, xpaci, xpacd -> nop */
	STAT_LDRA         = 4, /* ldraa / ldrab (W=0, S=0) -> ldr Xt, [Xn, #uimm] */
	STAT_PACGA_ZEROED = 5, /* pacga -> mov Xd, xzr (when zero_pacga != 0) */
	STAT_LDRA_SLEH    = 6, /* untouched ldraa / ldrab (S=1 or W=1) for XNU sleh */
	STAT_PACGA_SLEH   = 7, /* untouched pacga for XNU sleh */
	STAT_COUNT        = 8
};

static inline uint32_t load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void store_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline uint32_t rewrite_insn(uint32_t insn, int zero_pacga, uint64_t *stats, int *changed)
{
	/* retaa (0xd65f0bff) / retab (0xd65f0fff) -> ret (0xd65f03c0) */
	if ((insn & 0xfffffbffu) == 0xd65f0bffu) {
		stats[STAT_RETA]++;
		*changed = 1;
		return 0xd65f03c0u;
	}

	/* braaz / brabz / braa / brab -> br Xn (0xd61f0000 | (Rn << 5)) */
	if ((insn & 0xfffff81fu) == 0xd61f081fu ||
	    (insn & 0xfffff800u) == 0xd71f0800u) {
		stats[STAT_BRA]++;
		*changed = 1;
		return 0xd61f0000u | (insn & 0x000003e0u);
	}

	/* blraaz / blrabz / blraa / blrab -> blr Xn (0xd63f0000 | (Rn << 5)) */
	if ((insn & 0xfffff81fu) == 0xd63f081fu ||
	    (insn & 0xfffff800u) == 0xd73f0800u) {
		stats[STAT_BLRA]++;
		*changed = 1;
		return 0xd63f0000u | (insn & 0x000003e0u);
	}

	/*
	 * Data-processing (1-source) PAC/AUT/XPAC -> nop (0xd503201f):
	 *   op 0..7:   pacia, pacib, pacda, pacdb, autia, autib, autda, autdb
	 *   op 8..15:  paciza, pacizb, pacdza, pacdzb, autiza, autizb, autdza, autdzb (Rn == 31)
	 *   op 16..17: xpaci, xpacd (Rn == 31)
	 */
	if ((insn & 0xffffe000u) == 0xdac10000u ||
	    (insn & 0xffffe3e0u) == 0xdac123e0u ||
	    (insn & 0xfffffbe0u) == 0xdac143e0u) {
		stats[STAT_PAC_AUT_XPAC]++;
		*changed = 1;
		return 0xd503201fu;
	}

	/*
	 * ldraa / ldrab Xt, [Xn, #simm]:
	 *   [31:24] = 0xf8, M = [23], S = [22], [21] = 1, imm9 = [20:12], W = [11], o2 = [10] = 1
	 *   Note: o2 (bit 10) == 1 is essential so ARMv8.1 LSE atomics (ldadd/ldset/swp, bit 10 == 0)
	 *   are never matched. Only rewrite when S == 0 and W == 0.
	 */
	if ((insn & 0xff200400u) == 0xf8200400u) {
		if ((insn & 0x00400800u) == 0u) {
			uint32_t imm9 = (insn >> 12) & 0x1ffu;
			stats[STAT_LDRA]++;
			*changed = 1;
			return 0xf9400000u | (imm9 << 10) | (insn & 0x000003ffu);
		}
		stats[STAT_LDRA_SLEH]++;
		return insn;
	}

	/* pacga Xd, Xn, Xm: leave for XNU sleh unless --zero-pacga is set */
	if ((insn & 0xffe0fc00u) == 0x9ac03000u) {
		if (zero_pacga) {
			stats[STAT_PACGA_ZEROED]++;
			*changed = 1;
			return 0xaa1f03e0u | (insn & 0x0000001fu);
		}
		stats[STAT_PACGA_SLEH]++;
		return insn;
	}

	return insn;
}

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

/*
 * Scan and optionally rewrite a list of (offset, length) byte intervals inside
 * [buf, buf + buf_len). Intervals must already be 4-byte aligned and have
 * LC_DATA_IN_CODE ranges excluded.
 *
 * ranges_off_len points to 2 * nranges uint64_t values: [off0, len0, off1, len1, ...].
 * Accumulates counts into stats_out[8] and returns total rewritten instructions.
 */
EXPORT uint64_t de_pac_ranges(uint8_t *buf, uint64_t buf_len,
                              const uint64_t *ranges_off_len, size_t nranges,
                              int zero_pacga, int dry_run, uint64_t *stats_out)
{
	uint64_t local[STAT_COUNT] = {0};
	uint64_t rewritten = 0;

	for (size_t r = 0; r < nranges; r++) {
		uint64_t off = ranges_off_len[2 * r];
		uint64_t len = ranges_off_len[2 * r + 1];
		if (off >= buf_len)
			continue;
		if (len > buf_len - off)
			len = buf_len - off;
		len &= ~3ULL;

		uint8_t *p = buf + off;
		uint8_t *end = p + len;
		for (; p < end; p += 4) {
			uint32_t insn = load_le32(p);
			/* Quick high-nibble filter: PAC opcodes start with 0x9, 0xd, or 0xf */
			uint32_t top = insn >> 28;
			if (top != 0xd && top != 0xf && top != 0x9)
				continue;
			int changed = 0;
			uint32_t out = rewrite_insn(insn, zero_pacga, local, &changed);
			if (changed) {
				rewritten++;
				if (!dry_run)
					store_le32(p, out);
			}
		}
	}

	if (stats_out) {
		for (int i = 0; i < STAT_COUNT; i++)
			stats_out[i] += local[i];
	}
	return rewritten;
}
