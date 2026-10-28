/*
 * hotdiag: live OS & hardware observability CLI for a booted iOS device.
 *
 * Reads /dev/sweet-diag and /dev/sweet-log exposed by bsd/dev/sweet_diag.c
 * (Kernel 0132+), with a sysctl/getifaddrs fallback on older kernels.
 *
 * Usage:
 *   hotdiag [status]              colorized health & telemetry report
 *   hotdiag watch [INTERVAL_MS]   live-refreshing terminal dashboard (alias: top)
 *   hotdiag json                  raw JSON from /dev/sweet-diag
 *   hotdiag crashes               recent userland fatal signals + unslid PCs/BTs
 *   hotdiag klog [-f] [-n BYTES] [-g SUBSTR]
 *                                   read or tail (-f) XNU's 1 MiB sweet_log_ring
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define DIAG_CAP (64 * 1024)

static volatile sig_atomic_t g_stop = 0;
static bool g_color = false;

static void on_sigint(int sig)
{
	(void)sig;
	g_stop = 1;
}

static const char *c_rst(void)  { return g_color ? "\033[0m" : ""; }
static const char *c_bold(void) { return g_color ? "\033[1m" : ""; }
static const char *c_dim(void)  { return g_color ? "\033[2m" : ""; }
static const char *c_grn(void)  { return g_color ? "\033[32m" : ""; }
static const char *c_ylw(void)  { return g_color ? "\033[33m" : ""; }
static const char *c_red(void)  { return g_color ? "\033[31m" : ""; }
static const char *c_cyn(void)  { return g_color ? "\033[36m" : ""; }

static const char *diag_path(void)
{
	const char *p = getenv("HOTDIAG_PATH");
	if (!p || !*p) p = getenv("SWEETDIAG_PATH");
	return (p && *p) ? p : "/dev/sweet-diag";
}

static const char *klog_path(void)
{
	const char *p = getenv("HOTKLOG_PATH");
	if (!p || !*p) p = getenv("SWEETKLOG_PATH");
	return (p && *p) ? p : "/dev/sweet-log";
}

static ssize_t read_whole_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	size_t off = 0;
	while (off + 1 < cap) {
		ssize_t got = read(fd, buf + off, cap - 1 - off);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (got == 0)
			break;
		off += (size_t)got;
	}
	close(fd);
	buf[off] = '\0';
	return (ssize_t)off;
}

/* ---- Minimal zero-allocation JSON helpers over sweet_diag.c's schema ---- */

static const char *j_skip_ws(const char *p)
{
	while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

static const char *j_skip_val(const char *p)
{
	p = j_skip_ws(p);
	if (!p || !*p)
		return NULL;
	if (*p == '"') {
		p++;
		while (*p) {
			if (*p == '\\' && p[1]) {
				p += 2;
				continue;
			}
			if (*p == '"')
				return p + 1;
			p++;
		}
		return p;
	}
	if (*p == '{' || *p == '[') {
		char open_c = *p, close_c = (*p == '{') ? '}' : ']';
		int depth = 1;
		p++;
		while (*p && depth > 0) {
			if (*p == '"') {
				p = j_skip_val(p);
				continue;
			}
			if (*p == open_c)
				depth++;
			else if (*p == close_c)
				depth--;
			p++;
		}
		return p;
	}
	while (*p && *p != ',' && *p != '}' && *p != ']' &&
	       *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
		p++;
	return p;
}

/* Find direct child key inside an object starting at '{'. */
static const char *j_key(const char *obj, const char *key)
{
	const char *p = j_skip_ws(obj);
	if (!p || *p != '{')
		return NULL;
	p++;
	size_t klen = strlen(key);
	for (;;) {
		p = j_skip_ws(p);
		if (!p || !*p || *p == '}')
			return NULL;
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p != '"')
			return NULL;
		const char *ks = p + 1;
		const char *ke = j_skip_val(p);
		if (!ke || ke <= ks)
			return NULL;
		size_t len = (size_t)(ke - 1 - ks);
		p = j_skip_ws(ke);
		if (!p || *p != ':')
			return NULL;
		p = j_skip_ws(p + 1);
		if (len == klen && strncmp(ks, key, klen) == 0)
			return p;
		p = j_skip_val(p);
	}
}

/* Dotted path lookup, e.g. "display.backlight.millinits". */
static const char *j_path(const char *root, const char *path)
{
	char seg[64];
	const char *cur = root;
	const char *p = path;
	while (cur && *p) {
		const char *dot = strchr(p, '.');
		size_t n = dot ? (size_t)(dot - p) : strlen(p);
		if (n >= sizeof(seg))
			return NULL;
		memcpy(seg, p, n);
		seg[n] = '\0';
		cur = j_key(cur, seg);
		if (!dot)
			return cur;
		p = dot + 1;
	}
	return cur;
}

static long long j_ll(const char *root, const char *path, long long def)
{
	const char *v = j_path(root, path);
	if (!v)
		return def;
	if (*v == '"')
		v++;
	char *end = NULL;
	long long r = strtoll(v, &end, 0);
	return (end && end != v) ? r : def;
}

static unsigned long long j_ull(const char *root, const char *path, unsigned long long def)
{
	const char *v = j_path(root, path);
	if (!v)
		return def;
	if (*v == '"')
		v++;
	char *end = NULL;
	unsigned long long r = strtoull(v, &end, 0);
	return (end && end != v) ? r : def;
}

static double j_dbl(const char *root, const char *path, double def)
{
	const char *v = j_path(root, path);
	if (!v)
		return def;
	char *end = NULL;
	double r = strtod(v, &end);
	return (end && end != v) ? r : def;
}

static bool j_bool(const char *root, const char *path, bool def)
{
	const char *v = j_path(root, path);
	if (!v)
		return def;
	if (strncmp(v, "true", 4) == 0)
		return true;
	if (strncmp(v, "false", 5) == 0)
		return false;
	return def;
}

static void j_str(const char *root, const char *path, char *out, size_t cap, const char *def)
{
	if (!cap)
		return;
	const char *v = j_path(root, path);
	if (!v || *v != '"') {
		snprintf(out, cap, "%s", def ? def : "");
		return;
	}
	v++;
	size_t w = 0;
	while (*v && *v != '"' && w + 1 < cap) {
		if (*v == '\\' && v[1]) {
			v++;
			char esc = *v;
			if (esc == 'n')
				esc = '\n';
			else if (esc == 'r')
				esc = '\r';
			else if (esc == 't')
				esc = '\t';
			out[w++] = esc;
			v++;
			continue;
		}
		out[w++] = *v++;
	}
	out[w] = '\0';
}

/* Iterate array elements: returns pointer to first element, or advances to next. */
static const char *j_arr_first(const char *arr)
{
	const char *p = j_skip_ws(arr);
	if (!p || *p != '[')
		return NULL;
	p = j_skip_ws(p + 1);
	return (!p || !*p || *p == ']') ? NULL : p;
}

static const char *j_arr_next(const char *elem)
{
	const char *p = j_skip_val(elem);
	p = j_skip_ws(p);
	if (!p || *p != ',')
		return NULL;
	p = j_skip_ws(p + 1);
	return (!p || !*p || *p == ']') ? NULL : p;
}

/* ---- Pre-0132 fallback via sysctl + getifaddrs ---- */

static void fallback_status(void)
{
	printf("%sHOT DIAG (fallback mode: /dev/sweet-diag not present — boot Kernel 0132+ for full live telemetry)%s\n",
	       c_ylw(), c_rst());
#ifdef __APPLE__
	char inj[512] = "";
	size_t len = sizeof(inj);
	if (sysctlbyname("kern.exec_inject", inj, &len, NULL, 0) == 0)
		printf("  exec_inject : %s\n", inj[0] ? inj : "(empty)");
	int soc = -1, mv = -1, ma = 0, temp = 0;
	len = sizeof(int);
	if (sysctlbyname("hw.rmx1931.battery_soc", &soc, &len, NULL, 0) == 0 && soc >= 0) {
		len = sizeof(int); sysctlbyname("hw.rmx1931.battery_mv", &mv, &len, NULL, 0);
		len = sizeof(int); sysctlbyname("hw.rmx1931.battery_ma", &ma, &len, NULL, 0);
		len = sizeof(int); sysctlbyname("hw.rmx1931.battery_temp_deci_c", &temp, &len, NULL, 0);
		printf("  battery     : %d%% (%d mV, %d mA, %d.%d C)\n", soc, mv, ma, temp / 10, abs(temp % 10));
	}
#endif
	struct ifaddrs *ifa = NULL;
	if (getifaddrs(&ifa) == 0) {
		for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
			if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
				char ip[INET_ADDRSTRLEN];
				struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
				inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
				printf("  iface %-5s : %s\n", p->ifa_name, ip);
			}
		}
		freeifaddrs(ifa);
	}
	char mtl[2048];
	if (read_whole_file("/var/local/mtl2-clients", mtl, sizeof(mtl)) > 0)
		printf("  mtl2-clients:\n%s", mtl);
}

/* ---- Pretty status & crashes formatters ---- */

static void print_bar(unsigned pct, unsigned width)
{
	if (pct > 100)
		pct = 100;
	unsigned fill = (pct * width + 50) / 100;
	const char *col = pct >= 85 ? c_red() : (pct >= 50 ? c_ylw() : c_grn());
	printf("[%s", col);
	for (unsigned i = 0; i < width; i++)
		putchar(i < fill ? '#' : '-');
	printf("%s]", c_rst());
}

static void render_crashes(const char *root, bool verbose)
{
	unsigned long long total = j_ull(root, "runtime.crashes_total", 0);
	const char *arr = j_path(root, "runtime.recent_crashes");
	if (total == 0) {
		printf("  Crashes     : %s0 fatal userland signals since boot%s\n", c_grn(), c_rst());
		return;
	}
	printf("  Crashes     : %s%llu total%s (last %d kept in kernel ring):\n",
	       c_red(), total, c_rst(), 16);
	for (const char *el = j_arr_first(arr); el; el = j_arr_next(el)) {
		char name[32], pc_s[32], lr_s[32], far_s[32], slide_s[32], mh_s[32];
		j_str(el, "name", name, sizeof(name), "?");
		j_str(el, "pc", pc_s, sizeof(pc_s), "0x0");
		j_str(el, "lr", lr_s, sizeof(lr_s), "0x0");
		j_str(el, "far", far_s, sizeof(far_s), "0x0");
		j_str(el, "slide", slide_s, sizeof(slide_s), "0x0");
		j_str(el, "mh", mh_s, sizeof(mh_s), "0x0");
		unsigned long long seq = j_ull(el, "seq", 0);
		unsigned long long up = j_ull(el, "uptime_ms", 0);
		long long pid = j_ll(el, "pid", 0);
		long long sig = j_ll(el, "sig", 0);
		unsigned long long pc = strtoull(pc_s, NULL, 0);
		unsigned long long slide = strtoull(slide_s, NULL, 0);
		unsigned long long unslid = (slide && pc > slide) ? (pc - slide) : pc;
		printf("    #%llu [+%llu.%03llus] %s%s[%lld]%s sig=%lld pc=%s (unslid 0x%llx) lr=%s far=%s mh=%s\n",
		       seq, up / 1000ULL, up % 1000ULL, c_bold(), name, pid, c_rst(),
		       sig, pc_s, unslid, lr_s, far_s, mh_s);
		if (verbose) {
			const char *bt = j_key(el, "bt");
			const char *f = j_arr_first(bt);
			if (f) {
				printf("      bt:");
				for (; f; f = j_arr_next(f)) {
					char addr_s[32] = "";
					if (*f == '"') {
						size_t k = 0;
						const char *q = f + 1;
						while (*q && *q != '"' && k + 1 < sizeof(addr_s))
							addr_s[k++] = *q++;
						addr_s[k] = '\0';
					}
					unsigned long long a = strtoull(addr_s, NULL, 0);
					unsigned long long u = (slide && a > slide) ? (a - slide) : a;
					printf(" %s(0x%llx)", addr_s, u);
				}
				printf("\n");
			}
		}
	}
}

static void render_status(const char *root)
{
	/* System */
	unsigned long long up_ms = j_ull(root, "system.uptime_ms", 0);
	long long cpus = j_ll(root, "system.cpus_online", 0);
	bool wdt = j_bool(root, "system.wdt_enabled", false);
	long long rem_s = j_ll(root, "system.reboot_remaining_s", -1);
	char boot_args[256];
	j_str(root, "system.boot_args", boot_args, sizeof(boot_args), "");

	const char *cf = j_path(root, "system.cpufreq_mhz");
	unsigned long long c0 = 0, c1 = 0, c2 = 0;
	const char *ce = j_arr_first(cf);
	if (ce) { c0 = strtoull(ce, NULL, 10); ce = j_arr_next(ce); }
	if (ce) { c1 = strtoull(ce, NULL, 10); ce = j_arr_next(ce); }
	if (ce) { c2 = strtoull(ce, NULL, 10); }

	printf("%s=== HOT LIVE DIAGNOSTICS ===%s  uptime %lluh %llum %llus  cpus=%lld (%llu/%llu/%llu MHz)",
	       c_bold(), c_rst(), up_ms / 3600000ULL, (up_ms / 60000ULL) % 60ULL, (up_ms / 1000ULL) % 60ULL,
	       cpus, c0, c1, c2);
	if (wdt && rem_s >= 0) {
		const char *wc = rem_s < 60 ? c_red() : c_ylw();
		printf("  %s[WDT reboot in %llds]%s", wc, rem_s, c_rst());
	} else if (wdt) {
		printf("  %s[WDT petted]%s", c_grn(), c_rst());
	}
	printf("\n  boot-args   : %s%s%s\n", c_dim(), boot_args, c_rst());

	/* Display */
	bool disp_on = j_bool(root, "display.power_on", false);
	char pmode[32], fourcc[16], fbreason[64], panel_pa[32];
	j_str(root, "display.present_mode", pmode, sizeof(pmode), "unknown");
	j_str(root, "display.last_swap.fourcc", fourcc, sizeof(fourcc), "none");
	j_str(root, "display.last_fallback_reason", fbreason, sizeof(fbreason), "none");
	j_str(root, "display.panel_pa", panel_pa, sizeof(panel_pa), "0x0");
	double fps1 = j_dbl(root, "display.fps_1s", 0.0);
	double fps5 = j_dbl(root, "display.fps_5s", 0.0);
	unsigned long long pw = j_ull(root, "display.panel_w", 0);
	unsigned long long ph = j_ull(root, "display.panel_h", 0);
	unsigned long long pstride = j_ull(root, "display.panel_stride", 0);
	unsigned long long lw = j_ull(root, "display.logical_w", 0);
	unsigned long long lh = j_ull(root, "display.logical_h", 0);
	unsigned long long swaps = j_ull(root, "display.swaps_total", 0);
	unsigned long long gpres = j_ull(root, "display.gpu_presents", 0);
	unsigned long long cpres = j_ull(root, "display.cpu_presents", 0);
	unsigned long long avg_us = j_ull(root, "display.avg_present_us", 0);
	unsigned long long sid = j_ull(root, "display.last_swap.surface_id", 0);
	unsigned long long sw = j_ull(root, "display.last_swap.w", 0);
	unsigned long long sh = j_ull(root, "display.last_swap.h", 0);
	unsigned long long sbpr = j_ull(root, "display.last_swap.bpr", 0);
	unsigned long long sage = j_ull(root, "display.last_swap.age_ms", 0);
	unsigned long long bri = j_ull(root, "display.backlight.brightness_pct", 100);
	unsigned long long mnits = j_ull(root, "display.backlight.millinits", 0);
	unsigned long long dim_a = j_ull(root, "display.backlight.dim_alpha", 0);

	const char *d_col = !disp_on ? c_ylw() :
	    (strncmp(pmode, "gpu_", 4) == 0 ? c_grn() : c_ylw());
	printf("\n%s[DISPLAY]%s %s%s%s  FPS: %s%.1f%s (1s) / %.1f (5s)  avg present: %llu us\n",
	       c_cyn(), c_rst(), d_col, pmode, c_rst(), c_bold(), fps1, c_rst(), fps5, avg_us);
	printf("  Panel       : %llux%llu stride=%llu pa=%s  logical=%llux%llu  power=%s\n",
	       pw, ph, pstride, panel_pa, lw, lh, disp_on ? "ON" : "OFF");
	printf("  Last Swap   : surf#%llu %llux%llu bpr=%llu fmt=%s age=%llums  (swaps=%llu gpu=%llu cpu=%llu)\n",
	       sid, sw, sh, sbpr, fourcc, sage, swaps, gpres, cpres);
	printf("  Backlight   : %llu%% (%llu.%03llu nits, dim_alpha=%llu/255)  last CPU fallback: %s\n",
	       bri, mnits / 1000ULL, mnits % 1000ULL, dim_a, fbreason);

	/* GPU & Metal Clients */
	bool g_ready = j_bool(root, "gpu.ready", false);
	unsigned long long g_mhz = j_ull(root, "gpu.clock_mhz", 0);
	unsigned long long g_busy = j_ull(root, "gpu.busy_pct_live", 0);
	unsigned long long g_rec = j_ull(root, "gpu.recoveries", 0);
	unsigned long long p_used = j_ull(root, "gpu.pool_used_mb", 0);
	unsigned long long p_tot = j_ull(root, "gpu.pool_total_mb", 0);
	unsigned long long p_peak = j_ull(root, "gpu.pool_peak_mb", 0);
	unsigned long long p_fail = j_ull(root, "gpu.pool_alloc_failures", 0);
	unsigned long long p_cap = j_ull(root, "gpu.pool_cap_mb", 0);
	unsigned long long p_res = j_ull(root, "gpu.pool_reserve_mb", 0);
	unsigned long long w_sub = j_ull(root, "gpu.window_5s.submits", 0);
	unsigned long long w_mean = j_ull(root, "gpu.window_5s.mean_us", 0);
	unsigned long long w_max = j_ull(root, "gpu.window_5s.longest_us", 0);

	printf("\n%s[GPU / METAL]%s %s%s%s  %llu MHz  busy ",
	       c_cyn(), c_rst(), g_ready ? c_grn() : c_red(),
	       g_ready ? "Adreno 640 READY" : "NOT READY", c_rst(), g_mhz);
	print_bar((unsigned)g_busy, 16);
	printf(" %llu%%  recoveries=%llu\n", g_busy, g_rec);
	printf("  Pool        : %llu / %llu MB used (peak %llu MB, app cap %llu MB, sys reserve %llu MB, alloc_fail=%llu)\n",
	       p_used, p_tot, p_peak, p_cap, p_res, p_fail);
	printf("  5s Submits  : %llu (mean %llu us, max %llu us)  blit=%lluus clean=%lluus\n",
	       w_sub, w_mean, w_max, j_ull(root, "gpu.blit_us", 0), j_ull(root, "gpu.clean_us", 0));

	const char *clients = j_path(root, "gpu.clients");
	const char *cel = j_arr_first(clients);
	if (cel) {
		printf("  Clients     : %-6s %-20s %-10s %9s %9s %9s %s\n",
		       "PID", "PROCESS", "ABI", "POOL_KB", "PEAK_KB", "SUBMITS", "FLAGS");
		for (; cel; cel = j_arr_next(cel)) {
			char cname[36], abi[16];
			j_str(cel, "name", cname, sizeof(cname), "?");
			j_str(cel, "abi", abi, sizeof(abi), "?");
			long long cpid = j_ll(cel, "pid", 0);
			unsigned long long ckb = j_ull(cel, "pool_kb", 0);
			unsigned long long cpk = j_ull(cel, "pool_peak_kb", 0);
			unsigned long long csub = j_ull(cel, "submits", 0);
			unsigned long long cflt = j_ull(cel, "faults", 0);
			bool csys = j_bool(cel, "system", false);
			bool ccap = j_bool(cel, "cap_refused", false);
			printf("                %-6lld %-20s %-10s %9llu %9llu %9llu %s%s%s\n",
			       cpid, cname, abi, ckb, cpk, csub,
			       csys ? "SYS " : "", ccap ? "CAP_HIT " : "", cflt ? "FAULT" : "");
		}
	} else {
		printf("  Clients     : %s(no IOAccelerator user clients open)%s\n", c_ylw(), c_rst());
	}

	/* Input */
	char tdrv[40];
	j_str(root, "input.touch_driver", tdrv, sizeof(tdrv), "none");
	bool t_ok = j_bool(root, "input.touch_ready", false);
	printf("\n%s[INPUT & NETWORK]%s\n", c_cyn(), c_rst());
	printf("  Touch       : %s%s%s  events=%llu err=%llu fingers=0x%llx last=(%lld,%lld) %llums ago  btns=%llu\n",
	       t_ok ? c_grn() : c_ylw(), tdrv, c_rst(),
	       j_ull(root, "input.touch_events", 0), j_ull(root, "input.touch_errors", 0),
	       j_ull(root, "input.fingers_mask", 0), j_ll(root, "input.last_x", 0),
	       j_ll(root, "input.last_y", 0), j_ull(root, "input.last_touch_age_ms", 0),
	       j_ull(root, "input.button_events", 0));

	/* Network */
	bool wl_link = j_bool(root, "network.wifi.link_up", false);
	char ssid[40], en0_ip[24], ue0_ip[24];
	j_str(root, "network.wifi.ssid", ssid, sizeof(ssid), "");
	j_str(root, "network.wifi.en0_ip", en0_ip, sizeof(en0_ip), "");
	j_str(root, "network.usb.ue0_ip", ue0_ip, sizeof(ue0_ip), "");
	printf("  Wi-Fi (en0) : %s%s%s  ssid=\"%s\" rssi=%lld dBm %llu MHz  ip=%s%s%s (rx=%llu tx=%llu)\n",
	       wl_link ? c_grn() : c_ylw(), wl_link ? "LINK_UP" : "DOWN", c_rst(),
	       ssid, j_ll(root, "network.wifi.rssi_dbm", 0), j_ull(root, "network.wifi.freq_mhz", 0),
	       en0_ip[0] ? c_bold() : "", en0_ip[0] ? en0_ip : "none", en0_ip[0] ? c_rst() : "",
	       j_ull(root, "network.wifi.en0_rx_pkts", 0), j_ull(root, "network.wifi.en0_tx_pkts", 0));
	bool usb_cfg = j_bool(root, "network.usb.configured", false);
	printf("  USB (ue0)   : %s  ip=%s (rx=%llu tx=%llu)  TCP debug=2223\n",
	       usb_cfg ? "CONFIGURED" : "unplugged/idle", ue0_ip[0] ? ue0_ip : "none",
	       j_ull(root, "network.usb.rx_frames", 0), j_ull(root, "network.usb.tx_frames", 0));

	/* Peripherals */
	printf("\n%s[PERIPHERALS & RUNTIME]%s\n", c_cyn(), c_rst());
	if (j_bool(root, "peripherals.battery.gauge_online", false)) {
		long long t_dc = j_ll(root, "peripherals.battery.temp_deci_c", 0);
		printf("  Battery     : %llu%%  %llu mV  %+lld mA  %lld.%lld C  (%llu/%llu mAh, %s)\n",
		       j_ull(root, "peripherals.battery.soc_pct", 0),
		       j_ull(root, "peripherals.battery.voltage_mv", 0),
		       j_ll(root, "peripherals.battery.current_ma", 0),
		       t_dc / 10, llabs(t_dc % 10),
		       j_ull(root, "peripherals.battery.rem_mah", 0),
		       j_ull(root, "peripherals.battery.fcc_mah", 0),
		       j_bool(root, "peripherals.battery.charging", false) ? "charging" : "discharging");
	} else {
		printf("  Battery     : gauge offline\n");
	}
	printf("  Audio/Misc  : audio=%s (%s, nonsilent=%llums/%llums, peak=%llu%%)  torch=%llu  haptic=%llu\n",
	       j_bool(root, "peripherals.audio.ready", false) ? "ready" : "off",
	       j_bool(root, "peripherals.audio.streaming_now", false) ? "STREAMING" : "idle",
	       j_ull(root, "peripherals.audio.nonsilent_ms", 0),
	       j_ull(root, "peripherals.audio.written_ms", 0),
	       j_ull(root, "peripherals.audio.peak_pct", 0),
	       j_ull(root, "peripherals.torch.level", 0),
	       j_ull(root, "peripherals.haptic.activations", 0));

	char inj[256];
	j_str(root, "runtime.exec_inject", inj, sizeof(inj), "");
	printf("  exec_inject : hits=%llu  rule=\"%s\"  sandbox_denied=%llu\n",
	       j_ull(root, "runtime.exec_inject_hits", 0), inj,
	       j_ull(root, "runtime.sandbox_denied_total", 0));
	render_crashes(root, false);
}

/* ---- Kernel console log reader / tailer (/dev/sweet-log) ---- */

static uint64_t query_klog_pos(char *diag_buf)
{
#ifdef __APPLE__
	uint64_t pos = 0;
	size_t sz = sizeof(pos);
	if (sysctlbyname("kern.sweet.log_pos", &pos, &sz, NULL, 0) == 0)
		return pos;
#endif
	if (read_whole_file(diag_path(), diag_buf, DIAG_CAP) > 0)
		return (uint64_t)j_ull(diag_buf, "system.klog_bytes_total", 0);
	return 0;
}

static void emit_filtered(const char *buf, size_t n, const char *grep_sub, char *line_acc, size_t *acc_len)
{
	if (!grep_sub || !*grep_sub) {
		(void)fwrite(buf, 1, n, stdout);
		fflush(stdout);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		char ch = buf[i];
		if (*acc_len + 1 < 2048)
			line_acc[(*acc_len)++] = ch;
		if (ch == '\n') {
			line_acc[*acc_len] = '\0';
			if (strstr(line_acc, grep_sub))
				fputs(line_acc, stdout);
			*acc_len = 0;
		}
	}
	fflush(stdout);
}

static int cmd_klog(int argc, char **argv, char *diag_buf)
{
	bool follow = false;
	uint64_t tail_bytes = 32768;
	const char *grep_sub = NULL;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0)
			follow = true;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			tail_bytes = strtoull(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
			grep_sub = argv[++i];
	}

	int fd = open(klog_path(), O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "hotdiag: open %s: %s\n", klog_path(), strerror(errno));
		return 1;
	}
	uint64_t end = query_klog_pos(diag_buf);
	uint64_t off = (end > tail_bytes) ? (end - tail_bytes) : 0;
	char chunk[8192];
	char line_acc[2048];
	size_t acc_len = 0;

	signal(SIGINT, on_sigint);
	while (!g_stop) {
		ssize_t got = pread(fd, chunk, sizeof(chunk), (off_t)off);
		if (got > 0) {
			emit_filtered(chunk, (size_t)got, grep_sub, line_acc, &acc_len);
			off += (uint64_t)got;
			/* Handle ring wrap if off fell behind sweet_log_ring's 1 MiB window */
			uint64_t latest = query_klog_pos(diag_buf);
			if (latest > 1048576ULL && off < latest - 1048576ULL)
				off = latest - 1048576ULL;
			continue;
		}
		if (!follow)
			break;
		uint64_t latest = query_klog_pos(diag_buf);
		if (latest > off) {
			if (latest > 1048576ULL && off < latest - 1048576ULL)
				off = latest - 1048576ULL;
			continue;
		}
		usleep(200000);
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	g_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
	const char *cmd = (argc > 1) ? argv[1] : "status";

	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
		printf("Usage:\n"
		       "  hotdiag [status]              live OS & hardware health report\n"
		       "  hotdiag watch [INTERVAL_MS]   auto-refreshing dashboard (alias: top)\n"
		       "  hotdiag json                  raw JSON from /dev/sweet-diag\n"
		       "  hotdiag crashes               detailed userland crash ring + unslid BTs\n"
		       "  hotdiag klog [-f] [-n BYTES] [-g SUBSTR]\n"
		       "                                  read or follow XNU sweet_log_ring\n");
		return 0;
	}

	char *buf = calloc(1, DIAG_CAP);
	if (!buf)
		return 1;

	if (strcmp(cmd, "klog") == 0) {
		int rc = cmd_klog(argc, argv, buf);
		free(buf);
		return rc;
	}

	if (strcmp(cmd, "watch") == 0 || strcmp(cmd, "top") == 0) {
		int ms = (argc > 2) ? atoi(argv[2]) : 1000;
		if (ms < 100)
			ms = 100;
		signal(SIGINT, on_sigint);
		while (!g_stop) {
			ssize_t n = read_whole_file(diag_path(), buf, DIAG_CAP);
			printf("\033[H\033[2J");
			if (n > 0)
				render_status(buf);
			else
				fallback_status();
			fflush(stdout);
			usleep((useconds_t)ms * 1000u);
		}
		free(buf);
		return 0;
	}

	ssize_t n = read_whole_file(diag_path(), buf, DIAG_CAP);
	if (n <= 0) {
		if (strcmp(cmd, "json") == 0) {
			fprintf(stderr, "hotdiag: cannot read %s: %s\n", diag_path(), strerror(errno));
			free(buf);
			return 1;
		}
		fallback_status();
		free(buf);
		return 0;
	}

	if (strcmp(cmd, "json") == 0) {
		fwrite(buf, 1, (size_t)n, stdout);
	} else if (strcmp(cmd, "crashes") == 0) {
		render_crashes(buf, true);
	} else {
		render_status(buf);
	}
	free(buf);
	return 0;
}
