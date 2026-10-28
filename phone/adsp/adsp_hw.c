/*
 * SPDX-License-Identifier: MIT
 *
 * adsp_hw.c: Userland /dev/pmem access, SMEM v12, RPMh proxy votes, AOSS QMP,
 * SMP2P, TrustZone PAS ADSP ELF loader, apps_smmu S1-bypass stream setup, and
 * TLMM GPIO + open-drain bit-banged I2C for Maxim MAX98927L/R on SM8150.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "adspd.h"

/* ---- /dev/pmem kernel ioctls (bsd/dev/sweet_pmem.c) ---- */

struct pmem_alloc_req {
	uint64_t size;
	uint64_t align;
	uint64_t pa;
};

struct pmem_smc_req {
	uint64_t x[8];
};

struct pmem_rpmh_req {
	char     res[16];
	uint32_t n;
	uint32_t offset[4];
	uint32_t value[4];
};

#define PMEM_ALLOC_IOCTL _IOWR('P', 1, struct pmem_alloc_req)
#define PMEM_FREE_IOCTL  _IOW('P', 2, uint64_t)
#define PMEM_SMC_IOCTL   _IOWR('P', 3, struct pmem_smc_req)
#define PMEM_RPMH_IOCTL  _IOW('P', 5, struct pmem_rpmh_req)

static int pmem_fd = -1;

int
pm_open_dev(void)
{
	if (pmem_fd >= 0) {
		return 0;
	}
	pmem_fd = open("/dev/pmem", O_RDWR | O_CLOEXEC);
	if (pmem_fd < 0) {
		adsp_log("pmem: open /dev/pmem failed: %s", strerror(errno));
		snprintf(g_status.last_error, sizeof(g_status.last_error),
		    "open /dev/pmem: %s", strerror(errno));
		return -1;
	}
	return 0;
}

uint32_t
rd32(uint64_t pa)
{
	uint32_t v = 0;

	if (pread(pmem_fd, &v, sizeof(v), (off_t)pa) != (ssize_t)sizeof(v)) {
		adsp_log("pmem: rd32(0x%llx) failed: %s",
		    (unsigned long long)pa, strerror(errno));
	}
	return v;
}

void
wr32(uint64_t pa, uint32_t v)
{
	if (pwrite(pmem_fd, &v, sizeof(v), (off_t)pa) != (ssize_t)sizeof(v)) {
		adsp_log("pmem: wr32(0x%llx) failed: %s",
		    (unsigned long long)pa, strerror(errno));
	}
}

int
pm_rd(uint64_t pa, void *buf, size_t len)
{
	uint8_t *dst = buf;
	size_t off = 0;

	while (off < len) {
		size_t chunk = len - off > 65536 ? 65536 : len - off;
		ssize_t got = pread(pmem_fd, dst + off, chunk, (off_t)(pa + off));
		if (got <= 0) {
			return -1;
		}
		off += (size_t)got;
	}
	return 0;
}

int
pm_wr(uint64_t pa, const void *buf, size_t len)
{
	const uint8_t *src = buf;
	size_t off = 0;

	while (off < len) {
		size_t chunk = len - off > 65536 ? 65536 : len - off;
		ssize_t put = pwrite(pmem_fd, src + off, chunk, (off_t)(pa + off));
		if (put <= 0) {
			return -1;
		}
		off += (size_t)put;
	}
	return 0;
}

uint64_t
pm_alloc(uint64_t size, uint64_t align)
{
	struct pmem_alloc_req req = { .size = size, .align = align, .pa = 0 };

	if (ioctl(pmem_fd, PMEM_ALLOC_IOCTL, &req) < 0) {
		return 0;
	}
	return req.pa;
}

int
pm_free(uint64_t pa)
{
	if (pa == 0) {
		return 0;
	}
	return ioctl(pmem_fd, PMEM_FREE_IOCTL, &pa) < 0 ? -1 : 0;
}

static void
pm_smc(uint64_t x[8])
{
	struct pmem_smc_req req;

	memcpy(req.x, x, sizeof(req.x));
	if (ioctl(pmem_fd, PMEM_SMC_IOCTL, &req) == 0) {
		memcpy(x, req.x, sizeof(req.x));
	} else {
		x[0] = (uint64_t)-1LL;
	}
}

static int
pm_rpmh(const char *res, uint32_t level_idx)
{
	struct pmem_rpmh_req req;

	memset(&req, 0, sizeof(req));
	strncpy(req.res, res, sizeof(req.res) - 1);
	req.n = 1;
	req.offset[0] = 0;
	req.value[0] = level_idx;
	return ioctl(pmem_fd, PMEM_RPMH_IOCTL, &req) < 0 ? errno : 0;
}

uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

void
delay_ms(unsigned ms)
{
	usleep((useconds_t)ms * 1000u);
}

void
apcs_kick(unsigned bit)
{
	wr32(APCS_IPC_PA, 1u << bit);
}

/* ---- SMEM v12 partition & item access ---- */

#define TCSR_MUTEX_PA(n) (0x01f40000ULL + 0x1000ULL * (n))
#define SMEM_HWLOCK      3u
#define APPS_PROC_ID     1u
#define SMEM_PTABLE_PA   (SMEM_BASE_PA + SMEM_SIZE_BYTES - 0x1000ULL)
#define SMEM_CANARY      0xa5a5u

struct smem_part {
	uint64_t pa;
	uint32_t size;
	uint32_t cacheline;
	uint16_t host0;
	uint16_t host1;
};

static struct smem_part smem_parts[32];
static unsigned smem_nparts;
static int smem_global_part = -1;

static uint16_t
smem_rd16(uint64_t pa)
{
	uint32_t w = rd32(pa & ~3ULL);
	return (uint16_t)(w >> (8u * (pa & 2u)));
}

int
smem_init_adsp(void)
{
	uint32_t magic, count;

	if (pm_open_dev() != 0) {
		return -1;
	}
	if (smem_nparts > 0) {
		return 0;
	}
	magic = rd32(SMEM_PTABLE_PA);
	if (magic != 0x434f5424u) { /* "$TOC" */
		adsp_log("smem: bad $TOC magic 0x%08x", magic);
		return -1;
	}
	count = rd32(SMEM_PTABLE_PA + 8);
	for (uint32_t i = 0; i < count && smem_nparts < 32; i++) {
		uint64_t e = SMEM_PTABLE_PA + 32ULL + 48ULL * i;
		uint32_t off = rd32(e);
		uint32_t size = rd32(e + 4);
		uint32_t hosts = rd32(e + 12);
		uint16_t h0, h1;
		bool is_global;
		struct smem_part *p;

		if (off == 0 || size == 0) {
			continue;
		}
		h0 = (uint16_t)hosts;
		h1 = (uint16_t)(hosts >> 16);
		is_global = (h0 == SMEM_HOST_GLOBAL && h1 == SMEM_HOST_GLOBAL);
		if (!is_global && h0 != SMEM_HOST_APPS && h1 != SMEM_HOST_APPS) {
			continue;
		}
		p = &smem_parts[smem_nparts];
		p->pa = SMEM_BASE_PA + off;
		p->size = size;
		p->host0 = h0;
		p->host1 = h1;
		p->cacheline = rd32(e + 16);
		if (is_global) {
			smem_global_part = (int)smem_nparts;
		}
		smem_nparts++;
	}
	return 0;
}

static struct smem_part *
smem_find_part(unsigned host)
{
	if (host == SMEM_HOST_GLOBAL) {
		return smem_global_part >= 0 ? &smem_parts[smem_global_part] : NULL;
	}
	for (unsigned i = 0; i < smem_nparts; i++) {
		if ((smem_parts[i].host0 == SMEM_HOST_APPS && smem_parts[i].host1 == host) ||
		    (smem_parts[i].host1 == SMEM_HOST_APPS && smem_parts[i].host0 == host)) {
			return &smem_parts[i];
		}
	}
	return NULL;
}

static int
smem_part_get(const struct smem_part *p, unsigned item, uint64_t *pa, uint32_t *size)
{
	uint32_t free_unc = rd32(p->pa + 12);
	uint32_t free_cac = rd32(p->pa + 16);
	uint32_t cl = p->cacheline ? p->cacheline : 64u;

	for (uint64_t e = p->pa + 32ULL; e < p->pa + free_unc;) {
		uint16_t it, pad_data, pad_hdr;
		uint32_t sz;

		if (smem_rd16(e) != SMEM_CANARY) {
			return -2;
		}
		it = smem_rd16(e + 2);
		sz = rd32(e + 4);
		pad_data = smem_rd16(e + 8);
		pad_hdr = smem_rd16(e + 10);
		if (it == item) {
			*pa = e + 16ULL + pad_hdr;
			*size = sz - pad_data;
			return 0;
		}
		e += 16ULL + pad_hdr + sz;
	}
	for (uint64_t e = p->pa + p->size - ((16u + cl - 1u) & ~(cl - 1u));
	     e > p->pa + free_cac;) {
		uint16_t it;
		uint32_t sz;

		if (smem_rd16(e) != SMEM_CANARY) {
			return -2;
		}
		it = smem_rd16(e + 2);
		sz = rd32(e + 4);
		if (it == item) {
			*pa = e - sz;
			*size = sz - smem_rd16(e + 8);
			return 0;
		}
		e -= sz + ((16u + cl - 1u) & ~(cl - 1u));
	}
	return -1;
}

int
smem_get_item(unsigned host, unsigned item, uint64_t *pa, uint32_t *size)
{
	const struct smem_part *p = smem_find_part(host);
	uint64_t toc;

	if (p != NULL && smem_part_get(p, item, pa, size) == 0) {
		return 0;
	}
	if (smem_global_part >= 0 &&
	    smem_part_get(&smem_parts[smem_global_part], item, pa, size) == 0) {
		return 0;
	}
	toc = SMEM_BASE_PA + 16ULL * 4 + 32ULL * 4 + 16ULL + 16ULL * item;
	if (item < 512 && rd32(toc) != 0) {
		*pa = SMEM_BASE_PA + rd32(toc + 4);
		*size = rd32(toc + 8);
		return 0;
	}
	return -1;
}

static int
smem_hwlock(void)
{
	for (int i = 0; i < 100000; i++) {
		wr32(TCSR_MUTEX_PA(SMEM_HWLOCK), APPS_PROC_ID);
		if (rd32(TCSR_MUTEX_PA(SMEM_HWLOCK)) == APPS_PROC_ID) {
			return 0;
		}
	}
	return -1;
}

static void
smem_hwunlock(void)
{
	wr32(TCSR_MUTEX_PA(SMEM_HWLOCK), 0);
}

int
smem_alloc_item(unsigned host, unsigned item, uint32_t size, uint64_t *pa)
{
	static const uint8_t z[4096];
	const struct smem_part *p;
	uint32_t got, free_unc, free_cac, asize;
	uint64_t e;

	if (smem_get_item(host, item, pa, &got) == 0) {
		return 0;
	}
	p = smem_find_part(host);
	if (p == NULL || smem_hwlock() != 0) {
		return -1;
	}
	free_unc = rd32(p->pa + 12);
	free_cac = rd32(p->pa + 16);
	asize = (size + 7u) & ~7u;
	if (free_unc + 16u + asize > free_cac) {
		smem_hwunlock();
		return -1;
	}
	e = p->pa + free_unc;
	wr32(e, SMEM_CANARY | ((uint32_t)item << 16));
	wr32(e + 4, asize);
	wr32(e + 8, asize - size);
	wr32(e + 12, 0);
	for (uint32_t o = 0; o < asize; o += sizeof(z)) {
		uint32_t n = asize - o < sizeof(z) ? asize - o : (uint32_t)sizeof(z);
		(void)pm_wr(e + 16ULL + o, z, n);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(p->pa + 12, free_unc + 16u + asize);
	smem_hwunlock();
	*pa = e + 16ULL;
	return 0;
}

/* ---- RPMh cmd-db ARC proxy voting ---- */

static bool adsp_proxy_votes_held;
static bool has_lcx_res, has_lmx_res;
static uint32_t cx_svs_idx = 2;

static int
cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cULL);
	uint64_t data;

	if (rd32(db + 4) != 0x0c0330dbu) {
		return -1;
	}
	data = db + 4ULL + 4ULL + 8ULL * 16ULL + 4ULL + 4ULL;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8ULL + 16ULL * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffffu, cnt = w1 >> 16;

		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24ULL * e;
			char name[9];
			uint32_t w5;

			for (uint32_t i = 0; i < 8; i++) {
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8u * (i & 3u)));
			}
			name[8] = '\0';
			if (strncmp(name, id, 8) != 0) {
				continue;
			}
			w5 = rd32(ent + 20);
			*len = w5 & 0xffffu;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t
cmddb_aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2ULL * (i & ~1u)) >> (16u * (i & 1u))) & 0xffffu;
}

static int
adsp_vote_top(const char *res, bool *present_out)
{
	uint64_t aux = 0;
	uint32_t len = 0, n, top = 0;

	if (cmddb_aux(res, &aux, &len) != 0 || len < 2) {
		if (present_out != NULL) {
			*present_out = false;
		}
		return -1;
	}
	if (present_out != NULL) {
		*present_out = true;
	}
	n = len / 2;
	for (uint32_t i = 0; i < n; i++) {
		if (cmddb_aux16(aux, i) != 0) {
			top = i;
		}
	}
	(void)pm_rpmh(res, top);
	adsp_log("rpmh: %s -> idx %u (lvl %u)", res, top, cmddb_aux16(aux, top));
	return 0;
}

static void
adsp_acquire_proxy_votes(void)
{
	uint64_t aux = 0;
	uint32_t len = 0;

	(void)adsp_vote_top("lcx.lvl", &has_lcx_res);
	(void)adsp_vote_top("lmx.lvl", &has_lmx_res);

	if (cmddb_aux("cx.lvl", &aux, &len) == 0 && len >= 2) {
		uint32_t n = len / 2;
		uint32_t proxy_idx = n - 1;
		bool svs_found = false, proxy_found = false;

		for (uint32_t i = 0; i < n; i++) {
			uint32_t lv = cmddb_aux16(aux, i);
			if (!svs_found && lv >= 128u) {
				cx_svs_idx = i;
				svs_found = true;
			}
			if (!proxy_found && lv >= 0x181u) {
				proxy_idx = i;
				proxy_found = true;
			}
		}
		(void)pm_rpmh("cx.lvl", proxy_idx);
		adsp_log("rpmh: cx.lvl -> proxy idx %u (svs idx %u)", proxy_idx, cx_svs_idx);
	}
	adsp_proxy_votes_held = true;
	g_status.proxy_votes_released = false;
}

static void
adsp_release_proxy_votes(void)
{
	if (!adsp_proxy_votes_held) {
		return;
	}
	adsp_proxy_votes_held = false;
	if (has_lcx_res) {
		(void)pm_rpmh("lcx.lvl", 0);
	}
	if (has_lmx_res) {
		(void)pm_rpmh("lmx.lvl", 0);
	}
	(void)pm_rpmh("cx.lvl", cx_svs_idx);
	g_status.proxy_votes_released = true;
	adsp_log("rpmh: released LPASS proxy votes (cx.lvl -> svs[%u])", cx_svs_idx);
}

/* ---- AOSS QMP mailbox (0x0c300000) ---- */

#define QMP_RAM_PA 0x0c300000ULL
#define QMP_UP_VAL 0x0000ffffu

static int
wait_eq32(uint64_t pa, uint32_t want, int timeout_ms)
{
	for (int i = 0; i < timeout_ms; i++) {
		if (rd32(pa) == want) {
			return 0;
		}
		usleep(1000);
	}
	return -1;
}

static int
qmp_open_link(void)
{
	if (rd32(QMP_RAM_PA) != 0x4d41494cu || rd32(QMP_RAM_PA + 4) != 1u) {
		adsp_log("qmp: unexpected magic 0x%08x", rd32(QMP_RAM_PA));
		return -1;
	}
	if (rd32(QMP_RAM_PA + 0x24) == QMP_UP_VAL &&
	    rd32(QMP_RAM_PA + 0x2c) == QMP_UP_VAL &&
	    rd32(QMP_RAM_PA + 0x30) == QMP_UP_VAL) {
		return 0;
	}
	wr32(QMP_RAM_PA + 0x10, rd32(QMP_RAM_PA + 0x0c));
	wr32(QMP_RAM_PA + 0x24, QMP_UP_VAL);
	apcs_kick(IPC_BIT_AOSS);
	if (wait_eq32(QMP_RAM_PA + 0x28, QMP_UP_VAL, 1000) != 0) {
		return -1;
	}
	wr32(QMP_RAM_PA + 0x2c, QMP_UP_VAL);
	apcs_kick(IPC_BIT_AOSS);
	if (wait_eq32(QMP_RAM_PA + 0x14, QMP_UP_VAL, 1000) != 0) {
		return -1;
	}
	wr32(QMP_RAM_PA + 0x18, QMP_UP_VAL);
	apcs_kick(IPC_BIT_AOSS);
	return wait_eq32(QMP_RAM_PA + 0x30, QMP_UP_VAL, 1000);
}

static int
qmp_send_msg(const char *msg)
{
	uint32_t off, buf[16];
	int rc;

	if (qmp_open_link() != 0) {
		return -1;
	}
	off = rd32(QMP_RAM_PA + 0x38);
	memset(buf, 0, sizeof(buf));
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++) {
		wr32(QMP_RAM_PA + off + 4 + 4 * i, buf[i]);
	}
	wr32(QMP_RAM_PA + off, (uint32_t)sizeof(buf));
	(void)rd32(QMP_RAM_PA + off);
	apcs_kick(IPC_BIT_AOSS);
	rc = wait_eq32(QMP_RAM_PA + off, 0, 1000);
	adsp_log("qmp: '%s' -> %s", msg, rc == 0 ? "acked" : "timeout");
	return rc;
}

/* ---- SMP2P (ADSP outbound 429, inbound 443) ---- */

static uint64_t smp2p_out_pa;
static uint64_t smp2p_in_pa;
static uint64_t slave_kernel_pa;
static uint32_t last_adsp_smp2p = 0xffffffffu;

static void
smp2p_wr_name(uint64_t pa, const char *name)
{
	char n[16];

	memset(n, 0, sizeof(n));
	strncpy(n, name, sizeof(n) - 1);
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + (uint64_t)i, w);
	}
}

static int
smp2p_adsp_create(void)
{
	uint64_t pa = 0;

	if (smem_alloc_item(SMEM_HOST_ADSP, SMP2P_ADSP_OUT_ITEM, 20u + 16u * 20u, &pa) != 0) {
		adsp_log("smp2p: failed to alloc item %u", SMP2P_ADSP_OUT_ITEM);
		return -1;
	}
	if (rd32(pa) != 0x504d5324u) { /* "$SMP" */
		wr32(pa + 8, 0u | (SMEM_HOST_ADSP << 16)); /* local 0 -> remote 2 (ADSP) */
		wr32(pa + 12, 16u | (1u << 16));           /* total 16, valid 1 */
		wr32(pa + 16, 0u);
		smp2p_wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0u);
		wr32(pa + 0, 0x504d5324u);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1u | (1u << 8));              /* v1 + SSR_ACK */
	} else {
		/*
		 * Warm reboot: "$SMP" already exists in SMEM. Ensure "master-kernel"
		 * bit 0 ("qcom,force-stop" per live.dtb) is cleared to 0 BEFORE
		 * PAS_AUTH_AND_RESET so ADSP never halts in STOP_ACK on boot.
		 */
		uint32_t hdr12 = rd32(pa + 12);
		uint32_t total = hdr12 & 0xffffu;
		uint32_t valid = hdr12 >> 16;
		bool found = false;

		for (uint32_t i = 0; i < valid && i < 16; i++) {
			uint64_t e = pa + 20ULL + 20ULL * i;
			char nm[17];
			for (int k = 0; k < 16; k++) {
				nm[k] = (char)(rd32(e + (uint64_t)(k & ~3)) >> (8 * (k & 3)));
			}
			nm[16] = '\0';
			if (strcmp(nm, "master-kernel") == 0) {
				wr32(e + 16ULL, 0u);
				found = true;
				break;
			}
		}
		if (!found && valid < total && valid < 16) {
			uint64_t e = pa + 20ULL + 20ULL * valid;
			smp2p_wr_name(e, "master-kernel");
			wr32(e + 16ULL, 0u);
			__asm__ volatile("dsb sy" ::: "memory");
			wr32(pa + 12, total | ((valid + 1u) << 16));
		}
		__asm__ volatile("dsb sy" ::: "memory");
	}
	smp2p_out_pa = pa;
	apcs_kick(IPC_BIT_ADSP_SMP2P);
	return 0;
}

static int
smp2p_find_in_entry(const char *name, uint64_t *val_pa)
{
	uint64_t pa = 0;
	uint32_t sz = 0, n;

	if (smem_get_item(SMEM_HOST_ADSP, SMP2P_ADSP_IN_ITEM, &pa, &sz) != 0) {
		return -1;
	}
	smp2p_in_pa = pa;
	n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20ULL + 20ULL * i;
		char nm[17];

		for (int k = 0; k < 16; k++) {
			nm[k] = (char)(rd32(e + (uint64_t)(k & ~3)) >> (8 * (k & 3)));
		}
		nm[16] = '\0';
		if (strcmp(nm, name) == 0) {
			*val_pa = e + 16ULL;
			return 0;
		}
	}
	return -1;
}

void
smp2p_adsp_service(void)
{
	uint32_t sz = 0, v;

	if (smp2p_out_pa == 0 &&
	    smem_get_item(SMEM_HOST_ADSP, SMP2P_ADSP_OUT_ITEM, &smp2p_out_pa, &sz) != 0) {
		return;
	}
	if (smp2p_in_pa == 0 &&
	    smem_get_item(SMEM_HOST_ADSP, SMP2P_ADSP_IN_ITEM, &smp2p_in_pa, &sz) != 0) {
		return;
	}
	if (smp2p_in_pa != 0 && smp2p_out_pa != 0) {
		uint32_t want = (rd32(smp2p_in_pa + 16) & 1u) ? 2u : 0u;
		uint32_t cur = rd32(smp2p_out_pa + 16);
		if ((cur & 2u) != want) {
			wr32(smp2p_out_pa + 16, (cur & ~2u) | want);
			apcs_kick(IPC_BIT_ADSP_SMP2P);
			adsp_log("smp2p: SSR restart_ack -> %u", want ? 1u : 0u);
		}
	}
	if (slave_kernel_pa == 0) {
		(void)smp2p_find_in_entry("slave-kernel", &slave_kernel_pa);
	}
	if (slave_kernel_pa != 0) {
		v = rd32(slave_kernel_pa);
		if (v != last_adsp_smp2p) {
			adsp_log("smp2p: slave-kernel=0x%x%s%s%s%s", v,
			    (v & 1u) ? " ERR_FATAL" : "",
			    (v & 2u) ? " READY" : "",
			    (v & 4u) ? " HANDOVER" : "",
			    (v & 8u) ? " STOP_ACK" : "");
			last_adsp_smp2p = v;
		}
		if ((v & 2u) && !(v & 1u)) {
			g_status.smp2p_ready = true;
		}
		if ((v & 4u) && !(v & 1u)) {
			g_status.smp2p_handover = true;
			adsp_release_proxy_votes();
		}
	}
}

/* ---- TrustZone PAS ADSP firmware loader & lifecycle ---- */

#define SCM_PIL_FN(cmd) (0x42000000ULL | (2ULL << 8) | (uint64_t)(cmd))

struct mdt_seg {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
};

static int64_t
pas_scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL_FN(cmd), arginfo, a, b, c, 0, 0, 0 };

	pm_smc(x);
	if (r1 != NULL) {
		*r1 = x[1];
	}
	return (int64_t)x[0];
}

static uint8_t *
slurp_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long sz;
	uint8_t *buf;

	if (f == NULL) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)sz + 4u);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	memset(buf + sz, 0, 4);
	if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_len = (size_t)sz;
	return buf;
}

int
adsp_pas_shutdown(void)
{
	uint64_t r1 = 0;
	int64_t rc;

	if (pm_open_dev() != 0) {
		return -1;
	}
	rc = pas_scm(6, 1, PAS_ADSP, 0, 0, &r1); /* PAS_SHUTDOWN */
	(void)qmp_send_msg("{class: image, res: load_state, name: adsp, val: off}");
	adsp_release_proxy_votes();
	adsp_log("pas: shutdown(PAS_ADSP=1) -> rc=%lld r1=0x%llx",
	    (long long)rc, (unsigned long long)r1);
	return rc == 0 ? 0 : -1;
}

int
adsp_pas_boot(void)
{
	static const uint8_t zeros[65536];
	char mdt_path[128];
	uint8_t *mdt = NULL, *meta = NULL;
	size_t mlen = 0, ehdr_phdr_len, meta_len;
	struct mdt_seg segs[64];
	uint32_t phnum = 0;
	int hash_idx = -1;
	bool relocatable = false;
	uint64_t min_addr = UINT64_MAX, max_addr = 0;
	uint64_t meta_pa = 0, r1 = 0, total_loaded = 0, ve = 0;
	int64_t rc;

	if (smem_init_adsp() != 0) {
		return -1;
	}

	/*
	 * Warm-restart handling: SMEM DRAM survives warm resets from TWRP/ABL
	 * even though the SoC reset already powered down LPASS. Only call
	 * PAS_SHUTDOWN(1) if adspd already booted ADSP in THIS XNU boot
	 * (matching kern.boottime in /var/local/adspd.boot), avoiding a
	 * spurious PAS_SHUTDOWN while MPSS is running.
	 */
	{
		struct timeval bt = { 0 };
		size_t btsz = sizeof(bt);
		char cur_stamp[32] = "", prev_stamp[32] = "";
		FILE *sf;

		(void)sysctlbyname("kern.boottime", &bt, &btsz, NULL, 0);
		snprintf(cur_stamp, sizeof(cur_stamp), "%ld\n", (long)bt.tv_sec);
		sf = fopen("/var/local/adspd.boot", "r");
		if (sf != NULL) {
			if (fgets(prev_stamp, sizeof(prev_stamp), sf) == NULL) {
				prev_stamp[0] = '\0';
			}
			fclose(sf);
		}
		if (bt.tv_sec != 0 && strcmp(cur_stamp, prev_stamp) == 0 &&
		    smp2p_find_in_entry("slave-kernel", &ve) == 0) {
			uint32_t prev = rd32(ve);
			if (prev & 2u) {
				adsp_log("pas: ADSP already booted in this XNU session (slave-kernel=0x%x), restarting", prev);
				(void)adsp_pas_shutdown();
				delay_ms(200);
			}
		}
		sf = fopen("/var/local/adspd.boot", "w");
		if (sf != NULL) {
			fputs(cur_stamp, sf);
			fclose(sf);
		}
	}

	if (smp2p_adsp_create() != 0 || adsp_glink_prepare() != 0) {
		return -1;
	}
	adsp_acquire_proxy_votes();

	snprintf(mdt_path, sizeof(mdt_path), "%s/adsp.mdt", ADSP_FW_DIR);
	mdt = slurp_file(mdt_path, &mlen);
	if (mdt == NULL || mlen < 0x34 || memcmp(mdt, "\x7f" "ELF", 4) != 0) {
		adsp_log("pas: missing or invalid %s", mdt_path);
		snprintf(g_status.last_error, sizeof(g_status.last_error), "missing %s", mdt_path);
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}

	if (mdt[4] == 1) { /* ELF32 */
		uint32_t phoff32;
		uint16_t phentsz, phnum16;

		memcpy(&phoff32, mdt + 0x1c, 4);
		memcpy(&phentsz, mdt + 0x2a, 2);
		memcpy(&phnum16, mdt + 0x2c, 2);
		phnum = phnum16;
		if (phnum == 0 || phnum > 64 || phentsz < 32 ||
		    phoff32 + (size_t)phnum * phentsz > mlen) {
			adsp_log("pas: malformed ELF32 phdrs");
			free(mdt);
			adsp_release_proxy_votes();
			return -1;
		}
		ehdr_phdr_len = phoff32 + (size_t)phnum * phentsz;
		for (uint32_t i = 0; i < phnum; i++) {
			const uint8_t *p = mdt + phoff32 + (size_t)i * phentsz;
			uint32_t off32, pa32, fsz32, msz32;

			memcpy(&segs[i].type, p + 0, 4);
			memcpy(&off32, p + 4, 4);
			memcpy(&pa32, p + 12, 4);
			memcpy(&fsz32, p + 16, 4);
			memcpy(&msz32, p + 20, 4);
			memcpy(&segs[i].flags, p + 24, 4);
			segs[i].offset = off32;
			segs[i].paddr = pa32;
			segs[i].filesz = fsz32;
			segs[i].memsz = msz32;
		}
	} else if (mdt[4] == 2 && mlen >= 0x40) { /* ELF64 */
		uint64_t phoff64;
		uint16_t phentsz, phnum16;

		memcpy(&phoff64, mdt + 0x20, 8);
		memcpy(&phentsz, mdt + 0x36, 2);
		memcpy(&phnum16, mdt + 0x38, 2);
		phnum = phnum16;
		if (phnum == 0 || phnum > 64 || phentsz < 56 ||
		    phoff64 + (size_t)phnum * phentsz > mlen) {
			adsp_log("pas: malformed ELF64 phdrs");
			free(mdt);
			adsp_release_proxy_votes();
			return -1;
		}
		ehdr_phdr_len = (size_t)phoff64 + (size_t)phnum * phentsz;
		for (uint32_t i = 0; i < phnum; i++) {
			const uint8_t *p = mdt + phoff64 + (size_t)i * phentsz;

			memcpy(&segs[i].type, p + 0, 4);
			memcpy(&segs[i].flags, p + 4, 4);
			memcpy(&segs[i].offset, p + 8, 8);
			memcpy(&segs[i].paddr, p + 24, 8);
			memcpy(&segs[i].filesz, p + 32, 8);
			memcpy(&segs[i].memsz, p + 40, 8);
		}
	} else {
		adsp_log("pas: unsupported ELF class %u", mdt[4]);
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}

	for (uint32_t i = 0; i < phnum; i++) {
		if (((segs[i].flags >> 24) & 7u) == 2u) {
			hash_idx = (int)i;
			continue;
		}
		if (segs[i].type != 1u || segs[i].memsz == 0) {
			continue;
		}
		if (segs[i].flags & (1u << 27)) {
			relocatable = true;
		}
		if (segs[i].paddr < min_addr) {
			min_addr = segs[i].paddr;
		}
		if (segs[i].paddr + segs[i].memsz > max_addr) {
			max_addr = segs[i].paddr + segs[i].memsz;
		}
	}
	if (hash_idx < 0 || min_addr == UINT64_MAX) {
		adsp_log("pas: missing hash or PT_LOAD segments");
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}
	if (min_addr < ADSP_CARVEOUT_PA) {
		relocatable = true;
	}

	meta_len = ehdr_phdr_len + (size_t)segs[hash_idx].filesz;
	meta = calloc(1, meta_len);
	if (meta == NULL) {
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}
	memcpy(meta, mdt, ehdr_phdr_len);
	if (segs[hash_idx].offset + segs[hash_idx].filesz <= mlen) {
		memcpy(meta + ehdr_phdr_len, mdt + segs[hash_idx].offset,
		    (size_t)segs[hash_idx].filesz);
	} else {
		char bpath[128];
		size_t blen = 0;
		uint8_t *hseg;

		snprintf(bpath, sizeof(bpath), "%s/adsp.b%02d", ADSP_FW_DIR, hash_idx);
		hseg = slurp_file(bpath, &blen);
		if (hseg == NULL || blen < segs[hash_idx].filesz) {
			adsp_log("pas: missing hash segment %s", bpath);
			free(hseg);
			free(meta);
			free(mdt);
			adsp_release_proxy_votes();
			return -1;
		}
		memcpy(meta + ehdr_phdr_len, hseg, (size_t)segs[hash_idx].filesz);
		free(hseg);
	}

	meta_pa = pm_alloc(meta_len, 0x1000);
	if (meta_pa == 0) {
		adsp_log("pas: pm_alloc(%zu) for metadata failed", meta_len);
		free(meta);
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}
	(void)pm_wr(meta_pa, meta, meta_len);
	free(meta);

	rc = pas_scm(1, 2ULL | (2ULL << 6), PAS_ADSP, meta_pa, 0, &r1); /* PAS_INIT_IMAGE */
	(void)pm_free(meta_pa);
	adsp_log("pas: init_image (%zu B) -> rc=%lld r1=0x%llx",
	    meta_len, (long long)rc, (unsigned long long)r1);
	if (rc != 0 || r1 != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error),
		    "PAS_INIT_IMAGE failed (%lld)", (long long)rc);
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}

	rc = pas_scm(2, 3ULL, PAS_ADSP, ADSP_CARVEOUT_PA, ADSP_CARVEOUT_SIZE, &r1); /* PAS_MEM_SETUP */
	adsp_log("pas: mem_setup(0x%llx + 0x%llx, reloc=%d, min=0x%llx) -> rc=%lld r1=0x%llx",
	    (unsigned long long)ADSP_CARVEOUT_PA, (unsigned long long)ADSP_CARVEOUT_SIZE,
	    relocatable ? 1 : 0, (unsigned long long)min_addr,
	    (long long)rc, (unsigned long long)r1);
	if (rc != 0 || r1 != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error),
		    "PAS_MEM_SETUP failed (%lld)", (long long)rc);
		free(mdt);
		adsp_release_proxy_votes();
		return -1;
	}

	for (uint32_t i = 0; i < phnum; i++) {
		uint64_t pa, fsz_aligned;

		if (segs[i].type != 1u || (int)i == hash_idx || segs[i].memsz == 0) {
			continue;
		}
		pa = relocatable ?
		    (ADSP_CARVEOUT_PA + (segs[i].paddr - min_addr)) : segs[i].paddr;
		if (pa < ADSP_CARVEOUT_PA ||
		    pa + segs[i].memsz > ADSP_CARVEOUT_PA + ADSP_CARVEOUT_SIZE) {
			adsp_log("pas: seg %u out of carveout (0x%llx + 0x%llx)",
			    i, (unsigned long long)pa, (unsigned long long)segs[i].memsz);
			free(mdt);
			adsp_release_proxy_votes();
			return -1;
		}
		fsz_aligned = (segs[i].filesz + 3ULL) & ~3ULL;
		if (segs[i].filesz > 0) {
			char bpath[128];
			size_t slen = 0;
			uint8_t *sbuf;

			snprintf(bpath, sizeof(bpath), "%s/adsp.b%02u", ADSP_FW_DIR, i);
			sbuf = slurp_file(bpath, &slen);
			if (sbuf != NULL && slen >= segs[i].filesz) {
				(void)pm_wr(pa, sbuf, (size_t)fsz_aligned);
				free(sbuf);
			} else {
				free(sbuf);
				if (segs[i].offset + segs[i].filesz > mlen) {
					adsp_log("pas: missing %s", bpath);
					free(mdt);
					adsp_release_proxy_votes();
					return -1;
				}
				(void)pm_wr(pa, mdt + segs[i].offset, (size_t)fsz_aligned);
			}
			total_loaded += segs[i].filesz;
		}
		for (uint64_t o = fsz_aligned; o < segs[i].memsz; o += sizeof(zeros)) {
			uint64_t rem = segs[i].memsz - o;
			uint64_t chunk = rem < sizeof(zeros) ? rem : sizeof(zeros);
			(void)pm_wr(pa + o, zeros, (size_t)((chunk + 3ULL) & ~3ULL));
		}
	}
	free(mdt);
	adsp_log("pas: loaded %llu bytes into carveout 0x%llx",
	    (unsigned long long)total_loaded, (unsigned long long)ADSP_CARVEOUT_PA);

	(void)qmp_send_msg("{class: image, res: load_state, name: adsp, val: on}");
	if (smp2p_find_in_entry("slave-kernel", &ve) == 0) {
		wr32(ve, 0);
	}
	last_adsp_smp2p = 0xffffffffu;
	smp2p_in_pa = 0;
	slave_kernel_pa = 0;
	g_status.smp2p_ready = false;
	g_status.smp2p_handover = false;

	rc = pas_scm(5, 1ULL, PAS_ADSP, 0, 0, &r1); /* PAS_AUTH_AND_RESET */
	adsp_log("pas: auth_and_reset -> rc=%lld r1=0x%llx",
	    (long long)rc, (unsigned long long)r1);
	if (rc != 0 || r1 != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error),
		    "PAS_AUTH_AND_RESET failed (%lld)", (long long)rc);
		adsp_release_proxy_votes();
		return -1;
	}

	for (int t = 0; t < 150 && !g_stop_requested; t++) {
		smp2p_adsp_service();
		if (g_status.smp2p_ready) {
			adsp_log("pas: ADSP SMP2P ready after %d ms", t * 100);
			for (int h = 0; h < 15 && adsp_proxy_votes_held; h++) {
				smp2p_adsp_service();
				delay_ms(20);
			}
			/* Always release proxy votes once ready even if handover bit lags */
			adsp_release_proxy_votes();
			g_status.adsp_booted = true;
			return 0;
		}
		delay_ms(100);
	}

	adsp_log("pas: timeout waiting for ADSP SMP2P ready");
	snprintf(g_status.last_error, sizeof(g_status.last_error), "ADSP SMP2P ready timeout");
	adsp_release_proxy_votes();
	return -1;
}

/* ---- apps_smmu@0x15000000 VMSAv8-64 Stage-1 3-level stream for SID 0x1b21 ---- */

#define GCC_AUDIO_TBU_GDSC_PA 0x0017d040ULL /* hlos1_vote_aggre_noc_mmu_audio_tbu_gdsc */
#define SMMU_CB_CBAR          0x0001f300u   /* S2_BYPASS | MEMATTR=WBWA | BPSHCFG=NSH (arm_smmu.c) */
#define SMMU_CB_CBA2R         0x00000001u   /* VA64 (AArch64 VMSAv8-64) */
/* RES1[23:22] | HUPCF[8] | CFRE[5] | AFE[2] | TRE[1] | M[0]; CFCFG=0, CFIE=0 (arm_smmu.c) */
#define SMMU_CB_SCTLR_S1      0x00c00127u
/* T0SZ=25 (39-bit IA: 3-level L1->L2->L3 walk per qcom,use-3-lvl-tables), NC/NSH walks, 4KB, EPD1=1 */
#define SMMU_CB_TCR_S1        (25u | (1u << 23))
/* MAIR0: Attr0=0x44 (Normal Outer/Inner Non-Cacheable), Attr1=0xff (Normal WBWA) */
#define SMMU_CB_MAIR0_S1      0x0000ff44u

static bool
smmu_sid_matches(uint32_t smr, uint32_t sid, uint32_t mask)
{
	uint32_t id = smr & 0x7fffu;
	uint32_t m = (smr >> 16) & 0x7fffu;

	return ((id ^ sid) & ~(m | mask) & 0x7fffu) == 0;
}

void
adsp_smmu_check_fault(const char *tag)
{
	uint64_t s = APPS_SMMU_PA, cbp;
	uint32_t id1, psize, npage, sgfsr, fsr, fsynr0, far_lo, far_hi;

	if (!g_status.smmu_configured || g_status.smmu_cb < 0) {
		return;
	}
	sgfsr = rd32(s + 0x48);
	if (sgfsr != 0) {
		adsp_log("smmu: %s GLOBAL FAULT sGFSR=0x%08x sGFSYNR0=0x%08x sGFSYNR1=0x%08x",
		    tag ? tag : "check", sgfsr, rd32(s + 0x50), rd32(s + 0x54));
	}
	id1 = rd32(s + 0x24);
	psize = (id1 >> 31) ? 0x10000u : 0x1000u;
	npage = 1u << (((id1 >> 28) & 7u) + 1u);
	cbp = s + (uint64_t)npage * psize + (uint64_t)g_status.smmu_cb * psize;
	fsr = rd32(cbp + 0x58);
	if (fsr != 0) {
		fsynr0 = rd32(cbp + 0x68);
		far_lo = rd32(cbp + 0x60);
		far_hi = rd32(cbp + 0x64);
		adsp_log("smmu: %s CB%d FAULT FSR=0x%08x FSYNR0=0x%08x FAR=0x%x:%08x",
		    tag ? tag : "check", g_status.smmu_cb, fsr, fsynr0, far_hi, far_lo);
		wr32(cbp + 0x58, fsr);
	}
}

int
adsp_smmu_setup(void)
{
	static const uint8_t z[4096];
	uint64_t l1[512], l2[512], l3[512];
	uint64_t s = APPS_SMMU_PA, gr1, cbp;
	uint32_t gdsc, id0, id1, id2, nsmr, ncb, psize, npage, tcr2;
	uint64_t used_cb[4] = { 0 };
	int free_smr = -1, free_cb = -1, existing_smr = -1;

	/* Vote ON adsp_tbu@0x1519d000 GDSC (clear SW_COLLAPSE bit 0) before touching SMMU/DMA */
	gdsc = rd32(GCC_AUDIO_TBU_GDSC_PA);
	if (gdsc & 1u) {
		wr32(GCC_AUDIO_TBU_GDSC_PA, gdsc & ~1u);
		for (int w = 0; w < 200 && !(rd32(GCC_AUDIO_TBU_GDSC_PA) & 0x80000000u); w++) {
			usleep(10);
		}
	}

	id0 = rd32(s + 0x20);
	id1 = rd32(s + 0x24);
	id2 = rd32(s + 0x28);
	nsmr = id0 & 0xffu;
	ncb = id1 & 0xffu;
	psize = (id1 >> 31) ? 0x10000u : 0x1000u;
	npage = 1u << (((id1 >> 28) & 7u) + 1u);

	/* Zero <4 GiB audio DMA pool */
	for (uint64_t o = 0; o < ADSP_DMA_TOTAL_BYTES; o += sizeof(z)) {
		(void)pm_wr(ADSP_DMA_PA + o, z, sizeof(z));
	}

	/*
	 * Build 3-level VMSAv8-64 Stage-1 4 KiB page tables matching RMX1931 live.dtb
	 * (apps-smmu@0x15000000: qcom,qsmmu-v500 + qcom,use-3-lvl-tables -> 39-bit IA,
	 * T0SZ=25) and stock msm_audio_ion (va_start = 0x10000000, sid_bits = 1ULL << 32):
	 *   - L1 (0xC3808000): entries (c << 2) | 0 for c=0..127 -> L2 table (0xC3809000)
	 *     so both 32-bit IOVA 0x10000000 and 36-bit IOVA 0x1_10000000 resolve identically.
	 *   - L2 (0xC3809000): index (0x10000000 >> 21) & 0x1ff == 128 -> L3 table (0xC380A000).
	 *   - L3 (0xC380A000): 512 x 4 KiB Page Descriptors (0x10000000..0x101FFFFF) ->
	 *     physical HLOS DRAM 0xC3800000..0xC39FFFFF (nG=1, AF=1, OSH, AP=RW, Attr0=NC).
	 */
	memset(l1, 0, sizeof(l1));
	memset(l2, 0, sizeof(l2));
	for (unsigned c = 0; c < 128u; c++) {
		l1[(c << 2) | ((ADSP_DMA_IOVA >> 30) & 3u)] = ADSP_SMMU_L2_PA | 3ULL;
	}
	l2[(ADSP_DMA_IOVA >> 21) & 0x1ffu] = ADSP_SMMU_L3_PA | 3ULL;
	for (unsigned p = 0; p < 512u; p++) {
		uint64_t pa = ADSP_DMA_PA + (uint64_t)p * 4096ULL;
		l3[p] = pa | (1ULL << 11) | (1ULL << 10) | (2ULL << 8) | (1ULL << 6) | 3ULL;
	}
	(void)pm_wr(ADSP_SMMU_L3_PA, l3, sizeof(l3));
	(void)pm_wr(ADSP_SMMU_L2_PA, l2, sizeof(l2));
	(void)pm_wr(ADSP_SMMU_PT_PA, l1, sizeof(l1));
	__asm__ volatile("dsb sy" ::: "memory");

	adsp_log("smmu: pre sCR0=0x%08x sGFSR=0x%08x ID0=0x%08x ID1=0x%08x ID2=0x%08x",
	    rd32(s), rd32(s + 0x48), id0, id1, id2);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i);
		uint32_t s2cr = rd32(s + 0xc00 + 4 * i);

		if (!(smr >> 31)) {
			if (free_smr < 0) {
				free_smr = (int)i;
			}
			continue;
		}
		adsp_log("smmu: pre SMR%u=0x%08x S2CR%u=0x%08x", i, smr, i, s2cr);
		if (((s2cr >> 16) & 3u) == 0) {
			uint32_t c = s2cr & 0xffu;
			used_cb[c / 64] |= 1ULL << (c % 64);
		}
		if (smmu_sid_matches(smr, ADSP_AUDIO_SMMU_SID, ADSP_AUDIO_SMMU_MASK)) {
			if (((s2cr >> 16) & 3u) == 0) {
				existing_smr = (int)i;
				free_smr = (int)i;
				free_cb = (int)(s2cr & 0xffu);
			} else if (((smr >> 16) & 0x7fffu) == 0) {
				existing_smr = (int)i;
				free_smr = (int)i;
			} else {
				adsp_log("smmu: SMR%u=0x%08x S2CR%u=0x%08x wide-matches 0x%x; skipping duplicate SMR",
				    i, smr, i, s2cr, ADSP_AUDIO_SMMU_SID);
				g_status.smmu_configured = true;
				g_status.smmu_cb = -1;
				return 0;
			}
		}
	}

	if (free_cb < 0) {
		for (uint32_t c = 0; c < ncb && c < 256; c++) {
			cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
			if (!(used_cb[c / 64] & (1ULL << (c % 64))) &&
			    !(rd32(cbp) & 1u) &&
			    rd32(s + psize + 4 * c) == 0) {
				free_cb = (int)c;
				break;
			}
		}
	}
	if (free_smr < 0 || free_cb < 0) {
		adsp_log("smmu: no free SMR/CB for SID 0x%x (smr=%d cb=%d)",
		    ADSP_AUDIO_SMMU_SID, free_smr, free_cb);
		return -1;
	}

	gr1 = s + psize;
	cbp = s + (uint64_t)npage * psize + (uint64_t)free_cb * psize;
	tcr2 = ((id2 >> 4) & 7u) | 0x00038010u; /* PASize=OAS | TCR2_SEP_UPSTREAM | TCR2_AS */

	wr32(cbp, 0); /* Disable CB while updating translation tables */
	wr32(cbp + 0x58, 0xffffffffu); /* Clear FSR */
	wr32(gr1 + 0x800 + 4 * (uint64_t)free_cb, SMMU_CB_CBA2R);
	wr32(gr1 + 4 * (uint64_t)free_cb, SMMU_CB_CBAR);
	wr32(cbp + 0x10, tcr2);
	wr32(cbp + 0x30, SMMU_CB_TCR_S1);
	wr32(cbp + 0x20, (uint32_t)(ADSP_SMMU_PT_PA & 0xffffffffu));
	wr32(cbp + 0x24, ((uint32_t)(ADSP_SMMU_PT_PA >> 32) & 0xffffu) | ((uint32_t)free_cb << 16));
	wr32(cbp + 0x28, 0);
	wr32(cbp + 0x2c, (uint32_t)free_cb << 16);
	wr32(cbp + 0x38, SMMU_CB_MAIR0_S1);
	wr32(cbp + 0x3c, 0);
	wr32(cbp + 0x610, (uint32_t)free_cb); /* CBn_TLBIASID (0x610, 32-bit) */
	wr32(cbp + 0x7f0, 0);                 /* CBn_TLBSYNC */
	for (int i = 0; i < 50 && (rd32(cbp + 0x7f4) & 1u); i++) {
		usleep(100);
	}
	wr32(cbp, SMMU_CB_SCTLR_S1);
	wr32(s + 0x68, 0); /* sTLBIALLNSNH */
	wr32(s + 0x70, 0); /* sTLBGSYNC */
	for (int i = 0; i < 50 && (rd32(s + 0x74) & 1u); i++) {
		usleep(100);
	}
	wr32(s + 0xc00 + 4 * (uint64_t)free_smr, (uint32_t)free_cb);
	wr32(s + 0x800 + 4 * (uint64_t)free_smr,
	    (1u << 31) | (ADSP_AUDIO_SMMU_MASK << 16) | ADSP_AUDIO_SMMU_SID);
	wr32(s + 0x70, 0); /* sTLBGSYNC */
	for (int i = 0; i < 50 && (rd32(s + 0x74) & 1u); i++) {
		usleep(1000);
	}

	g_status.smmu_configured = true;
	g_status.smmu_cb = free_cb;
	adsp_log("smmu: mapped SID 0x%x (mask 0x%x) -> SMR%d / CB%d (existing=%d IOVA 0x%x -> PA 0x%llx SMR=0x%08x S2CR=0x%08x SCTLR=0x%08x TCR=0x%08x TCR2=0x%08x GDSC=0x%08x)",
	    ADSP_AUDIO_SMMU_SID, ADSP_AUDIO_SMMU_MASK, free_smr, free_cb, existing_smr,
	    ADSP_DMA_IOVA, (unsigned long long)ADSP_DMA_PA,
	    rd32(s + 0x800 + 4 * (uint64_t)free_smr), rd32(s + 0xc00 + 4 * (uint64_t)free_smr),
	    rd32(cbp), rd32(cbp + 0x30), rd32(cbp + 0x10), rd32(GCC_AUDIO_TBU_GDSC_PA));
	return 0;
}

int
adsp_hyp_assign_fastrpc_heap(void)
{
	static const uint8_t z[65536];
	static const uint32_t src_vms[1] = { 3u };               /* VMID_HLOS */
	static const uint32_t dst_vms[4] = { 3u, 15u, 5u, 6u };  /* HLOS, MSS_MSA, SSC_Q6, ADSP_Q6 */
	struct {
		uint32_t vm;
		uint32_t perm;
		uint64_t ctx;
		uint32_t ctx_size;
		uint32_t pad;
	} dst_desc[4];
	struct timeval bt = { 0 };
	size_t btsz = sizeof(bt);
	char cur_stamp[64] = "", prev_stamp[64] = "";
	FILE *sf;
	uint64_t t, mem[2], ext[4], x[8];

	if (pm_open_dev() != 0) {
		return -1;
	}
	(void)sysctlbyname("kern.boottime", &bt, &btsz, NULL, 0);
	snprintf(cur_stamp, sizeof(cur_stamp), "%lld %llx %llx\n",
	    (long long)bt.tv_sec,
	    (unsigned long long)ADSP_FASTRPC_HEAP_PA,
	    (unsigned long long)ADSP_FASTRPC_HEAP_HYP_SIZE);

	sf = fopen("/var/local/adspd.rh", "r");
	if (sf != NULL) {
		if (fgets(prev_stamp, sizeof(prev_stamp), sf) == NULL) {
			prev_stamp[0] = '\0';
		}
		fclose(sf);
	}
	if (bt.tv_sec != 0 && strcmp(cur_stamp, prev_stamp) == 0) {
		adsp_log("fastrpc: remote heap 0x%llx+0x%llx already hyp_assigned this boot",
		    (unsigned long long)ADSP_FASTRPC_HEAP_PA,
		    (unsigned long long)ADSP_FASTRPC_HEAP_HYP_SIZE);
		return 0;
	}

	/* Zero the 2 MiB audiopd remote heap before sharing with ADSP_Q6 */
	for (uint64_t o = 0; o < ADSP_FASTRPC_HEAP_SIZE; o += sizeof(z)) {
		(void)pm_wr(ADSP_FASTRPC_HEAP_PA + o, z, sizeof(z));
	}

	t = pm_alloc(4096, 4096);
	if (t == 0) {
		adsp_log("fastrpc: pm_alloc(4096) for MP_ASSIGN failed");
		return -1;
	}
	mem[0] = ADSP_FASTRPC_HEAP_PA;
	mem[1] = ADSP_FASTRPC_HEAP_HYP_SIZE;
	memset(dst_desc, 0, sizeof(dst_desc));
	for (int i = 0; i < 4; i++) {
		dst_desc[i].vm = dst_vms[i];
		dst_desc[i].perm = 7u; /* PERM_READ | PERM_WRITE | PERM_EXEC (adsprpc.c:4268) */
	}
	ext[0] = sizeof(src_vms);
	ext[1] = t + 128ULL;
	ext[2] = sizeof(dst_desc);
	ext[3] = 0;

	(void)pm_wr(t, mem, sizeof(mem));
	(void)pm_wr(t + 64ULL, src_vms, sizeof(src_vms));
	(void)pm_wr(t + 128ULL, dst_desc, sizeof(dst_desc));
	(void)pm_wr(t + 512ULL, ext, sizeof(ext));

	/* MP_ASSIGN (0x0c/0x16): SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL) */
	memset(x, 0, sizeof(x));
	x[0] = 0x42000000ULL | (0x0cULL << 8) | 0x16ULL;
	x[1] = 7ULL | (1ULL << 4) | (1ULL << 8) | (1ULL << 12);
	x[2] = t;
	x[3] = sizeof(mem);
	x[4] = t + 64ULL;
	x[5] = t + 512ULL;
	pm_smc(x);
	(void)pm_free(t);

	adsp_log("fastrpc: hyp_assign 0x%llx+0x%llx -> {3,15,5,6} RWX rc=%lld",
	    (unsigned long long)ADSP_FASTRPC_HEAP_PA,
	    (unsigned long long)ADSP_FASTRPC_HEAP_HYP_SIZE,
	    (long long)x[0]);
	if (x[0] == 0) {
		sf = fopen("/var/local/adspd.rh", "w");
		if (sf != NULL) {
			fputs(cur_stamp, sf);
			fclose(sf);
		}
		return 0;
	}
	return -1;
}

/* ---- SM8150 TLMM GPIO & Open-Drain Bit-Banged I2C (GPIO51 SDA / GPIO52 SCL) ---- */

#define GPIO_CFG_PA(tile, pin) ((tile) + TLMM_GPIO_STRIDE * (uint64_t)(pin))
#define GPIO_IO_PA(tile, pin)  ((tile) + TLMM_GPIO_STRIDE * (uint64_t)(pin) + 4ULL)

#define TLMM_PULL_NONE 0u
#define TLMM_PULL_UP   3u
#define TLMM_DRV_2MA   0u
#define TLMM_DRV_8MA   3u
#define TLMM_GPIO_OE   (1u << 9)

void
tlmm_setup_quat_mi2s(void)
{
	/* qua_mi2s_sck=137, ws=138, data0(DIN)=139, data1(DOUT)=140 on SOUTH, func 1 */
	static const unsigned pins[4] = {
		GPIO_QUAT_MI2S_SCK,
		GPIO_QUAT_MI2S_WS,
		GPIO_QUAT_MI2S_DATA0,
		GPIO_QUAT_MI2S_DATA1,
	};

	for (int i = 0; i < 4; i++) {
		uint32_t cfg = TLMM_PULL_NONE | (1u << 2) | (TLMM_DRV_8MA << 6);
		wr32(GPIO_CFG_PA(TLMM_SOUTH_PA, pins[i]), cfg);
	}
	adsp_log("tlmm: configured gpio137..140 (SOUTH) -> qua_mi2s func=1 8mA");
}

void
tlmm_gpio_drive(uint64_t tile_pa, unsigned pin, int high)
{
	uint32_t cfg = TLMM_PULL_NONE | (0u << 2) | (TLMM_DRV_2MA << 6) | TLMM_GPIO_OE;

	wr32(GPIO_IO_PA(tile_pa, pin), high ? 2u : 0u);
	wr32(GPIO_CFG_PA(tile_pa, pin), cfg);
	wr32(GPIO_IO_PA(tile_pa, pin), high ? 2u : 0u);
}

/* Open-drain bit-banged I2C on SOUTH GPIO51 (SDA) & GPIO52 (SCL) (~80 kHz) */
#define SDA_CFG_PA GPIO_CFG_PA(TLMM_SOUTH_PA, GPIO_SPK_I2C_SDA)
#define SDA_IO_PA  GPIO_IO_PA(TLMM_SOUTH_PA, GPIO_SPK_I2C_SDA)
#define SCL_CFG_PA GPIO_CFG_PA(TLMM_SOUTH_PA, GPIO_SPK_I2C_SCL)
#define SCL_IO_PA  GPIO_IO_PA(TLMM_SOUTH_PA, GPIO_SPK_I2C_SCL)

#define I2C_OD_FLOAT (TLMM_PULL_UP | (0u << 2) | (TLMM_DRV_2MA << 6))
#define I2C_OD_LOW   (TLMM_PULL_UP | (0u << 2) | (TLMM_DRV_2MA << 6) | TLMM_GPIO_OE)

static uint32_t saved_sda_cfg, saved_scl_cfg;
static bool i2c_saved_valid;

static inline void
i2c_udelay(void)
{
	usleep(6);
}

static inline void
sda_hi(void)
{
	wr32(SDA_CFG_PA, I2C_OD_FLOAT);
}

static inline void
sda_lo(void)
{
	wr32(SDA_IO_PA, 0);
	wr32(SDA_CFG_PA, I2C_OD_LOW);
}

static inline int
sda_in(void)
{
	return (rd32(SDA_IO_PA) & 1u) != 0;
}

static inline void
scl_hi(void)
{
	wr32(SCL_CFG_PA, I2C_OD_FLOAT);
	for (int i = 0; i < 200; i++) {
		if (rd32(SCL_IO_PA) & 1u) {
			break;
		}
		usleep(2);
	}
}

static inline void
scl_lo(void)
{
	wr32(SCL_IO_PA, 0);
	wr32(SCL_CFG_PA, I2C_OD_LOW);
}

static void
i2c_bus_acquire(void)
{
	if (!i2c_saved_valid) {
		saved_sda_cfg = rd32(SDA_CFG_PA);
		saved_scl_cfg = rd32(SCL_CFG_PA);
		i2c_saved_valid = true;
	}
	wr32(SDA_IO_PA, 0);
	wr32(SCL_IO_PA, 0);
	sda_hi();
	scl_hi();
	i2c_udelay();
	if (!sda_in()) {
		for (int i = 0; i < 9 && !sda_in(); i++) {
			scl_lo();
			i2c_udelay();
			scl_hi();
			i2c_udelay();
		}
	}
}

static void
i2c_bus_release(void)
{
	sda_hi();
	scl_hi();
	i2c_udelay();
	if (i2c_saved_valid) {
		wr32(SDA_CFG_PA, saved_sda_cfg);
		wr32(SCL_CFG_PA, saved_scl_cfg);
	}
}

static void
i2c_start_cond(void)
{
	sda_hi();
	scl_hi();
	i2c_udelay();
	sda_lo();
	i2c_udelay();
	scl_lo();
	i2c_udelay();
}

static void
i2c_stop_cond(void)
{
	sda_lo();
	i2c_udelay();
	scl_hi();
	i2c_udelay();
	sda_hi();
	i2c_udelay();
}

static int
i2c_tx_byte(uint8_t b)
{
	int ack;

	for (int i = 7; i >= 0; i--) {
		if ((b >> i) & 1u) {
			sda_hi();
		} else {
			sda_lo();
		}
		i2c_udelay();
		scl_hi();
		i2c_udelay();
		scl_lo();
	}
	sda_hi();
	i2c_udelay();
	scl_hi();
	i2c_udelay();
	ack = !sda_in();
	scl_lo();
	i2c_udelay();
	return ack ? 0 : -1;
}

static uint8_t
i2c_rx_byte(int send_ack)
{
	uint8_t b = 0;

	sda_hi();
	for (int i = 7; i >= 0; i--) {
		i2c_udelay();
		scl_hi();
		i2c_udelay();
		if (sda_in()) {
			b |= (uint8_t)(1u << i);
		}
		scl_lo();
	}
	if (send_ack) {
		sda_lo();
	} else {
		sda_hi();
	}
	i2c_udelay();
	scl_hi();
	i2c_udelay();
	scl_lo();
	sda_hi();
	i2c_udelay();
	return b;
}

int
spk_i2c_probe7(uint8_t addr7)
{
	int rc;

	i2c_bus_acquire();
	i2c_start_cond();
	rc = i2c_tx_byte((uint8_t)(addr7 << 1));
	i2c_stop_cond();
	i2c_bus_release();
	return rc;
}

int
spk_i2c_write_reg16(uint8_t addr7, uint16_t reg, uint8_t val)
{
	int rc = -1;

	i2c_bus_acquire();
	i2c_start_cond();
	if (i2c_tx_byte((uint8_t)(addr7 << 1)) == 0 &&
	    i2c_tx_byte((uint8_t)(reg >> 8)) == 0 &&
	    i2c_tx_byte((uint8_t)(reg & 0xffu)) == 0 &&
	    i2c_tx_byte(val) == 0) {
		rc = 0;
	}
	i2c_stop_cond();
	i2c_bus_release();
	return rc;
}

int
spk_i2c_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *val)
{
	int rc = -1;

	i2c_bus_acquire();
	i2c_start_cond();
	if (i2c_tx_byte((uint8_t)(addr7 << 1)) == 0 &&
	    i2c_tx_byte((uint8_t)(reg >> 8)) == 0 &&
	    i2c_tx_byte((uint8_t)(reg & 0xffu)) == 0) {
		i2c_start_cond();
		if (i2c_tx_byte((uint8_t)((addr7 << 1) | 1u)) == 0) {
			*val = i2c_rx_byte(0);
			rc = 0;
		}
	}
	i2c_stop_cond();
	i2c_bus_release();
	return rc;
}

void
spk_i2c_scan_bus(void)
{
	printf("i2c-scan on SOUTH gpio51(SDA)/gpio52(SCL):\n");
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		if (spk_i2c_probe7(a) == 0) {
			uint8_t rev = 0;
			int rr = spk_i2c_read_reg16(a, 0x01ff, &rev);
			printf("  found 0x%02x (reg 0x01ff -> %s 0x%02x)\n",
			    a, rr == 0 ? "ok" : "err", rev);
		}
	}
}
