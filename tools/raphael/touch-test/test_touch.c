/*
 * Host test for bsd/dev/sweet_touch.c of the Bergamota xnu-11215 series with
 * the raphael board patch (GT9886 Normandy on QUP19).
 *
 * The code under test is compiled verbatim (see mk_testcopy.py); only the
 * lowest layer is replaced: touch_rd/touch_wr hit fake MMIO regions and
 * touch_msg talks to a message-level GT9886 model. Kernel services are
 * stubs that log what the driver asked for.
 */
#include "kstub.h"
#include <stdarg.h>
#include <stdlib.h>

static uint32_t touch_rd(vm_offset_t base, uint32_t off);
static void touch_wr(vm_offset_t base, uint32_t off, uint32_t val);
static int touch_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch);

const char *test_board;
#include "sweet_touch_test.c"

/* ------------------------------------------------------------------ log */
#define LOG_MAX 8192
static char logv[LOG_MAX][80];
static unsigned logn;
static unsigned failures, checks;

static void
ev(const char *fmt, ...)
{
	if (logn >= LOG_MAX) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(logv[logn++], sizeof(logv[0]), fmt, ap);
	va_end(ap);
}

static int
find_ev(const char *s, int from)
{
	for (unsigned i = (unsigned)(from < 0 ? 0 : from); i < logn; ++i) {
		if (strcmp(logv[i], s) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static unsigned
count_ev(const char *s)
{
	unsigned n = 0;
	for (unsigned i = 0; i < logn; ++i) {
		n += strcmp(logv[i], s) == 0;
	}
	return n;
}

#define EXPECT(cond, ...) do { \
	++checks; \
	if (!(cond)) { \
		++failures; \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* Events must appear in this order (each after the previous match). */
static void
expect_order(const char *const *seq, unsigned n)
{
	int at = -1;
	for (unsigned i = 0; i < n; ++i) {
		int k = find_ev(seq[i], at + 1);
		EXPECT(k >= 0, "event '%s' missing after position %d", seq[i], at);
		if (k < 0) {
			return;
		}
		at = k;
	}
}

/* ------------------------------------------------------------ boot args */
static struct { const char *name; int val; } bootargs[8];
static unsigned nbootargs;

static void
bootarg(const char *name, int val)
{
	bootargs[nbootargs].name = name;
	bootargs[nbootargs++].val = val;
}

int
PE_parse_boot_argn(const char *arg, void *ptr, int max_len)
{
	for (unsigned i = 0; i < nbootargs; ++i) {
		if (strcmp(arg, bootargs[i].name) == 0 && max_len == 4) {
			memcpy(ptr, &bootargs[i].val, 4);
			return 1;
		}
	}
	return 0;
}

/* --------------------------------------------------------- fake MMIO */
struct region {
	uint32_t pa, size;
	uint8_t *mem;
	char name[8];
};
static struct region regions[4];
static unsigned nregions;

static struct {
	uint32_t rcg;
	bool dfs;
	uint32_t level[8];
	uint32_t rcg_cfg;
	uint32_t sahb_cbcr;
	bool gsi_only;
} hw;

static struct region *find_region(const char *name);

static struct region *
region_of(vm_offset_t base)
{
	for (unsigned i = 0; i < nregions; ++i) {
		if ((vm_offset_t)regions[i].mem == base) {
			return &regions[i];
		}
	}
	return NULL;
}

static uint32_t *
reg32(struct region *r, uint32_t off)
{
	if (!r || off + 4 > r->size) {
		printf("  FATAL: MMIO %s+%#x out of range\n", r ? r->name : "?", off);
		exit(2);
	}
	return (uint32_t *)(r->mem + off);
}

static struct region *
find_region(const char *name)
{
	for (unsigned i = 0; i < nregions; ++i) {
		if (strcmp(regions[i].name, name) == 0) {
			return &regions[i];
		}
	}
	return NULL;
}

vm_offset_t
ml_io_map(vm_offset_t pa, vm_size_t size)
{
	struct region *r = &regions[nregions++];
	r->pa = (uint32_t)pa;
	r->size = (uint32_t)size;
	r->mem = calloc(1, size);
	if (pa == 0x00100000u) {
		strcpy(r->name, "gcc");
		*reg32(r, hw.rcg + 4u) = hw.rcg_cfg;
		*reg32(r, hw.rcg + 0x14u) = hw.dfs ? 1u : 0u;
		for (unsigned l = 0; l < 8; ++l) {
			*reg32(r, hw.rcg + 0x1cu + 4u * l) = hw.level[l];
		}
		if (hw.rcg == 0x1e3a8u) {
			*reg32(r, 0x1e008u) = hw.sahb_cbcr;
		}
	} else if (pa == 0x03d00000u || pa == 0x03900000u) {
		strcpy(r->name, "tlmm");
	} else {
		strcpy(r->name, "se");
		*reg32(r, 0x068) = 3u << 8;                    /* SE_FW: I2C protocol */
		*reg32(r, 0xe24) = (32u << 24) | (16u << 16);  /* HW_TX: 32-bit, depth 16 */
		*reg32(r, 0xe28) = (32u << 24) | (16u << 16);  /* HW_RX */
		*reg32(r, 0x064) = hw.gsi_only ? 1u : 0u;      /* GENI_IF_DISABLE_RO */
	}
	ev("map %s %#x+%#x", r->name, (unsigned)pa, (unsigned)size);
	return (vm_offset_t)r->mem;
}

static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	struct region *r = region_of(base);
	uint32_t *p = reg32(r, off);
	if (strcmp(r->name, "se") == 0 && tb == &touch_raphael && find_region("gcc") &&
	    *reg32(find_region("gcc"), 0x52014) == 0) {
		ev("se read before clocks %#x", off);
	}
	if (strcmp(r->name, "gcc") == 0 && off == hw.rcg) {
		*p &= ~1u; /* RCG update completes at once */
	}
	return *p;
}

static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	struct region *r = region_of(base);
	*reg32(r, off) = val;
	if (strcmp(r->name, "tlmm") == 0) {
		unsigned pin = off / 0x1000u, reg = off % 0x1000u;
		if (reg == 0) {
			ev("cfg %u mux%u pull%u drv%u oe%u", pin, (val >> 2) & 15u, val & 3u,
			    (val >> 6) & 7u, (val >> 9) & 1u);
		} else if (reg == 4) {
			ev("io %u %s", pin, (val & 2u) ? "hi" : "lo");
		}
	} else if (strcmp(r->name, "gcc") == 0) {
		ev("gcc %#x=%#x", off, val);
	} else if (off == 0x7c) {
		ev("se clk_sel=%u", val);
	}
}

/* ------------------------------------------------------- kernel stubs */
void
clock_interval_to_deadline(uint32_t interval, uint64_t scale, uint64_t *deadline)
{
	(void)scale;
	*deadline = interval;
	ev("wait %u", interval);
}
wait_result_t assert_wait_deadline(void *e, int i, uint64_t d) { (void)e; (void)i; (void)d; return 0; }
wait_result_t thread_block(thread_continue_t c) { (void)c; return 0; }
void delay(int usec) { (void)usec; }
thread_t current_thread(void) { return NULL; }
kern_return_t thread_terminate(thread_t t) { (void)t; return 0; }
void thread_deallocate(thread_t t) { (void)t; }
static uint64_t fake_now;
uint64_t mach_absolute_time(void) { return fake_now += 1000; }
void absolutetime_to_nanoseconds(uint64_t a, uint64_t *r) { *r = a; }
void nanoseconds_to_absolutetime(uint64_t n, uint64_t *r) { *r = n; }
static unsigned boosts;
void sweet_input_boost(void) { ++boosts; }
kern_return_t
kernel_thread_start(thread_continue_t fn, void *arg, thread_t *out)
{
	*out = NULL;
	fn(arg, 0); /* run the driver thread to completion */
	return KERN_SUCCESS;
}

static unsigned rpmh_calls;
int
sweet_rpmh_vreg_enable(const char *res, unsigned mv)
{
	++rpmh_calls;
	ev("rpmh %s %u", res, mv);
	return 0;
}

struct hid_ev { int finger, down, x, y; };
static struct hid_ev hid[256];
static unsigned nhid;
void
sweet_hid_touch(int finger, int down, int x, int y)
{
	hid[nhid++] = (struct hid_ev){finger, down, x, y};
	ev("hid %d %s %d,%d", finger, down ? "down" : "up", x, y);
}

/* ------------------------------------------------- GT9886 model (I2C) */
/* Vendor values (cfg_bin + goodix_ts_i2c.c), deliberately NOT the driver's macros. */
#define DEV_EVENT      0x4100u
#define DEV_FIRST      12u
#define DEV_DOZE_CTRL  0x30f0u
#define DEV_DOZE_STAT  0x3100u
#define DEV_VERSION    0x452cu
struct frame { uint8_t b[96]; unsigned len; };
struct pt { unsigned id, x, y; };

static struct {
	bool present;
	uint8_t ver[72];
	uint8_t doze_stat;
	uint16_t ptr;
	const struct frame *frames;
	unsigned nframes, idx, polls, acks, doze_req, doze_on;
	bool advance, ended, runaway;
	unsigned msgs;
} dev;

static struct frame
mkframe(uint8_t status, unsigned n, const struct pt *p, bool corrupt)
{
	struct frame f;
	memset(&f, 0, sizeof(f));
	f.b[0] = status;
	f.b[1] = (uint8_t)n;
	for (unsigned i = 0; i < n; ++i) {
		uint8_t *q = f.b + 2 + 8 * i;
		q[0] = (uint8_t)p[i].id;
		q[1] = (uint8_t)p[i].x;
		q[2] = (uint8_t)(p[i].x >> 8);
		q[3] = (uint8_t)p[i].y;
		q[4] = (uint8_t)(p[i].y >> 8);
		q[5] = 0x10; /* width */
		q[6] = 0x22;
		q[7] = 0x33;
	}
	uint8_t sum = 0;
	for (unsigned i = 0; i < n * 8 + 3; ++i) {
		sum = (uint8_t)(sum + f.b[i]);
	}
	f.b[n * 8 + 3] = (uint8_t)(0u - sum + (corrupt ? 1u : 0u));
	f.len = n * 8 + 4 < 12 ? 12 : n * 8 + 4;
	return f;
}

static void
mkversion(const char *pid, uint8_t sensor, bool bad_sum)
{
	memset(dev.ver, 0, sizeof(dev.ver));
	for (unsigned i = 0; i < 72; ++i) {
		dev.ver[i] = (uint8_t)(i * 7 + 3);
	}
	memcpy(dev.ver + 9, pid, 4);
	dev.ver[17] = 0x01;
	dev.ver[18] = 0x02;
	dev.ver[19] = 0x03;
	dev.ver[20] = 0x04;
	dev.ver[21] = (uint8_t)(0xa0 | sensor); /* upper nibble must be masked */
	uint8_t sum = 0;
	for (unsigned i = 0; i < 71; ++i) {
		sum = (uint8_t)(sum + dev.ver[i]);
	}
	dev.ver[71] = (uint8_t)(0u - sum + (bad_sum ? 1u : 0u));
}

static int
touch_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr,
    bool stretch)
{
	/* Watchdog: a driver that never reaches the scripted frames must not hang the test. */
	if (++dev.msgs > 20000u) {
		if (!dev.runaway) {
			dev.runaway = true;
			ev("runaway");
		}
		st.dead = true;
		return EIO;
	}
	/* Same contract as the real touch_msg_run. */
	if (st.dead || nt > 3 || nr > 90 || (nt && nr) || (!nt && !nr)) {
		ev("i2c bad-args nt=%u nr=%u", nt, nr);
		return EIO;
	}
	if (addr != 0x5d || !dev.present) {
		ev("i2c nack %#x", addr);
		return ENXIO;
	}
	if (nt) {
		if (nt == 2 && stretch) {
			dev.ptr = (uint16_t)((tx[0] << 8) | tx[1]);
			return 0;
		}
		if (nt == 3 && !stretch) {
			uint16_t reg = (uint16_t)((tx[0] << 8) | tx[1]);
			ev("i2c w %#x=%#x", reg, tx[2]);
			if (reg == DEV_DOZE_CTRL && tx[2] == 0xaa) {
				dev.doze_req++;
				dev.doze_stat = 0xaa; /* firmware answers 0xbb one read later */
			} else if (reg == DEV_DOZE_CTRL && tx[2] == 0xcc) {
				dev.doze_on++;
			} else if (reg == DEV_EVENT && tx[2] == 0) {
				dev.acks++;
				dev.advance = true;
			}
			return 0;
		}
		ev("i2c unexpected write nt=%u stretch=%d", nt, stretch);
		return EIO;
	}
	uint16_t reg = dev.ptr;
	memset(rx, 0, nr);
	if (reg == DEV_EVENT) {
		if (dev.advance) {
			dev.idx++;
			dev.advance = false;
		}
		dev.polls++;
		if (dev.idx >= dev.nframes || dev.polls > 500) {
			if (!dev.ended) {
				ev("script end");
			}
			dev.ended = true;
			st.dead = true; /* stop the poll loop after this read */
			return 0;
		}
		const struct frame *f = &dev.frames[dev.idx];
		memcpy(rx, f->b, nr < sizeof(f->b) ? nr : sizeof(f->b));
		ev("poll %u", dev.idx);
		if (!(f->b[0] & 0x80u)) {
			dev.advance = true; /* the driver must not ack this one */
		}
	} else if (reg == DEV_EVENT + DEV_FIRST) {
		const struct frame *f = &dev.frames[dev.idx];
		memcpy(rx, f->b + DEV_FIRST, nr);
		ev("read tail %u", nr);
	} else if (reg == DEV_DOZE_STAT && nr == 1) {
		rx[0] = dev.doze_stat;
		if (dev.doze_stat == 0xaa) {
			dev.doze_stat = 0xbb;
		}
	} else if (reg >= DEV_VERSION && reg + nr <= DEV_VERSION + 72u) {
		memcpy(rx, dev.ver + (reg - DEV_VERSION), nr);
		ev("read version %u", nr);
	} else {
		ev("read %#x len %u", reg, nr);
	}
	return 0;
}

/* ------------------------------------------------------------- driver */
static void
reset_all(void)
{
	for (unsigned i = 0; i < nregions; ++i) {
		free(regions[i].mem);
	}
	nregions = 0;
	memset(&st, 0, sizeof(st));
	memset(&dev, 0, sizeof(dev));
	memset(&hw, 0, sizeof(hw));
	nbootargs = 0;
	logn = 0;
	tb = &touch_sweet;
	test_board = "xiaomi,sweet";
	boosts = 0;
	nhid = 0;
	rpmh_calls = 0;
	bootarg("sweettouchdelay", 0);
}

static void
raphael_hw(bool dfs)
{
	hw.rcg = 0x1e3a8u;
	hw.dfs = dfs;
	/* Boot-firmware DFS table: GPLL0-even levels first, XO/1 at level 2. */
	hw.level[0] = (6u << 8) | 0x1u | (2u << 12);
	hw.level[1] = (6u << 8) | 0x1u | (2u << 12);
	hw.level[2] = 0x1u;
	for (unsigned l = 3; l < 8; ++l) {
		hw.level[l] = (1u << 8) | 0x3u;
	}
	hw.rcg_cfg = (1u << 8) | 0x5u; /* GPLL0, /3: must be replaced when not DFS */
	hw.sahb_cbcr = (1u << 31) | 2u; /* CLK_OFF but HW gated: must be skipped */
}

static uint32_t
pin_cfg(unsigned pin)
{
	return *reg32(find_region("tlmm"), pin * 0x1000u);
}

static uint32_t
pin_io(unsigned pin)
{
	return *reg32(find_region("tlmm"), pin * 0x1000u + 4u);
}

static void
dump_log_on_failure(unsigned before)
{
	EXPECT(!dev.runaway, "driver ran away (%u I2C messages)", dev.msgs);
	if (failures != before) {
		printf("  --- event log (%u) ---\n", logn);
		for (unsigned i = 0; i < logn && i < 220; ++i) {
			printf("  %s\n", logv[i]);
		}
	}
}

static void
expect_hid(const struct hid_ev *want, unsigned n)
{
	EXPECT(nhid == n, "hid events: got %u want %u", nhid, n);
	for (unsigned i = 0; i < n && i < nhid; ++i) {
		EXPECT(hid[i].finger == want[i].finger && hid[i].down == want[i].down &&
		    hid[i].x == want[i].x && hid[i].y == want[i].y,
		    "hid[%u]: got %d %s %d,%d want %d %s %d,%d", i,
		    hid[i].finger, hid[i].down ? "down" : "up", hid[i].x, hid[i].y,
		    want[i].finger, want[i].down ? "down" : "up", want[i].x, want[i].y);
	}
}

/* ---------------------------------------------------------- the tests */
static int
nor_parse(const uint8_t *buf, unsigned n, unsigned flip, uint32_t *mask, int *x, int *y)
{
	st.flip = flip;
	return touch_nor_parse(buf, n, mask, x, y);
}

static void
test_parser(void)
{
	printf("[parser]\n");
	uint32_t mask;
	int x[TOUCH_MAX], y[TOUCH_MAX];
	struct frame f;
	struct pt one = {0, 100, 200};
	tb = &touch_raphael;

	f = mkframe(0x80, 0, NULL, false);
	EXPECT(nor_parse(f.b, 0, 0, &mask, x, y) == 0 && mask == 0, "n=0");

	f = mkframe(0x80, 1, &one, false);
	EXPECT(nor_parse(f.b, 1, 0, &mask, x, y) == 0 && mask == 1 &&
	    x[0] == 100 && y[0] == 200, "n=1 plain");
	EXPECT(nor_parse(f.b, 1, 3, &mask, x, y) == 0 &&
	    x[0] == 979 && y[0] == 2139, "flip xy");
	EXPECT(nor_parse(f.b, 1, 1, &mask, x, y) == 0 &&
	    x[0] == 979 && y[0] == 200, "flip x");
	EXPECT(nor_parse(f.b, 1, 2, &mask, x, y) == 0 &&
	    x[0] == 100 && y[0] == 2139, "flip y");

	f = mkframe(0x80, 1, &one, true);
	EXPECT(nor_parse(f.b, 1, 0, &mask, x, y) == EIO, "bad checksum");

	struct pt dup[2] = {{3, 1, 2}, {3, 5, 6}};
	f = mkframe(0x80, 2, dup, false);
	EXPECT(nor_parse(f.b, 2, 0, &mask, x, y) == EIO, "duplicate id");

	struct pt bigid = {10, 1, 2};
	f = mkframe(0x80, 1, &bigid, false);
	EXPECT(nor_parse(f.b, 1, 0, &mask, x, y) == EIO, "id 10");

	struct pt edge = {9, 1079, 2339};
	f = mkframe(0x80, 1, &edge, false);
	EXPECT(nor_parse(f.b, 1, 3, &mask, x, y) == 0 && mask == (1u << 9) &&
	    x[9] == 0 && y[9] == 0, "max coords + flip");

	struct pt offx = {1, 1080, 5}, offy = {1, 5, 2340};
	f = mkframe(0x80, 1, &offx, false);
	EXPECT(nor_parse(f.b, 1, 0, &mask, x, y) == EIO, "x out of range");
	f = mkframe(0x80, 1, &offy, false);
	EXPECT(nor_parse(f.b, 1, 0, &mask, x, y) == EIO, "y out of range");

	struct pt pen[2] = {{2, 10, 20}, {0x80, 300, 400}};
	f = mkframe(0x80, 2, pen, false);
	EXPECT(nor_parse(f.b, 2, 0, &mask, x, y) == 0 && mask == (1u << 2) &&
	    x[2] == 10 && y[2] == 20, "pen record dropped");

	struct pt flags = {0x41, 7, 8};
	f = mkframe(0x80, 1, &flags, false);
	EXPECT(nor_parse(f.b, 1, 0, &mask, x, y) == 0 && mask == (1u << 1),
	    "id is the low nibble");

	uint8_t big[96] = {0x80, 11};
	EXPECT(nor_parse(big, 11, 0, &mask, x, y) == EIO, "n=11");

	struct pt ten[10];
	for (unsigned i = 0; i < 10; ++i) {
		ten[i] = (struct pt){i, 100 + i, 200 + i};
	}
	f = mkframe(0x80, 10, ten, false);
	EXPECT(nor_parse(f.b, 10, 0, &mask, x, y) == 0 && mask == 0x3ffu &&
	    x[9] == 109 && y[9] == 209, "ten fingers");
}

static struct frame frames[40];

static void
script_main(void)
{
	struct pt a = {0, 100, 200};
	struct pt b2[2] = {{0, 101, 201}, {1, 500, 600}};
	struct pt fp[2] = {{2, 10, 20}, {0x80, 300, 400}};
	struct pt oob = {3, 1080, 5};
	frames[0] = mkframe(0x80, 1, &a, false);   /* down */
	frames[1] = mkframe(0x80, 2, b2, false);   /* move + second finger (tail read) */
	frames[2] = mkframe(0x80, 0, NULL, false); /* all up */
	frames[3] = mkframe(0x80, 1, &a, true);    /* bad checksum: ack, no report */
	frames[4] = mkframe(0x00, 0, NULL, false); /* no new frame: no ack */
	frames[5] = mkframe(0x80, 2, fp, false);   /* finger + pen */
	frames[6] = mkframe(0x80, 1, &oob, false); /* out of range: ack, no report */
	dev.frames = frames;
	dev.nframes = 7;
}

static void
run_raphael(const char *pid, uint8_t sensor, bool bad_sum, bool dfs, int flip_arg)
{
	reset_all();
	raphael_hw(dfs);
	test_board = "xiaomi,raphael";
	if (flip_arg >= 0) {
		bootarg("sweettouchflip", flip_arg);
	}
	dev.present = true;
	mkversion(pid, sensor, bad_sum);
	script_main();
	sweet_touch_init();
}

static void
test_raphael_main(void)
{
	unsigned before = failures;
	printf("[raphael: sensor 0, DFS (XO/1 at level 2)]\n");
	run_raphael("9886", 0, false, true, -1);

	EXPECT(tb == &touch_raphael, "board");
	EXPECT(st.kind == TOUCH_GT9886 && st.sensor == 0 && st.flip == 3,
	    "kind=%d sensor=%u flip=%u", st.kind, st.sensor, st.flip);
	EXPECT(find_ev("map tlmm 0x3d00000+0x7b000", 0) >= 0 &&
	    find_ev("map se 0xc88000+0x4000", 0) >= 0 && find_ev("map gcc 0x100000+0x60000", 0) >= 0,
	    "MMIO windows");
	/* Clocks: only wrapper-2 S_AHB, M_AHB and SE2 are voted; DFS level 2. */
	EXPECT(*reg32(find_region("gcc"), 0x52014) == 0x46u, "vote=%#x",
	    *reg32(find_region("gcc"), 0x52014));
	EXPECT(*reg32(find_region("gcc"), 0x5200c) == 0, "sweet vote register untouched");
	EXPECT(*reg32(find_region("gcc"), hw.rcg + 4u) == hw.rcg_cfg, "DFS: RCG cfg untouched");
	EXPECT(st.clk_sel == 2 && *reg32(find_region("se"), 0x7c) == 2, "clk_sel=%u", st.clk_sel);
	/* Pins. */
	EXPECT(pin_cfg(57) == ((2u << 2)) && pin_cfg(58) == ((2u << 2)),
	    "i2c pins cfg %#x %#x", pin_cfg(57), pin_cfg(58));
	EXPECT(pin_cfg(122) == (3u | (7u << 6)), "irq cfg %#x", pin_cfg(122));
	EXPECT(pin_cfg(54) == (3u | (7u << 6) | 0x200u) && (pin_io(54) & 2u), "reset cfg %#x io %#x",
	    pin_cfg(54), pin_io(54));
	EXPECT(pin_cfg(98) == 0x200u && (pin_io(98) & 2u) && pin_cfg(100) == 0x200u &&
	    (pin_io(100) & 2u), "power pins");
	const unsigned sweet_pins[] = {6, 7, 8, 9, 90};
	for (unsigned i = 0; i < 5; ++i) {
		EXPECT(pin_cfg(sweet_pins[i]) == 0 && pin_io(sweet_pins[i]) == 0,
		    "sweet pin %u touched", sweet_pins[i]);
	}
	EXPECT(rpmh_calls == 0, "no RPMh vote on raphael");
	/* Power-up order and timing. */
	static const char *const order[] = {
		"cfg 122 mux0 pull3 drv7 oe0", "io 54 lo", "cfg 54 mux0 pull3 drv7 oe1",
		"io 98 hi", "io 100 hi", "wait 300",
		"cfg 57 mux2 pull0 drv0 oe0", "cfg 58 mux2 pull0 drv0 oe0",
		"io 54 lo", "wait 2", "io 54 hi", "wait 300",
		"i2c w 0x30f0=0xaa", "read version 72", "poll 0",
	};
	expect_order(order, sizeof(order) / sizeof(order[0]));
	EXPECT(dev.doze_req == 1 && dev.doze_on == 0, "doze req=%u on=%u", dev.doze_req, dev.doze_on);
	/* Reports (sensor 0 => both axes mirrored). */
	static const struct hid_ev want[] = {
		{0, 1, 979, 2139},              /* frame 0 */
		{0, 1, 978, 2138}, {1, 1, 579, 1739}, /* frame 1 */
		{0, 0, 978, 2138}, {1, 0, 579, 1739}, /* frame 2 */
		{2, 1, 1069, 2319},             /* frame 5 (pen dropped) */
		{2, 0, 1069, 2319},             /* errors then release / loop exit */
	};
	expect_hid(want, sizeof(want) / sizeof(want[0]));
	EXPECT(count_ev("read tail 8") == 2, "tail reads for 2-point frames: %u",
	    count_ev("read tail 8"));
	/* Acks: every touch frame (0,1,2,3,5,6), never the status-0 frame. */
	EXPECT(dev.acks == 6, "acks=%u", dev.acks);
	EXPECT(count_ev("i2c bad-args nt=0 nr=0") == 0, "transfer contract");
	int first_se = -1;
	for (unsigned i = 0; i < logn && first_se < 0; ++i) {
		if (strncmp(logv[i], "se read before clocks", 21) == 0) {
			first_se = (int)i;
		}
	}
	EXPECT(first_se < 0, "SE register read before the wrapper 2 clock vote");
	EXPECT(boosts >= 3, "input boost on new contacts (%u)", boosts);
	dump_log_on_failure(before);
}

static void
test_raphael_variants(void)
{
	unsigned before = failures;
	printf("[raphael: sensor 1 => no flip]\n");
	run_raphael("9886", 1, false, true, -1);
	EXPECT(st.flip == 0 && nhid >= 1 && hid[0].x == 100 && hid[0].y == 200,
	    "flip=%u first=%d,%d", st.flip, nhid ? hid[0].x : -1, nhid ? hid[0].y : -1);
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: sweettouchflip=1 overrides sensor 0]\n");
	run_raphael("9886", 0, false, true, 1);
	EXPECT(st.flip == 1 && nhid >= 1 && hid[0].x == 979 && hid[0].y == 200,
	    "flip=%u first=%d,%d", st.flip, nhid ? hid[0].x : -1, nhid ? hid[0].y : -1);
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: not DFS => RCG set to XO/1, clk_sel 0]\n");
	run_raphael("9886", 0, false, false, -1);
	uint32_t cfg = *reg32(find_region("gcc"), hw.rcg + 4u);
	EXPECT((cfg & 0x371fu) == 1u && st.clk_sel == 0 && st.kind == TOUCH_GT9886,
	    "cfg=%#x clk_sel=%u", cfg, st.clk_sel);
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: DFS without an XO/1 level => stop before any pin]\n");
	reset_all();
	raphael_hw(true);
	hw.level[2] = (6u << 8) | 1u;
	test_board = "xiaomi,raphael";
	dev.present = true;
	mkversion("9886", 0, false);
	script_main();
	sweet_touch_init();
	EXPECT(st.kind == 0 && find_ev("cfg 54 mux0 pull3 drv7 oe1", 0) < 0 && nhid == 0,
	    "must not touch pins");
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: wrong PID 9896 => stop, no polling]\n");
	run_raphael("9896", 0, false, true, -1);
	EXPECT(st.kind == 0 && dev.polls == 0 && nhid == 0, "kind=%d polls=%u", st.kind, dev.polls);
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: version checksum bad => stop]\n");
	run_raphael("9886", 0, true, true, -1);
	EXPECT(st.kind == 0 && dev.polls == 0, "kind=%d polls=%u", st.kind, dev.polls);
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: no device (NACK) => stop]\n");
	reset_all();
	raphael_hw(true);
	test_board = "xiaomi,raphael";
	dev.present = false;
	sweet_touch_init();
	EXPECT(st.kind == 0 && nhid == 0 && count_ev("i2c nack 0x5d") >= 2, "nacks=%u",
	    count_ev("i2c nack 0x5d"));
	dump_log_on_failure(before);

	before = failures;
	printf("[raphael: 20 bad frames => doze turned off again]\n");
	reset_all();
	raphael_hw(true);
	test_board = "xiaomi,raphael";
	dev.present = true;
	mkversion("9886", 0, false);
	struct pt a = {0, 100, 200};
	for (unsigned i = 0; i < 25; ++i) {
		frames[i] = mkframe(0x80, 1, &a, true);
	}
	dev.frames = frames;
	dev.nframes = 25;
	sweet_touch_init();
	EXPECT(dev.doze_req == 2 && dev.acks == 25 && nhid == 0, "doze_req=%u acks=%u hid=%u",
	    dev.doze_req, dev.acks, nhid);
	dump_log_on_failure(before);
}

static void
test_raphael_gsi_only(void)
{
	unsigned before = failures;
	printf("[raphael: GSI-only SE => bit-banged probe, stops without a device]\n");
	reset_all();
	raphael_hw(true);
	hw.gsi_only = true;
	test_board = "xiaomi,raphael";
	dev.present = true;
	mkversion("9886", 0, false);
	sweet_touch_init();
	EXPECT(st.bitbang && st.kind == 0 && nhid == 0, "bitbang=%d kind=%d", st.bitbang, st.kind);
	EXPECT(find_ev("cfg 57 mux0 pull0 drv0 oe0", 0) >= 0, "SDA as plain GPIO");
	EXPECT(count_ev("se clk_sel=2") == 0, "no FIFO SE programming");
	dump_log_on_failure(before);
}

static void
test_sweet_unchanged(void)
{
	unsigned before = failures;
	printf("[sweet: default board keeps the upstream sequence]\n");
	reset_all();
	hw.rcg = 0x18148u;
	hw.dfs = false;
	hw.rcg_cfg = 0x105u;
	dev.present = true; /* answers like a Normandy, so the Yellowstone probe must fail */
	mkversion("9886", 0, false);
	sweet_touch_init();
	EXPECT(tb == &touch_sweet, "default board");
	EXPECT(find_ev("map tlmm 0x3900000+0x5b000", 0) >= 0 && find_ev("map se 0xa84000+0x4000", 0) >= 0,
	    "sweet MMIO windows");
	EXPECT(*reg32(find_region("gcc"), 0x5200c) == ((0x0fu << 18) | (1u << 23)), "sweet vote");
	EXPECT(*reg32(find_region("gcc"), 0x52014) == 0, "raphael vote untouched");
	EXPECT(rpmh_calls == 1 && find_ev("rpmh ldoc11 3200", 0) >= 0, "L11C vote");
	static const char *const order[] = {
		"cfg 9 mux0 pull3 drv3 oe0", "io 8 lo", "rpmh ldoc11 3200", "io 90 hi", "wait 10",
		"cfg 6 mux1 pull0 drv0 oe0", "cfg 7 mux1 pull0 drv0 oe0", "io 8 lo", "wait 2",
		"io 8 hi", "wait 300", "read 0x4022 len 32", "read 0x8240 len 32", "i2c nack 0x38",
	};
	expect_order(order, sizeof(order) / sizeof(order[0]));
	EXPECT(st.kind == 0 && nhid == 0, "no IC selected");
	EXPECT(find_ev("cfg 54 mux0 pull3 drv7 oe1", 0) < 0, "raphael pins untouched");
	dump_log_on_failure(before);

	before = failures;
	printf("[beryllium board: raphael pins and vote untouched]\n");
	reset_all();
	test_board = "xiaomi,beryllium";
	hw.rcg = 0x18738u;
	hw.gsi_only = true;   /* beryllium SE14 is GSI-only: bit-banged, no device in the model */
	sweet_touch_init();
	EXPECT(tb == &touch_beryllium, "beryllium board");
	EXPECT(find_ev("map tlmm 0x3900000+0x5b000", 0) >= 0, "beryllium TLMM window");
	EXPECT(find_ev("cfg 54 mux0 pull3 drv7 oe1", 0) < 0 && find_ev("io 98 hi", 0) < 0,
	    "raphael pins untouched");
	EXPECT(*reg32(find_region("gcc"), 0x52014) == 0, "raphael vote untouched");
	dump_log_on_failure(before);
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_parser();
	test_raphael_main();
	test_raphael_variants();
	test_raphael_gsi_only();
	test_sweet_unchanged();
	printf("\n%u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
