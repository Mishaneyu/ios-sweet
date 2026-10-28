/*
 * SPDX-License-Identifier: MIT
 *
 * adspd.c: Main daemon & CLI entrypoint for RMX1931 SM8150 LPASS ADSP audio
 * and Maxim MAX98927L/R stereo speaker + cellular voice call routing.
 *
 * Usage:
 *   adspd [auto|up]     Boot ADSP, bring up Q6AFE/ADM/ASM + MAX98927, stream PCM
 *   adspd status        Show /var/local/adsp-status.json & /tmp/sweet-audio.shm
 *   adspd tone [ms]     Play stereo C5+E5 test chime (via running daemon or standalone)
 *   adspd voice-test    Verify live Q6Voice (MVM/CVS/CVP) bringup & media resume
 *   adspd i2c-scan      Reset MAX98927L/R and scan SOUTH gpio51/52 I2C bus
 *   adspd shutdown      Cleanly stop adspd and run PAS_SHUTDOWN(1) + QMP off
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "adspd.h"

/* Private XNU libsystem_kernel jetsam control (same as wland.c) */
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
    void *buffer, size_t size);
struct sweet_memlimit_props {
	int32_t  active;
	uint32_t active_attr;
	int32_t  inactive;
	uint32_t inactive_attr;
};

struct adsp_status g_status;
volatile int g_stop_requested;
struct sweet_audio_shm *g_shm;
static uint64_t boot_t0_ms;

void
adsp_log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	uint64_t rel;
	int n;

	if (boot_t0_ms == 0) {
		boot_t0_ms = now_ms();
	}
	rel = now_ms() - boot_t0_ms;
	n = snprintf(line, sizeof(line), "[%7llu] adspd: ", (unsigned long long)rel);
	if (n < 0) {
		return;
	}
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	line[n] = '\0';
	(void)write(STDOUT_FILENO, line, (size_t)n);
	if (!g_status.asm_stream_up || g_status.periods_written < 24u) {
		(void)fsync(STDOUT_FILENO);
	}
}

static void
json_escape_copy(char *dst, size_t dst_sz, const char *src)
{
	size_t o = 0;

	for (size_t i = 0; src[i] != '\0' && o + 2 < dst_sz; i++) {
		char c = src[i];
		if (c == '"' || c == '\\') {
			dst[o++] = '_';
		} else if ((unsigned char)c < 0x20) {
			dst[o++] = ' ';
		} else {
			dst[o++] = c;
		}
	}
	dst[o] = '\0';
}

void
adsp_status_write(void)
{
	char tmp_path[128];
	char buf[1536];
	char err_esc[128];
	uint32_t shm_w = 0, shm_r = 0, shm_vol = 0, shm_spk = 0, shm_voice = 0;
	int fd, n;

	if (g_shm != NULL) {
		shm_w = __atomic_load_n(&g_shm->write_frames, __ATOMIC_RELAXED);
		shm_r = __atomic_load_n(&g_shm->read_frames, __ATOMIC_RELAXED);
		shm_vol = __atomic_load_n(&g_shm->volume_q16, __ATOMIC_RELAXED);
		shm_spk = __atomic_load_n(&g_shm->speaker_running, __ATOMIC_RELAXED);
		shm_voice = __atomic_load_n(&g_shm->baseband_voice_io, __ATOMIC_RELAXED);
	}
	json_escape_copy(err_esc, sizeof(err_esc), g_status.last_error);

	n = snprintf(buf, sizeof(buf),
	    "{\n"
	    "  \"pid\": %d,\n"
	    "  \"state\": \"%s\",\n"
	    "  \"adsp_booted\": %s,\n"
	    "  \"smp2p_ready\": %s,\n"
	    "  \"smp2p_handover\": %s,\n"
	    "  \"proxy_votes_released\": %s,\n"
	    "  \"glink_ipcrtr_up\": %s,\n"
	    "  \"glink_apr_up\": %s,\n"
	    "  \"glink_fastrpc_up\": %s,\n"
	    "  \"audiopd_spawned\": %s,\n"
	    "  \"q6_core_ready\": %s,\n"
	    "  \"smmu_configured\": %s,\n"
	    "  \"smmu_cb\": %d,\n"
	    "  \"afe_quat_rx_up\": %s,\n"
	    "  \"adm_copp_up\": %s,\n"
	    "  \"adm_copp_id\": %d,\n"
	    "  \"asm_stream_up\": %s,\n"
	    "  \"asm_map_handle\": \"0x%x\",\n"
	    "  \"amp_left_0x39_ok\": %s,\n"
	    "  \"amp_left_rev\": \"0x%02x\",\n"
	    "  \"amp_right_0x3a_ok\": %s,\n"
	    "  \"amp_right_rev\": \"0x%02x\",\n"
	    "  \"amp_enabled\": %s,\n"
	    "  \"voice_active\": %s,\n"
	    "  \"voice_speakerphone\": %s,\n"
	    "  \"mvm_handle\": \"0x%x\",\n"
	    "  \"cvs_handle\": \"0x%x\",\n"
	    "  \"cvp_handle\": \"0x%x\",\n"
	    "  \"periods_written\": %llu,\n"
	    "  \"frames_played\": %llu,\n"
	    "  \"underruns\": %llu,\n"
	    "  \"pd_queries\": %u,\n"
	    "  \"tftp_reads\": %u,\n"
	    "  \"fastrpc_rev_calls\": %u,\n"
	    "  \"shm_write_frames\": %u,\n"
	    "  \"shm_read_frames\": %u,\n"
	    "  \"shm_volume_q16\": %u,\n"
	    "  \"shm_speaker_running\": %u,\n"
	    "  \"shm_baseband_voice_io\": %u,\n"
	    "  \"last_error\": \"%s\"\n"
	    "}\n",
	    g_status.pid,
	    g_status.state[0] ? g_status.state : "init",
	    g_status.adsp_booted ? "true" : "false",
	    g_status.smp2p_ready ? "true" : "false",
	    g_status.smp2p_handover ? "true" : "false",
	    g_status.proxy_votes_released ? "true" : "false",
	    g_status.glink_ipcrtr_up ? "true" : "false",
	    g_status.glink_apr_up ? "true" : "false",
	    g_status.glink_fastrpc_up ? "true" : "false",
	    g_status.audiopd_spawned ? "true" : "false",
	    g_status.q6_core_ready ? "true" : "false",
	    g_status.smmu_configured ? "true" : "false",
	    g_status.smmu_cb,
	    g_status.afe_quat_rx_up ? "true" : "false",
	    g_status.adm_copp_up ? "true" : "false",
	    g_status.adm_copp_id,
	    g_status.asm_stream_up ? "true" : "false",
	    g_status.asm_map_handle,
	    g_status.amp_left_ok ? "true" : "false",
	    g_status.amp_left_rev,
	    g_status.amp_right_ok ? "true" : "false",
	    g_status.amp_right_rev,
	    g_status.amp_enabled ? "true" : "false",
	    g_status.voice_active ? "true" : "false",
	    g_status.voice_speakerphone ? "true" : "false",
	    g_status.mvm_handle,
	    g_status.cvs_handle,
	    g_status.cvp_handle,
	    (unsigned long long)g_status.periods_written,
	    (unsigned long long)g_status.frames_played,
	    (unsigned long long)g_status.underruns,
	    g_status.pd_queries,
	    g_status.tftp_reads,
	    g_status.fastrpc_rev_calls,
	    shm_w, shm_r, shm_vol, shm_spk, shm_voice,
	    err_esc);
	if (n <= 0) {
		return;
	}

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ADSP_STATUS_PATH);
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		(void)write(fd, buf, (size_t)n);
		if (!g_status.asm_stream_up || g_status.periods_written < 24u) {
			(void)fsync(fd);
		}
		close(fd);
		(void)rename(tmp_path, ADSP_STATUS_PATH);
	}
}

static void
on_term_signal(int sig)
{
	(void)sig;
	g_stop_requested = 1;
}

static int
shm_init_ring(void)
{
	struct stat st;
	struct sweet_audio_shm *m;
	int fd;

	fd = open(SWEET_AUDIO_SHM_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
	if (fd < 0) {
		adsp_log("shm: open %s failed: %s", SWEET_AUDIO_SHM_PATH, strerror(errno));
		return -1;
	}
	(void)fchmod(fd, 0666);
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	if ((size_t)st.st_size < sizeof(struct sweet_audio_shm)) {
		if (ftruncate(fd, (off_t)sizeof(struct sweet_audio_shm)) != 0) {
			adsp_log("shm: ftruncate failed: %s", strerror(errno));
			close(fd);
			return -1;
		}
	}
	m = mmap(NULL, sizeof(struct sweet_audio_shm), PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		adsp_log("shm: mmap failed: %s", strerror(errno));
		return -1;
	}

	if (__atomic_load_n(&m->magic, __ATOMIC_ACQUIRE) != SWEET_AUDIO_SHM_MAGIC ||
	    m->version != SWEET_AUDIO_SHM_VERSION) {
		memset(m, 0, sizeof(*m));
		m->version = SWEET_AUDIO_SHM_VERSION;
		m->volume_q16 = 65536u;
		m->codec_mic_source = 'mc01';
		__atomic_store_n(&m->magic, SWEET_AUDIO_SHM_MAGIC, __ATOMIC_RELEASE);
		adsp_log("shm: created %s (%zu B, mode 0666)", SWEET_AUDIO_SHM_PATH, sizeof(*m));
	} else {
		uint32_t w = __atomic_load_n(&m->write_frames, __ATOMIC_ACQUIRE);
		__atomic_store_n(&m->read_frames, w, __ATOMIC_RELEASE);
		adsp_log("shm: attached existing %s at frame %u (vol_q16=%u)",
		    SWEET_AUDIO_SHM_PATH, w, m->volume_q16);
	}
	g_shm = m;
	return 0;
}

static void
escape_jetsam_and_bg(void)
{
	struct sweet_memlimit_props ml = { 2048, 0, 2048, 0 };

	(void)memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));
	(void)setpriority(PRIO_DARWIN_PROCESS, 0, 0);
	(void)setpriority(PRIO_PROCESS, 0, -12);
	(void)setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, IOPOL_IMPORTANT);
}

static bool
qrtr_sock_is_live(void)
{
	struct timeval bt = { 0 };
	size_t blen = sizeof(bt);
	char want[32], stamp[32] = "";
	char buf[4096];
	FILE *f;

	if (access("/var/local/wland.off", F_OK) == 0) {
		return true;
	}
	/*
	 * Check /var/local/wland.boot + /var/local/wland.log instead of opening
	 * and immediately closing /var/run/sweet-qrtr.sock (which would allocate
	 * qrtr-mux port 0x40 and emit a spurious QRTR_TYPE_DEL_CLIENT to MPSS).
	 */
	if (sysctlbyname("kern.boottime", &bt, &blen, NULL, 0) != 0) {
		return access(QRTR_MUX_SOCK_PATH, F_OK) == 0;
	}
	snprintf(want, sizeof(want), "%ld\n", (long)bt.tv_sec);
	f = fopen("/var/local/wland.boot", "r");
	if (f == NULL) {
		return false;
	}
	if (fgets(stamp, sizeof(stamp), f) == NULL) {
		stamp[0] = '\0';
	}
	fclose(f);
	if (strcmp(stamp, want) != 0) {
		return false;
	}
	f = fopen("/var/local/wland.log", "rb");
	if (f == NULL) {
		return false;
	}
	size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[nr] = '\0';
	return strstr(buf, "qrtr-mux: listening on") != NULL ||
	    strstr(buf, "glink: IPCRTR open") != NULL ||
	    strstr(buf, "GLINK/QRTR link_up failed") != NULL;
}

static bool
wland_wlan_bringup_done(void)
{
	char buf[8192];
	int fd;

	if (access("/var/local/wland.off", F_OK) == 0) {
		return true;
	}
	fd = open("/dev/sweet-diag", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			if (strstr(buf, "\"wland_open\": true") != NULL ||
			    strstr(buf, "\"radio_ready\": true") != NULL) {
				return true;
			}
		}
	}
	FILE *f = fopen("/var/local/wland.log", "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long sz = ftell(f);
			long off = sz > 4096 ? (sz - 4096) : 0;
			if (fseek(f, off, SEEK_SET) == 0) {
				size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
				buf[nr] = '\0';
				if (strstr(buf, "bridge: /dev/wlan open") != NULL ||
				    strstr(buf, "WMI READY") != NULL ||
				    strstr(buf, "WLFW bring-up failed") != NULL ||
				    strstr(buf, "radio start failed") != NULL ||
				    strstr(buf, "wland: off") != NULL) {
					fclose(f);
					return true;
				}
			}
		}
		fclose(f);
	}
	return false;
}

static void
wait_for_wland_qrtr(void)
{
	bool qrtr_up = qrtr_sock_is_live();

	if (!qrtr_up) {
		adsp_log("coexist: waiting up to 20s for live %s...", QRTR_MUX_SOCK_PATH);
		for (int i = 0; i < 200 && !g_stop_requested; i++) {
			if (qrtr_sock_is_live()) {
				adsp_log("coexist: %s live after %d ms", QRTR_MUX_SOCK_PATH, i * 100);
				qrtr_up = true;
				break;
			}
			delay_ms(100);
		}
		if (!qrtr_up) {
			adsp_log("coexist: proceeding without live %s after 20s timeout", QRTR_MUX_SOCK_PATH);
			return;
		}
	}
	/*
	 * MPSS spawns wlan_pd (wlanmdsp.mbn TFTP + MSA hyp_assign + WLFW mission
	 * mode) across t=1.4s..11.8s after QRTR comes up. Wait until wland opens
	 * /dev/wlan before firing TrustZone PAS_INIT_IMAGE/MEM_SETUP/AUTH_AND_RESET
	 * on ADSP so PIL/RPMh/QMP never collide with wlan_pd bring-up.
	 */
	if (wland_wlan_bringup_done()) {
		adsp_log("coexist: wland /dev/wlan already up");
		return;
	}
	adsp_log("coexist: waiting up to 16s for wland WLFW + /dev/wlan bring-up...");
	for (int i = 0; i < 160 && !g_stop_requested; i++) {
		if (wland_wlan_bringup_done()) {
			adsp_log("coexist: wland /dev/wlan ready after %d ms", i * 100);
			delay_ms(150);
			return;
		}
		delay_ms(100);
	}
	adsp_log("coexist: proceeding after 16s wland WLFW wait timeout");
}

static int
find_running_adspd_pid(bool *out_streaming)
{
	FILE *f = fopen(ADSP_STATUS_PATH, "r");
	char buf[1536];
	size_t n;
	const char *p;
	int pid = -1;

	if (out_streaming != NULL) {
		*out_streaming = false;
	}
	if (f == NULL) {
		return -1;
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	p = strstr(buf, "\"pid\":");
	if (p != NULL) {
		pid = atoi(p + 6);
	}
	if (pid > 1 && pid != (int)getpid() && kill(pid, 0) == 0) {
		if (out_streaming != NULL && strstr(buf, "\"asm_stream_up\": true") != NULL) {
			*out_streaming = true;
		}
		return pid;
	}
	return -1;
}

static int
cmd_status(void)
{
	FILE *f = fopen(ADSP_STATUS_PATH, "r");

	if (f != NULL) {
		char buf[1536];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = '\0';
		printf("=== %s ===\n%s", ADSP_STATUS_PATH, buf);
	} else {
		printf("%s: not present (%s)\n", ADSP_STATUS_PATH, strerror(errno));
	}

	int fd = open(SWEET_AUDIO_SHM_PATH, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		struct sweet_audio_shm *s = mmap(NULL, sizeof(*s), PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if (s != MAP_FAILED) {
			uint32_t w = __atomic_load_n(&s->write_frames, __ATOMIC_ACQUIRE);
			uint32_t r = __atomic_load_n(&s->read_frames, __ATOMIC_ACQUIRE);
			printf("=== %s ===\n"
			    "  magic=0x%08x ver=%u write=%u read=%u queued=%u\n"
			    "  volume_q16=%u speaker_running=%u baseband_voice_io=%u mic_src=0x%08x\n",
			    SWEET_AUDIO_SHM_PATH, s->magic, s->version, w, r, w - r,
			    s->volume_q16, s->speaker_running, s->baseband_voice_io,
			    s->codec_mic_source);
			munmap(s, sizeof(*s));
		}
	} else {
		printf("%s: not present (%s)\n", SWEET_AUDIO_SHM_PATH, strerror(errno));
	}
	return 0;
}

static int
cmd_i2c_scan(void)
{
	if (pm_open_dev() != 0) {
		return 1;
	}
	/* Pulse active-low resets on MAX98927L (SOUTH 148) & MAX98927R (NORTH 37) */
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_MAX98927L_RESET, 0);
	tlmm_gpio_drive(TLMM_NORTH_PA, GPIO_MAX98927R_RESET, 0);
	usleep(1500);
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_MAX98927L_RESET, 1);
	tlmm_gpio_drive(TLMM_NORTH_PA, GPIO_MAX98927R_RESET, 1);
	usleep(6000);
	spk_i2c_scan_bus();
	return 0;
}

static int
cmd_shutdown(void)
{
	int pid = find_running_adspd_pid(NULL);

	if (pid > 1) {
		printf("stopping running adspd (pid %d)...\n", pid);
		(void)kill(pid, SIGTERM);
		for (int i = 0; i < 20 && kill(pid, 0) == 0; i++) {
			delay_ms(100);
		}
	}
	return adsp_pas_shutdown() == 0 ? 0 : 1;
}

static int
cmd_voice_test(unsigned hold_ms)
{
	uint8_t saved[256];
	ssize_t saved_len = -1;
	char tmp_path[128];
	char sbuf[1536];
	bool voice_ok = false, resume_ok = false;
	int fd;

	fd = open(VOICE_CALL_STATE_PATH, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		saved_len = read(fd, saved, sizeof(saved));
		close(fd);
	}

	snprintf(tmp_path, sizeof(tmp_path), "%s.vtest.%d", VOICE_CALL_STATE_PATH, (int)getpid());
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0) {
		fprintf(stderr, "adspd voice-test: open %s failed: %s\n", tmp_path, strerror(errno));
		return 1;
	}
	(void)fchmod(fd, 0666);
	{
		const char *json = "{\"active\":true,\"speaker\":true}\n";
		(void)write(fd, json, strlen(json));
	}
	close(fd);
	if (rename(tmp_path, VOICE_CALL_STATE_PATH) != 0) {
		(void)unlink(tmp_path);
		return 1;
	}

	for (int i = 0; i < 30; i++) {
		delay_ms(100);
		FILE *f = fopen(ADSP_STATUS_PATH, "r");
		if (f != NULL) {
			size_t n = fread(sbuf, 1, sizeof(sbuf) - 1, f);
			fclose(f);
			sbuf[n] = '\0';
			if (strstr(sbuf, "\"voice_active\": true") != NULL) {
				voice_ok = true;
				break;
			}
		}
	}
	if (voice_ok) {
		printf("adspd voice-test: Q6Voice ACTIVE!\n");
		const char *mvm = strstr(sbuf, "\"mvm_handle\":");
		const char *cvs = strstr(sbuf, "\"cvs_handle\":");
		const char *cvp = strstr(sbuf, "\"cvp_handle\":");
		if (mvm) printf("  %.28s\n", mvm);
		if (cvs) printf("  %.28s\n", cvs);
		if (cvp) printf("  %.28s\n", cvp);
		delay_ms(hold_ms);
	} else {
		printf("adspd voice-test: FAILED to enter voice_active within 3000 ms\n");
	}

	/* Restore previous /tmp/sweet-voice-call.state (or mark idle) */
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd >= 0) {
		(void)fchmod(fd, 0666);
		if (saved_len > 0) {
			(void)write(fd, saved, (size_t)saved_len);
		} else {
			uint32_t idle_shm[4] = { 0x53575643u, 0u, 0u, 0u };
			(void)write(fd, idle_shm, sizeof(idle_shm));
		}
		close(fd);
		(void)rename(tmp_path, VOICE_CALL_STATE_PATH);
	} else {
		(void)unlink(VOICE_CALL_STATE_PATH);
	}

	for (int i = 0; i < 30; i++) {
		delay_ms(100);
		FILE *f = fopen(ADSP_STATUS_PATH, "r");
		if (f != NULL) {
			size_t n = fread(sbuf, 1, sizeof(sbuf) - 1, f);
			fclose(f);
			sbuf[n] = '\0';
			if (strstr(sbuf, "\"voice_active\": false") != NULL &&
			    strstr(sbuf, "\"asm_stream_up\": true") != NULL) {
				resume_ok = true;
				break;
			}
		}
	}
	printf("adspd voice-test: media_resumed=%d\n", resume_ok ? 1 : 0);
	return (voice_ok && resume_ok) ? 0 : 1;
}

static int
inject_tone_via_shm(unsigned duration_ms)
{
	int fd = open(SWEET_AUDIO_SHM_PATH, O_RDWR | O_CLOEXEC);
	struct sweet_audio_shm *s;

	if (fd < 0) {
		return -1;
	}
	s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (s == MAP_FAILED) {
		return -1;
	}

	uint32_t total_frames = (duration_ms * HAKIOS_AUDIO_RATE) / 1000u;
	uint32_t done = 0;
	uint32_t mask = SWEET_AUDIO_RING_FRAMES - 1u;
	uint32_t start_r = __atomic_load_n(&s->read_frames, __ATOMIC_ACQUIRE);

	if (__atomic_load_n(&s->volume_q16, __ATOMIC_ACQUIRE) == 0) {
		__atomic_store_n(&s->volume_q16, 65536u, __ATOMIC_RELEASE);
	}
	printf("adspd tone: feeding %u ms stereo C5+E5 chime into running daemon via %s...\n",
	    duration_ms, SWEET_AUDIO_SHM_PATH);

	while (done < total_frames) {
		uint32_t w = __atomic_load_n(&s->write_frames, __ATOMIC_RELAXED);
		uint32_t r = __atomic_load_n(&s->read_frames, __ATOMIC_ACQUIRE);
		if (w - r > 8192u) {
			usleep(10000);
			continue;
		}
		uint32_t chunk = total_frames - done > 960u ? 960u : (total_frames - done);
		for (uint32_t i = 0; i < chunk; i++) {
			uint32_t idx = ((w + i) & mask) * 2u;
			uint32_t pos = done + i;
			double env = 1.0;
			if (pos < 960u) {
				env = (double)pos / 960.0;
			} else if (total_frames - pos < 2400u) {
				env = (double)(total_frames - pos) / 2400.0;
			}
			double t = (double)pos / (double)HAKIOS_AUDIO_RATE;
			s->pcm[idx]     = (int16_t)lrint(sin(2.0 * M_PI * 523.25 * t) * 11000.0 * env);
			s->pcm[idx + 1] = (int16_t)lrint(sin(2.0 * M_PI * 659.25 * t) * 11000.0 * env);
		}
		__atomic_store_n(&s->write_frames, w + chunk, __ATOMIC_RELEASE);
		done += chunk;
	}
	usleep(250000);
	uint32_t end_r = __atomic_load_n(&s->read_frames, __ATOMIC_ACQUIRE);
	munmap(s, sizeof(*s));
	printf("adspd tone: complete (daemon consumed %u frames)\n", end_r - start_r);
	return 0;
}

static int
run_daemon(bool is_auto, bool tone_only, unsigned tone_ms)
{
	uint64_t last_status_ms = 0;

	if (is_auto) {
		char bootargs[1024] = "";
		size_t blen = sizeof(bootargs) - 1;

		(void)sysctlbyname("kern.bootargs", bootargs, &blen, NULL, 0);
		if (access(ADSP_OFF_PATH, F_OK) == 0 || strstr(bootargs, "sweetaudio=0") != NULL) {
			fprintf(stderr, "adspd: disabled via %s or sweetaudio=0\n", ADSP_OFF_PATH);
			return 0;
		}
		(void)rename(ADSP_LOG_PATH, ADSP_LOG_PATH ".prev");
		if (freopen(ADSP_LOG_PATH, "a", stdout) != NULL) {
			(void)dup2(fileno(stdout), STDERR_FILENO);
		}
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	escape_jetsam_and_bg();

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_term_signal);
	signal(SIGTERM, on_term_signal);

	memset(&g_status, 0, sizeof(g_status));
	g_status.pid = (int)getpid();
	g_status.smmu_cb = -1;
	g_status.adm_copp_id = -1;
	snprintf(g_status.state, sizeof(g_status.state), "starting");

	/* Create /tmp/sweet-audio.shm (0666) BEFORE waiting on wland so mediaserverd attaches early */
	if (shm_init_ring() != 0) {
		return 1;
	}
	adsp_status_write();

	if (is_auto) {
		wait_for_wland_qrtr();
	}
	if (g_stop_requested) {
		return 0;
	}

	snprintf(g_status.state, sizeof(g_status.state), "booting_adsp");
	adsp_status_write();
	if (adsp_pas_boot() != 0) {
		snprintf(g_status.state, sizeof(g_status.state), "error");
		adsp_status_write();
		if (!is_auto) {
			return 1;
		}
		(void)setpriority(PRIO_PROCESS, 0, 0);
		while (!g_stop_requested) {
			delay_ms(2000);
		}
		return 0;
	}

	snprintf(g_status.state, sizeof(g_status.state), "glink_bringup");
	adsp_status_write();
	if (adsp_glink_bringup(12000) != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error), "GLINK apr_audio_svc timeout");
		snprintf(g_status.state, sizeof(g_status.state), "error");
		adsp_status_write();
		if (!is_auto) {
			return 1;
		}
		(void)setpriority(PRIO_PROCESS, 0, 0);
		while (!g_stop_requested) {
			(void)adsp_glink_poll(250);
		}
		return 0;
	}

	snprintf(g_status.state, sizeof(g_status.state), "q6_bringup");
	adsp_status_write();
	if (adsp_q6_bringup_playback() != 0) {
		snprintf(g_status.state, sizeof(g_status.state), "error");
		adsp_status_write();
		if (!is_auto) {
			return 1;
		}
		(void)setpriority(PRIO_PROCESS, 0, 0);
		while (!g_stop_requested) {
			(void)adsp_glink_poll(250);
		}
		return 0;
	}
	(void)setpriority(PRIO_PROCESS, 0, -2);

	if (tone_only) {
		adsp_log("playing standalone %u ms stereo test tone...", tone_ms);
		adsp_q6_play_tone_Start(tone_ms);
	}

	last_status_ms = now_ms();
	while (!g_stop_requested) {
		(void)adsp_glink_poll(4);
		adsp_q6_tick();

		uint64_t cur = now_ms();
		if (cur - last_status_ms >= 2000ULL) {
			last_status_ms = cur;
			adsp_status_write();
		}
		if (tone_only && !adsp_q6_tone_active()) {
			/* Drain 220 ms so all 8 x 20 ms periods finish playing */
			for (int d = 0; d < 55 && !g_stop_requested; d++) {
				(void)adsp_glink_poll(4);
				adsp_q6_tick();
			}
			break;
		}
	}

	adsp_log("shutting down audio stream cleanly...");
	adsp_q6_teardown();
	snprintf(g_status.state, sizeof(g_status.state), "stopped");
	adsp_status_write();
	return 0;
}

int
main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "auto";

	if (strcmp(cmd, "status") == 0) {
		return cmd_status();
	}
	if (strcmp(cmd, "i2c-scan") == 0) {
		return cmd_i2c_scan();
	}
	if (strcmp(cmd, "shutdown") == 0) {
		return cmd_shutdown();
	}
	if (strcmp(cmd, "voice-test") == 0) {
		unsigned ms = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 600u;
		if (ms < 100u) ms = 100u;
		if (ms > 5000u) ms = 5000u;
		return cmd_voice_test(ms);
	}
	if (strcmp(cmd, "tone") == 0) {
		unsigned ms = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1000u;
		bool streaming = false;
		int dpid;
		if (ms < 100u) ms = 100u;
		if (ms > 15000u) ms = 15000u;
		dpid = find_running_adspd_pid(&streaming);
		if (dpid > 1) {
			for (int w = 0; w < 40 && !streaming; w++) {
				delay_ms(200);
				dpid = find_running_adspd_pid(&streaming);
				if (dpid <= 1) {
					break;
				}
			}
			if (streaming) {
				return inject_tone_via_shm(ms) == 0 ? 0 : 1;
			}
			if (dpid > 1) {
				fprintf(stderr,
				    "adspd tone: daemon (pid %d) is running but stream is not ready\n",
				    dpid);
				return 1;
			}
		}
		return run_daemon(false, true, ms);
	}
	if (strcmp(cmd, "auto") == 0) {
		return run_daemon(true, false, 0);
	}
	if (strcmp(cmd, "up") == 0) {
		return run_daemon(false, false, 0);
	}

	fprintf(stderr,
	    "usage: adspd [auto | up | status | tone [ms] | voice-test [ms] | i2c-scan | shutdown]\n");
	return 2;
}
