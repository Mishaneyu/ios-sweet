/*
 * rmtfs: the modem's EFS lives in the modemst1/modemst2/fsg/fsc partitions,
 * which the modem reaches through this QMI service (14) on the apps side
 * (protocol as github.com/andersson/rmtfs; memory set up as the downstream
 * msm_sharedmem rmtfs client: 2 MB EFS + 1 MB OEM NV backup, guarded by 4K at
 * both ends, shared with the modem's VM by hyp_assign).
 *
 * On Realme X2 Pro (RMX1931 / 19688 / 19781), "/oppo/oem_partion" uses Oppo's
 * CNV container unpacker (0x5a header + oppodycnvbk inode + oppostanvbk inode +
 * 0x190 trailer in the 1 MB OEM window) when MPSS issues RMTFS_RW_IOVEC 0+0.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "link.h"
#include "pmem.h"
#include "qmi.h"
#include "rmtfs.h"
#include "scm.h"

#define RMTFS_DIR	"/var/local/fw/rmtfs"
#define BUF_EFS_SIZE	0x200000u
#define BUF_OEM_SIZE	0x100000u
#define BUF_TOTAL_SIZE	(BUF_EFS_SIZE + BUF_OEM_SIZE)
#define BUF_VERSION	5u
#define SECTOR		512
#define CNV_GOOD_CRC	0xe2f0u

enum { RMTFS_OPEN = 1, RMTFS_CLOSE, RMTFS_RW_IOVEC, RMTFS_ALLOC_BUFF, RMTFS_GET_DEV_ERROR, RMTFS_FORCE_SYNC_IND };

static const struct {
	const char *path;
	const char *file;
	bool optional;
	bool read_only;
} parts[] = {
	{ "/boot/modem_fs1",   "modemst1.img",    false, false },
	{ "/boot/modem_fs2",   "modemst2.img",    false, false },
	{ "/boot/modem_fsg",   "fsg.img",         false, false },
	{ "/boot/modem_fsc",   "fsc.img",         false, false },
	{ "/oppo/oem_partion", "oppostanvbk.img", true,  true  },
	{ "/oem/nvbk/static",  "oppostanvbk.img", true,  true  },
	{ "/oem/nvbk/dynamic", "oppodycnvbk.img", true,  true  },
};
#define NPARTS (sizeof(parts) / sizeof(parts[0]))
#define IDX_OEM_PARTION 4u
#define IDX_OEM_STATIC  5u
#define IDX_OEM_DYNAMIC 6u

static int fds[NPARTS] = { -1, -1, -1, -1, -1, -1, -1 };
static bool eff_ro[NPARTS];
static bool ro_warned[NPARTS];
static uint64_t buf_pa;

static uint8_t oem_cache[BUF_OEM_SIZE];
static uint32_t oem_cache_len;
static bool oem_cache_ready;

static const uint16_t cnv_crc16_tab[256] = {
	0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50a5u, 0x60c6u, 0x70e7u,
	0x8108u, 0x9129u, 0xa14au, 0xb16bu, 0xc18cu, 0xd1adu, 0xe1ceu, 0xf1efu,
	0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52b5u, 0x4294u, 0x72f7u, 0x62d6u,
	0x9339u, 0x8318u, 0xb37bu, 0xa35au, 0xd3bdu, 0xc39cu, 0xf3ffu, 0xe3deu,
	0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64e6u, 0x74c7u, 0x44a4u, 0x5485u,
	0xa56au, 0xb54bu, 0x8528u, 0x9509u, 0xe5eeu, 0xf5cfu, 0xc5acu, 0xd58du,
	0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76d7u, 0x66f6u, 0x5695u, 0x46b4u,
	0xb75bu, 0xa77au, 0x9719u, 0x8738u, 0xf7dfu, 0xe7feu, 0xd79du, 0xc7bcu,
	0x48c4u, 0x58e5u, 0x6886u, 0x78a7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
	0xc9ccu, 0xd9edu, 0xe98eu, 0xf9afu, 0x8948u, 0x9969u, 0xa90au, 0xb92bu,
	0x5af5u, 0x4ad4u, 0x7ab7u, 0x6a96u, 0x1a71u, 0x0a50u, 0x3a33u, 0x2a12u,
	0xdbfdu, 0xcbdcu, 0xfbbfu, 0xeb9eu, 0x9b79u, 0x8b58u, 0xbb3bu, 0xab1au,
	0x6ca6u, 0x7c87u, 0x4ce4u, 0x5cc5u, 0x2c22u, 0x3c03u, 0x0c60u, 0x1c41u,
	0xedaeu, 0xfd8fu, 0xcdecu, 0xddcdu, 0xad2au, 0xbd0bu, 0x8d68u, 0x9d49u,
	0x7e97u, 0x6eb6u, 0x5ed5u, 0x4ef4u, 0x3e13u, 0x2e32u, 0x1e51u, 0x0e70u,
	0xff9fu, 0xefbeu, 0xdfddu, 0xcffcu, 0xbf1bu, 0xaf3au, 0x9f59u, 0x8f78u,
	0x9188u, 0x81a9u, 0xb1cau, 0xa1ebu, 0xd10cu, 0xc12du, 0xf14eu, 0xe16fu,
	0x1080u, 0x00a1u, 0x30c2u, 0x20e3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
	0x83b9u, 0x9398u, 0xa3fbu, 0xb3dau, 0xc33du, 0xd31cu, 0xe37fu, 0xf35eu,
	0x02b1u, 0x1290u, 0x22f3u, 0x32d2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
	0xb5eau, 0xa5cbu, 0x95a8u, 0x8589u, 0xf56eu, 0xe54fu, 0xd52cu, 0xc50du,
	0x34e2u, 0x24c3u, 0x14a0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
	0xa7dbu, 0xb7fau, 0x8799u, 0x97b8u, 0xe75fu, 0xf77eu, 0xc71du, 0xd73cu,
	0x26d3u, 0x36f2u, 0x0691u, 0x16b0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
	0xd94cu, 0xc96du, 0xf90eu, 0xe92fu, 0x99c8u, 0x89e9u, 0xb98au, 0xa9abu,
	0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18c0u, 0x08e1u, 0x3882u, 0x28a3u,
	0xcb7du, 0xdb5cu, 0xeb3fu, 0xfb1eu, 0x8bf9u, 0x9bd8u, 0xabbbu, 0xbb9au,
	0x4a75u, 0x5a54u, 0x6a37u, 0x7a16u, 0x0af1u, 0x1ad0u, 0x2ab3u, 0x3a92u,
	0xfd2eu, 0xed0fu, 0xdd6cu, 0xcd4du, 0xbdaau, 0xad8bu, 0x9de8u, 0x8dc9u,
	0x7c26u, 0x6c07u, 0x5c64u, 0x4c45u, 0x3ca2u, 0x2c83u, 0x1ce0u, 0x0cc1u,
	0xef1fu, 0xff3eu, 0xcf5du, 0xdf7cu, 0xaf9bu, 0xbfbau, 0x8fd9u, 0x9ff8u,
	0x6e17u, 0x7e36u, 0x4e55u, 0x5e74u, 0x2e93u, 0x3eb2u, 0x0ed1u, 0x1ef0u,
};

static bool cnv_crc_valid(const uint8_t *buf, uint32_t len)
{
	if (!len)
		return false;
	uint32_t w8 = ~0u;
	for (uint32_t i = 0; i < len; i++) {
		uint32_t idx = (buf[i] ^ ((w8 >> 8) & 0xffu)) & 0xffu;
		w8 = cnv_crc16_tab[idx] ^ (w8 << 8);
	}
	return ((~w8) & 0xffffu) == CNV_GOOD_CRC;
}

static bool cnv_find_inode(int fd, bool is_dynamic, uint32_t *out_sec, uint32_t *out_sz, uint32_t *out_bid)
{
	uint8_t sb[512];
	if (fd < 0 || pread(fd, sb, sizeof(sb), 0) != (ssize_t)sizeof(sb))
		return false;
	if (!cnv_crc_valid(sb, sizeof(sb)))
		return false;
	uint32_t inode_num = 0;
	memcpy(&inode_num, sb + 24, 4);
	if (inode_num == 0 || inode_num > 512)
		return false;

	uint8_t inodes[512 * 16];
	ssize_t ilen = (ssize_t)(inode_num * 16u);
	if (pread(fd, inodes, (size_t)ilen, 28) != ilen)
		return false;

	/*
	 * RMX1931 (19688 / 19781):
	 *  - oppodycnvbk: vendor rmt_storage redirects 19688_INDIA/EUEX/pcb>=25 -> board_id 15
	 *    (W_G_L_19688_EVT2). Pick the valid-CRC 19688 inode (13..16) with the largest
	 *    populated factory NV payload (falling back to any populated valid-CRC inode).
	 *  - oppostanvbk: prefer 14 (W_G_L_19688_INDIA), then 15 (EVT2), 16 (EUEX), 13 (base).
	 */
	static uint8_t scratch[0x80000];
	if (is_dynamic) {
		uint32_t best_nz = 0, best_sec = 0, best_sz = 0, best_bid = 0;
		for (int pass = 0; pass < 2; pass++) {
			for (uint32_t i = 0; i < inode_num; i++) {
				uint32_t sec, bid, sz;
				memcpy(&sec, inodes + i * 16 + 0, 4);
				memcpy(&bid, inodes + i * 16 + 4, 4);
				memcpy(&sz,  inodes + i * 16 + 8, 4);
				if (pass == 0 && (bid < 13 || bid > 16))
					continue;
				if (sz == 0 || sz > sizeof(scratch))
					continue;
				if (pread(fd, scratch, sz, (off_t)sec * SECTOR) != (ssize_t)sz)
					continue;
				if (!cnv_crc_valid(scratch, sz))
					continue;
				uint32_t nz = 0;
				for (uint32_t k = 0; k < sz; k++)
					if (scratch[k] != 0)
						nz++;
				if (nz > best_nz || (nz == best_nz && bid == 15)) {
					best_nz = nz;
					best_sec = sec;
					best_sz = sz;
					best_bid = bid;
				}
			}
			if (best_sz > 0 && best_nz > 8192)
				break;
		}
		if (best_sz > 0) {
			*out_sec = best_sec;
			*out_sz  = best_sz;
			*out_bid = best_bid;
			return true;
		}
		return false;
	}

	static const uint32_t sta_pref[] = { 14u, 15u, 16u, 13u };
	for (unsigned p = 0; p < sizeof(sta_pref) / sizeof(sta_pref[0]); p++) {
		for (uint32_t i = 0; i < inode_num; i++) {
			uint32_t sec, bid, sz;
			memcpy(&sec, inodes + i * 16 + 0, 4);
			memcpy(&bid, inodes + i * 16 + 4, 4);
			memcpy(&sz,  inodes + i * 16 + 8, 4);
			if (bid != sta_pref[p] || sz == 0 || sz > sizeof(scratch))
				continue;
			if (pread(fd, scratch, sz, (off_t)sec * SECTOR) != (ssize_t)sz)
				continue;
			if (!cnv_crc_valid(scratch, sz))
				continue;
			*out_sec = sec;
			*out_sz  = sz;
			*out_bid = bid;
			return true;
		}
	}
	return false;
}

static void prepare_oppo_oem_cache(void)
{
	if (oem_cache_ready)
		return;
	int dyc_fd = fds[IDX_OEM_DYNAMIC];
	int sta_fd = fds[IDX_OEM_STATIC] >= 0 ? fds[IDX_OEM_STATIC] : fds[IDX_OEM_PARTION];
	if (dyc_fd < 0 && sta_fd < 0)
		return;

	uint32_t dyc_sec = 0, dyc_sz = 0, dyc_bid = 0;
	uint32_t sta_sec = 0, sta_sz = 0, sta_bid = 0;
	bool dyc_ok = cnv_find_inode(dyc_fd, true, &dyc_sec, &dyc_sz, &dyc_bid);
	bool sta_ok = cnv_find_inode(sta_fd, false, &sta_sec, &sta_sz, &sta_bid);
	if (!dyc_ok && !sta_ok) {
		printf("rmtfs: oppo CNV superblock/inode validation failed (dyc_fd=%d sta_fd=%d)\n",
		       dyc_fd, sta_fd);
		return;
	}

	uint16_t extra_sz = (0x10u + dyc_sz + sta_sz + 0x190u <= BUF_OEM_SIZE) ? 0x190u : 0u;
	if (0x10u + dyc_sz + sta_sz > BUF_OEM_SIZE) {
		sta_sz = 0;
		sta_ok = false;
	}
	if (0x10u + dyc_sz > BUF_OEM_SIZE)
		return;

	memset(oem_cache, 0, sizeof(oem_cache));
	oem_cache[0] = 0x5au; /* Oppo oembackup header magic */
	memcpy(oem_cache + 0x04, &dyc_sz, 4);
	memcpy(oem_cache + 0x08, &sta_sz, 4);
	memcpy(oem_cache + 0x0c, &extra_sz, 2);

	uint32_t off = 0x10u;
	if (dyc_ok && dyc_sz > 0) {
		if (pread(dyc_fd, oem_cache + off, dyc_sz, (off_t)dyc_sec * SECTOR) == (ssize_t)dyc_sz)
			off += dyc_sz;
	}
	if (sta_ok && sta_sz > 0) {
		if (pread(sta_fd, oem_cache + off, sta_sz, (off_t)sta_sec * SECTOR) == (ssize_t)sta_sz)
			off += sta_sz;
	}
	if (extra_sz == 0x190u) {
		/*
		 * Oppo buf3 trailer (0x190 B, rmt_storage 0x9930..0xa188):
		 *   +0x000: uint32_t trailer_off
		 *   +0x004: uint16_t pcb_data_sz (0x0100 when valid)
		 *   +0x006: uint8_t  pcb_data[256] = [L][raw[1..L+6]][raw[0x27..0xb6] (IMEI+RSA)]
		 *   +0x188: uint32_t hw_emmcID
		 *   +0x18c: uint8_t  svn_valid (1)
		 *   +0x18d: uint8_t  svn (37)
		 */
		memcpy(oem_cache + off + 0x000, &off, 4);
		oem_cache[off + 0x18c] = 1u;
		oem_cache[off + 0x18d] = 37u;

		int pfd = open(RMTFS_DIR "/oppo_pcba.bin", O_RDONLY);
		if (pfd >= 0) {
			uint8_t pcba[187];
			ssize_t pr = read(pfd, pcba, sizeof(pcba));
			close(pfd);
			if (pr == (ssize_t)sizeof(pcba)) {
				const uint8_t *emmc4 = pcba;
				const uint8_t *raw = pcba + 4;
				uint8_t L = raw[0];
				if (L >= 1u && L <= 60u && (uint32_t)L + 7u + 0x90u <= 256u) {
					uint16_t pcb_sz = 0x0100u;
					uint8_t *pcb_data = oem_cache + off + 0x006;
					memcpy(oem_cache + off + 0x004, &pcb_sz, 2);
					pcb_data[0] = L;
					memcpy(&pcb_data[1], &raw[1], (size_t)L + 6u);
					memcpy(&pcb_data[(size_t)L + 7u], &raw[0x27], 0x90u);
					memcpy(oem_cache + off + 0x188, emmc4, 4);
					printf("rmtfs: loaded oppo_pcba record (len=%u)\n", (unsigned)L);
				} else {
					printf("rmtfs: oppo_pcba invalid length %u\n", (unsigned)L);
				}
			} else {
				printf("rmtfs: oppo_pcba unexpected size %zd\n", pr);
			}
			volatile uint8_t *vp = pcba;
			for (size_t k = 0; k < sizeof(pcba); k++)
				vp[k] = 0;
		} else {
			printf("rmtfs: optional " RMTFS_DIR "/oppo_pcba.bin not present\n");
		}
		off += 0x190u;
	}

	oem_cache_len = (off + 3u) & ~3u;
	oem_cache_ready = true;
	printf("rmtfs: unpacked /oppo/oem_partion CNV (dyc bid=%u %u B, sta bid=%u %u B, total=%u B)\n",
	       dyc_bid, dyc_sz, sta_bid, sta_sz, oem_cache_len);
}

int rmtfs_prepare(void)
{
	for (unsigned i = 0; i < NPARTS; i++) {
		if (fds[i] >= 0)
			continue;
		char p[128];
		snprintf(p, sizeof(p), RMTFS_DIR "/%s", parts[i].file);
		eff_ro[i] = parts[i].read_only;
		fds[i] = open(p, parts[i].read_only ? O_RDONLY : O_RDWR);
		if (fds[i] < 0 && parts[i].optional && !parts[i].read_only &&
		    (errno == EACCES || errno == EROFS)) {
			fds[i] = open(p, O_RDONLY);
			if (fds[i] >= 0)
				eff_ro[i] = true;
		}
		if (fds[i] < 0) {
			if (parts[i].optional && errno == ENOENT) {
				fds[i] = -1;
				printf("rmtfs: optional %s not present\n", p);
				continue;
			}
			printf("rmtfs: %s: %s\n", p, strerror(errno));
			return -1;
		}
		if (parts[i].optional)
			printf("rmtfs: mapped \"%s\" -> %s%s\n",
			       parts[i].path, p, eff_ro[i] ? " (ro)" : "");
	}
	prepare_oppo_oem_cache();

	if (buf_pa) {
		if (oem_cache_ready)
			pm_wr(buf_pa + BUF_EFS_SIZE, oem_cache, oem_cache_len);
		return 0;
	}
	/*
	 * One 3 MB buffer per phone boot (2 MB EFS for callers 1..4 + 1 MB OEM NV backup fo
	 * callers 5..7), kept across modem restarts: a fresh hyp_assign after every restart
	 * (the old ones never handed back) ends with an SMC that never returns.
	 */
	struct timeval bt;
	size_t btl = sizeof(bt);
	sysctlbyname("kern.boottime", &bt, &btl, NULL, 0);
	FILE *f = fopen(RMTFS_DIR "/.buffer", "r");
	long long fb = 0;
	unsigned long long fpa = 0;
	unsigned fver = 0;
	if (f) {
		if (fscanf(f, "%lld %llx %u", &fb, &fpa, &fver) != 3 || fver != BUF_VERSION)
			fpa = 0;
		fclose(f);
	}
	if (fpa && fb == (long long)bt.tv_sec) {
		buf_pa = fpa;
		printf("rmtfs: buffer %llx (v%u, %x bytes) from this boot's first run\n",
		       (unsigned long long)buf_pa, fver, BUF_TOTAL_SIZE);
	} else {
		uint64_t a = pm_alloc(BUF_TOTAL_SIZE + 0x2000, 0x1000);
		if (!a)
			return -1;
		buf_pa = a + 0x1000;
		static const uint32_t src[] = { VMID_HLOS }, dst[] = { VMID_HLOS, VMID_MSS_MSA };
		if (hyp_assign(buf_pa, BUF_TOTAL_SIZE, src, 1, dst, 2))
			return -1;
		if ((f = fopen(RMTFS_DIR "/.buffer", "w"))) {
			fprintf(f, "%lld %llx %u\n", (long long)bt.tv_sec,
			        (unsigned long long)buf_pa, BUF_VERSION);
			fclose(f);
		}
	}
	if (oem_cache_ready)
		pm_wr(buf_pa + BUF_EFS_SIZE, oem_cache, oem_cache_len);
	return 0;
}

static int caller_window(uint32_t caller, uint64_t *out_base, uint32_t *out_max)
{
	if (caller < 1 || caller > NPARTS || !buf_pa)
		return -1;
	if (caller <= 4) {
		*out_base = buf_pa;
		*out_max = BUF_EFS_SIZE;
	} else {
		*out_base = buf_pa + BUF_EFS_SIZE;
		*out_max = BUF_OEM_SIZE;
	}
	return 0;
}

int rmtfs_init(void)
{
	if (rmtfs_prepare())
		return -1;
	if (qrtr_new_server(RMTFS_SERVICE, 1, 0, RMTFS_PORT))
		return -1;
	printf("rmtfs: serving at %u:%x, buffer %llx (efs=%x oem=%x)\n",
	       QRTR_NODE_APPS, RMTFS_PORT, (unsigned long long)buf_pa, BUF_EFS_SIZE, BUF_OEM_SIZE);
	return 0;
}

static void reply(const struct qrtr_hdr *h, struct qmi_buf *q)
{
	qrtr_send(QRTR_TYPE_DATA, RMTFS_PORT, h->src_node, h->src_port, q->b, (uint32_t)qmi_finish(q));
}

static int iovec(uint32_t caller, uint8_t dir, const uint8_t *v, uint16_t len)
{
	uint64_t cbase = 0;
	uint32_t cmax = 0;
	if (caller_window(caller, &cbase, &cmax))
		return -1;
	unsigned idx = caller - 1;

	/*
	 * Oppo "/oppo/oem_partion" CNV protocol (vendor rmt_storage 0xa19c / 0x9d2c):
	 * MPSS sends sectors 0+0 (dir=0) expecting the unpacked 0x5a header + dycnvbk +
	 * stanvbk + 0x190 trailer in oembackup shared memory (cbase).
	 */
	if (idx == IDX_OEM_PARTION) {
		if (dir == 0) {
			prepare_oppo_oem_cache();
			if (!oem_cache_ready)
				return -1;
			pm_wr(cbase, oem_cache, oem_cache_len);
			printf("rmtfs: populated /oppo/oem_partion -> %llx (%u bytes)\n",
			       (unsigned long long)cbase, oem_cache_len);
			return 0;
		}
		if (!ro_warned[idx]) {
			ro_warned[idx] = true;
			printf("rmtfs: ignoring write to read-only /oppo/oem_partion\n");
		}
		return 0;
	}

	int fd = fds[idx];
	if (fd < 0)
		return -1;
	unsigned hdr = (len % 12 == 1) ? 1 : (len % 12 == 2) ? 2 : 4;
	if (len < hdr)
		return -1;
	unsigned n = (len - hdr) / 12;
	static uint8_t tmp[BUF_EFS_SIZE];
	for (unsigned i = 0; i < n; i++) {
		uint32_t e[3];
		memcpy(e, v + hdr + 12 * i, 12);
		uint64_t off = (uint64_t)e[0] * SECTOR, bytes = (uint64_t)e[2] * SECTOR;
		/* phys_offset: an address in this caller's window, or a relative offset into it */
		uint64_t pa = e[1] >= (uint32_t)cbase ? (cbase & ~0xffffffffull) + e[1] : cbase + e[1];
		if (bytes > cmax || bytes > sizeof(tmp) || pa < cbase || pa + bytes > cbase + cmax) {
			printf("rmtfs: iovec out of caller %u window (%x %x %x, base %llx max %x)\n",
			       caller, e[0], e[1], e[2], (unsigned long long)cbase, cmax);
			return -1;
		}
		if (dir == 0) {
			ssize_t r = pread(fd, tmp, bytes, (off_t)off);
			if (r < (ssize_t)bytes)
				memset(tmp + (r > 0 ? r : 0), 0, bytes - (r > 0 ? r : 0));
			pm_wr(pa, tmp, bytes);
		} else if (eff_ro[idx]) {
			if (!ro_warned[idx]) {
				ro_warned[idx] = true;
				printf("rmtfs: ignoring write to read-only %s (sectors %u+%u)\n",
				       parts[idx].file, e[0], e[2]);
			}
			continue;
		} else {
			pm_rd(pa, tmp, bytes);
			if (pwrite(fd, tmp, bytes, (off_t)off) != (ssize_t)bytes)
				return -1;
		}
		printf("rmtfs: %s %s sectors %u+%u\n", dir ? "write" : "read", parts[idx].file, e[0], e[2]);
	}
	if (dir && !eff_ro[idx])
		fsync(fd);
	return 0;
}

void rmtfs_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	uint32_t caller = 0;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	qmi_get_u32(&m, 1, &caller);
	switch (m.id) {
	case RMTFS_OPEN: {
		uint16_t l;
		const uint8_t *p = qmi_get(&m, 1, &l);
		char path[64] = { 0 };
		if (p)
			memcpy(path, p, l < 63 ? l : 63);
		unsigned i;
		for (i = 0; i < NPARTS; i++) {
			if (!strcmp(path, parts[i].path)) {
				if (i == IDX_OEM_PARTION && (fds[IDX_OEM_PARTION] >= 0 || fds[IDX_OEM_DYNAMIC] >= 0))
					break;
				if (fds[i] >= 0)
					break;
			}
		}
		printf("rmtfs: open \"%s\" -> %d\n", path, i < NPARTS ? (int)i + 1 : -1);
		if (i < NPARTS) {
			qmi_tlv_result(&q, 0, 0);
			qmi_tlv_u32(&q, 0x10, i + 1);
		} else {
			qmi_tlv_result(&q, 1, 3);
		}
		break;
	}
	case RMTFS_CLOSE:
		printf("rmtfs: close %u\n", caller);
		qmi_tlv_result(&q, 0, 0);
		break;
	case RMTFS_ALLOC_BUFF: {
		uint32_t size = 0;
		uint64_t cbase = 0;
		uint32_t cmax = 0;
		qmi_get_u32(&m, 2, &size);
		int ok = (caller_window(caller, &cbase, &cmax) == 0 && size <= cmax);
		printf("rmtfs: alloc %u bytes for caller %u -> %llx (max %x)%s\n",
		       size, caller, (unsigned long long)cbase, cmax, ok ? "" : " REJECTED");
		if (ok) {
			qmi_tlv_result(&q, 0, 0);
			qmi_tlv_u64(&q, 0x10, cbase);
		} else {
			qmi_tlv_result(&q, 1, 3);
		}
		break;
	}
	case RMTFS_RW_IOVEC: {
		uint8_t dir = 0;
		uint16_t l = 0;
		qmi_get_u8(&m, 2, &dir);
		const uint8_t *v = qmi_get(&m, 3, &l);
		int r = v ? iovec(caller, dir, v, l) : -1;
		qmi_tlv_result(&q, r ? 1 : 0, r ? 3 : 0);
		break;
	}
	case RMTFS_GET_DEV_ERROR:
		qmi_tlv_result(&q, 0, 0);
		qmi_tlv_u8(&q, 0x10, 0);
		break;
	default:
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);	/* QMI_ERR_NOT_SUPPORTED */
		break;
	}
	reply(h, &q);
}
