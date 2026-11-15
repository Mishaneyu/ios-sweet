/*
 * bgmtl-env.dylib: Chlorophyll switches set from an initializer, for processes that are not started by upstream's
 * bgl-launch wrapper. Linked (LC_LOAD_DYLIB) BEFORE /usr/local/lib/bgmtl/bgmtl-inject.dylib, so this initializer runs
 * before the inject's constructor.
 *
 * backboardd (/usr/libexec/backboardd): the Metal gate of bgl-launch (metal_mode()), then BGMTL_BB=1, which makes the
 * inject register Chlorophyll as the system compositor's Metal device. All of these must hold:
 *   /private/var/tmp/bgmtl-bb.arm exists, /private/var/tmp/bgmtl-bb.off does not, the inject and its two libraries
 *   are readable, and the previous Metal launch of THIS boot (kern.bootsessionuuid) became healthy: bgmtl-bb.state
 *   "alive=0 boot=<this boot>" means it did not, and then bgmtl-bb.off is written and backboardd stays stock.
 *   Otherwise "alive=0 boot=<uuid>" is written here; the inject writes alive=1 10 s after its first completed command
 *   buffer with no failure. The GPU must be up (kern.sweet_gpu state=on); with sweetgpu=8 in the boot-args this waits up
 *   to 20 s for it (at boot backboardd starts while the driver is still bringing the GPU up), otherwise it starts stock.
 *
 * every other process (posters):
 *   /private/var/tmp/bgmtl.defer present -> BGMTL_DEFER_SCHEDULED=1
 *   /private/var/tmp/bgmtl.fddebug (one line) -> FD_MESA_DEBUG=<line> (freedreno debug flags: nobin, sysmem, ...)
 *   (the wallpaper's flicker fix is /private/var/tmp/bgmtl.sync, read by Chlorophyll itself)
 *
 * One line per start goes to /private/var/tmp/bgmtl-env.log.
 *
 * Self-contained, no imports and no fixups (built without Apple's SDK; ld64.lld cannot emit arm64e chained fixups;
 * linked with -init_offsets): files and sysctl go through raw XNU syscalls, and the environment is replaced through the
 * ProgramVars pointer dyld passes to every initializer (getenv reads *environPtr; a later setenv copies it).
 */
struct ProgramVars {
	const void *mh;
	int *NXArgcPtr;
	const char ***NXArgvPtr;
	const char ***environPtr;
	const char **prognamePtr;
};

#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define SYS_close   6
#define SYS_getpid  20
#define SYS_select  93
#define SYS_sysctl  202

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_APPEND 0x8
#define O_CREAT  0x200
#define O_TRUNC  0x400

static long
sc6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	register long x16 __asm__("x16") = n;
	long err;
	__asm__ volatile ("svc #0x80\n\tcset %1, cs" : "+r"(x0), "=r"(err) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
	    : "memory", "cc");
	return err ? -1 : x0;
}
#define sc3(n, a, b, c) sc6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

#define MAXENV 512
static const char *env[MAXENV + 4];
static char fddebug[16 + 128] = "FD_MESA_DEBUG=";

static long
slen(const char *s)
{
	long n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
has_prefix(const char *s, const char *p)
{
	while (*p) {
		if (*s++ != *p++) {
			return 0;
		}
	}
	return 1;
}

static int
same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int
ends_with(const char *s, const char *t)
{
	long a = slen(s), b = slen(t);
	if (b > a) {
		return 0;
	}
	for (long i = 0; i < b; i++) {
		if (s[a - b + i] != t[i]) {
			return 0;
		}
	}
	return 1;
}

static int
exists(const char *path)
{
	long fd = sc3(SYS_open, path, O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	sc3(SYS_close, fd, 0, 0);
	return 1;
}

/* first line of PATH (at most n - 1 bytes) into buf; 1 if the file could be opened */
static int
readline(const char *path, char *buf, long n)
{
	long fd = sc3(SYS_open, path, O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	long got = sc3(SYS_read, fd, buf, n - 1);
	sc3(SYS_close, fd, 0, 0);
	long i = 0;
	if (got < 0) {
		got = 0;
	}
	while (i < got && buf[i] != '\n' && buf[i] != '\r') {
		i++;
	}
	buf[i] = 0;
	return 1;
}

static void
writefile(const char *path, const char *text, long flags)
{
	long fd = sc3(SYS_open, path, O_WRONLY | O_CREAT | flags, 0644);
	if (fd >= 0) {
		sc3(SYS_write, fd, text, slen(text));
		sc3(SYS_close, fd, 0, 0);
	}
}

static long
append(char *line, long k, long cap, const char *s)
{
	while (*s && k < cap - 1) {
		line[k++] = *s++;
	}
	line[k] = 0;
	return k;
}

/* sysctlbyname(name) for a string value, with raw syscalls: name -> mib through {0, 3}, then the value (truncated to n - 1) */
static int
sysctl_str(const char *name, char *out, long n)
{
	int name2mib[2] = { 0, 3 }, mib[12];
	long len = sizeof(mib);
	if (sc6(SYS_sysctl, (long)name2mib, 2, (long)mib, (long)&len, (long)name, slen(name)) < 0) {
		return 0;
	}
	char big[1024];
	long vlen = sizeof(big) - 1;
	if (sc6(SYS_sysctl, (long)mib, len / (long)sizeof(int), (long)big, (long)&vlen, 0, 0) < 0) {
		return 0;
	}
	if (vlen < 0) {
		vlen = 0;
	}
	big[vlen] = 0;
	long i = 0;
	for (; i < n - 1 && big[i]; i++) {
		out[i] = big[i];
	}
	out[i] = 0;
	return out[0] != 0;
}

static int
boot_uuid(char *out, long n)
{
	return sysctl_str("kern.bootsessionuuid", out, n);
}

static int
contains(const char *s, const char *t)
{
	for (; *s; s++) {
		if (has_prefix(s, t)) {
			return 1;
		}
	}
	return 0;
}

#define GPU_WAIT_MS 20000
static long g_waited_ms = -1;

static int
gpu_on(char *st, long n)
{
	return sysctl_str("kern.sweet_gpu", st, n) && has_prefix(st, "state=on ");
}

#define BB_ARM    "/private/var/tmp/bgmtl-bb.arm"
#define BB_OFF    "/private/var/tmp/bgmtl-bb.off"
#define BB_STATE  "/private/var/tmp/bgmtl-bb.state"

/* bgl-launch's metal_mode(): 1 = this backboardd launch goes on Chlorophyll; why says what decided */
static int
metal_mode(const char **why)
{
	char boot[64], st[160], line[160];
	if (!exists(BB_ARM)) {
		*why = "not armed";
		return 0;
	}
	if (exists(BB_OFF)) {
		*why = "bgmtl-bb.off present";
		return 0;
	}
	if (!exists("/usr/local/lib/bgmtl/bgmtl-inject.dylib") || !exists("/usr/local/lib/bgmtl/libbgair.dylib") ||
	    !exists("/usr/local/lib/bgmtl/libdrm.2.dylib")) {
		*why = "Chlorophyll libraries missing";
		return 0;
	}
	/* the GPU must be up (kern.sweet_gpu "state=on ..."). At boot backboardd can start a moment before the driver finishes:
	 * when the kernel was booted with sweetgpu=8, wait for it (up to GPU_WAIT_MS); otherwise, or if it never comes, this
	 * launch is stock and is not counted as an unhealthy Metal launch */
	if (!gpu_on(st, sizeof(st))) {
		char args[600];
		if (!sysctl_str("kern.bootargs", args, sizeof(args)) || !contains(args, "sweetgpu=8")) {
			*why = "GPU not up (no sweetgpu=8)";
			return 0;
		}
		long waited = 0;
		while (waited < GPU_WAIT_MS && !gpu_on(st, sizeof(st))) {
			long tv[2] = { 0, 250000 };     /* struct timeval: 250 ms */
			sc6(SYS_select, 0, 0, 0, 0, (long)tv, 0);
			waited += 250;
		}
		if (!gpu_on(st, sizeof(st))) {
			*why = "GPU not up after the wait";
			return 0;
		}
		g_waited_ms = waited;
	}
	if (!boot_uuid(boot, sizeof(boot))) {
		boot[0] = '-';
		boot[1] = 0;
	}
	/* "alive=0 boot=<uuid>": the previous Metal launch of this boot never became healthy */
	if (readline(BB_STATE, st, sizeof(st)) && has_prefix(st, "alive=0 boot=") && boot[0] != '-' &&
	    same(st + 13, boot)) {
		writefile(BB_OFF, "the previous Metal launch of this boot never became healthy (bgmtl-env)\n", O_TRUNC);
		*why = "previous Metal launch of this boot never became healthy: bgmtl-bb.off written";
		return 0;
	}
	long k = append(line, 0, sizeof(line), "alive=0 boot=");
	k = append(line, k, sizeof(line), boot);
	append(line, k, sizeof(line), "\n");
	writefile(BB_STATE, line, O_TRUNC);
	*why = "Metal";
	return 1;
}

__attribute__((constructor)) static void
bgmtl_env(int argc, const char **argv, const char **envp, const char **apple, const struct ProgramVars *vars)
{
	(void)argc; (void)apple;
	const char *prog = argv && argv[0] ? argv[0] : "";
	int bb = ends_with(prog, "/backboardd");
	const char *why = "";
	int metal = 0, defer = 0, fd = 0;

	if (bb) {
		metal = metal_mode(&why);
	} else {
		defer = exists("/private/var/tmp/bgmtl.defer");
		fd = readline("/private/var/tmp/bgmtl.fddebug", fddebug + 14, 128) && fddebug[14];
	}
	if ((metal || defer || fd) && vars && vars->environPtr) {
		const char **cur = *vars->environPtr ? *vars->environPtr : envp;
		int n = 0;
		for (int i = 0; cur && cur[i] && n < MAXENV; i++) {
			if (has_prefix(cur[i], "BGMTL_BB=") || has_prefix(cur[i], "BGMTL_DEFER_SCHEDULED=") ||
			    (fd && has_prefix(cur[i], "FD_MESA_DEBUG="))) {
				continue;
			}
			env[n++] = cur[i];
		}
		if (metal) {
			env[n++] = "BGMTL_BB=1";
		}
		if (defer) {
			env[n++] = "BGMTL_DEFER_SCHEDULED=1";
		}
		if (fd) {
			env[n++] = fddebug;
		}
		env[n] = 0;
		*vars->environPtr = env;
	}

	/* one line per start */
	char line[300], num[24];
	long k = 0, p = sc3(SYS_getpid, 0, 0, 0), j = 0;
	do {
		num[j++] = (char)('0' + p % 10);
		p /= 10;
	} while (p > 0 && j < 20);
	k = append(line, k, sizeof(line), "bgmtl-env: pid ");
	while (j > 0 && k < (long)sizeof(line) - 2) {
		line[k++] = num[--j];
	}
	line[k] = 0;
	k = append(line, k, sizeof(line), " ");
	k = append(line, k, sizeof(line), prog);
	if (bb) {
		k = append(line, k, sizeof(line), metal ? ": BGMTL_BB=1 (" : ": stock (");
		k = append(line, k, sizeof(line), why);
		k = append(line, k, sizeof(line), ")");
		if (g_waited_ms >= 0) {
			char w[24];
			long v = g_waited_ms, m = 0;
			do {
				w[m++] = (char)('0' + v % 10);
				v /= 10;
			} while (v > 0 && m < 20);
			k = append(line, k, sizeof(line), ", waited ");
			while (m > 0 && k < (long)sizeof(line) - 2) {
				line[k++] = w[--m];
			}
			line[k] = 0;
			k = append(line, k, sizeof(line), " ms for the GPU");
		}
	}
	if (defer) {
		k = append(line, k, sizeof(line), " BGMTL_DEFER_SCHEDULED=1");
	}
	if (fd) {
		k = append(line, k, sizeof(line), " ");
		k = append(line, k, sizeof(line), fddebug);
	}
	append(line, k, sizeof(line), "\n");
	writefile("/private/var/tmp/bgmtl-env.log", line, O_APPEND);
}
