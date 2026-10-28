/*
 * SPDX-License-Identifier: MIT
 *
 * adsp_glink.c: Multi-channel GLINK over SMEM (remote_pid=2, items 478/479/480,
 * APCS doorbell bit 8) for LPASS ADSP, with embedded QRTR peer on "IPCRTR"
 * (pd-mapper service 0x40 + read-only TFTP service 4096) and APR packet
 * transport on "apr_audio_svc".
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "adspd.h"

/* ---- GLINK wire commands ---- */
#define GLINK_CMD_VERSION           0u
#define GLINK_CMD_VERSION_ACK       1u
#define GLINK_CMD_OPEN              2u
#define GLINK_CMD_CLOSE             3u
#define GLINK_CMD_OPEN_ACK          4u
#define GLINK_CMD_INTENT            5u
#define GLINK_CMD_RX_DONE           6u
#define GLINK_CMD_RX_INTENT_REQ     7u
#define GLINK_CMD_RX_INTENT_REQ_ACK 8u
#define GLINK_CMD_TX_DATA           9u
#define GLINK_CMD_CLOSE_ACK         11u
#define GLINK_CMD_TX_DATA_CONT      12u
#define GLINK_CMD_READ_NOTIF        13u
#define GLINK_CMD_RX_DONE_W_REUSE   14u
#define GLINK_CMD_SIGNALS           15u

#define GLINK_FEAT_INTENT_REUSE     1u

#define SMEM_GLINK_DESC_ITEM        478u
#define SMEM_GLINK_TX_ITEM          479u
#define SMEM_GLINK_RX_ITEM          480u

#define CH_IDX_IPCRTR               0
#define CH_IDX_APR                  1
#define CH_IDX_FASTRPC              2
#define ACTIVE_CH_COUNT             3

#define N_LOCAL_INTENTS             12
#define LOCAL_INTENT_SIZE           4096u
#define MAX_REMOTE_INTENTS          128

struct remote_intent {
	uint32_t size;
	uint32_t iid;
	bool     busy;
};

struct glink_chan {
	const char          *name;
	uint16_t             lcid;
	uint16_t             rcid;
	bool                 open_tx;
	bool                 open_acked;
	bool                 opened_by_remote;
	bool                 intent_req_acked;
	uint32_t             next_iid;
	int                  nrintent;
	struct remote_intent rintent[MAX_REMOTE_INTENTS];
	uint32_t             rxoff;
	uint8_t              rxbuf[32768];
};

static uint64_t desc_pa, tx_fifo_pa, rx_fifo_pa;
static uint32_t tx_fifo_len, rx_fifo_len;
static bool version_acked;
static uint64_t last_rx_tx_ms;

static struct glink_chan chans[ACTIVE_CH_COUNT] = {
	[CH_IDX_IPCRTR]  = { .name = "IPCRTR",                .lcid = 1, .next_iid = 100 },
	[CH_IDX_APR]     = { .name = "apr_audio_svc",         .lcid = 2, .next_iid = 100 },
	[CH_IDX_FASTRPC] = { .name = "fastrpcglink-apps-dsp", .lcid = 3, .next_iid = 100 },
};

#define DESC_TX_TAIL (desc_pa + 0ULL)
#define DESC_TX_HEAD (desc_pa + 4ULL)
#define DESC_RX_TAIL (desc_pa + 8ULL)
#define DESC_RX_HEAD (desc_pa + 12ULL)

/* Forward declarations for embedded QRTR & FastRPC services */
static void qrtr_on_packet(const uint8_t *pkt, uint32_t len);
static void pdmapper_load_jsn_db(void);
static void fastrpc_on_packet(const uint8_t *pkt, uint32_t len);

int
adsp_glink_prepare(void)
{
	static const uint8_t z4k[4096];
	static const uint8_t z32[32];
	uint64_t dpa = 0, tpa = 0, rpa = 0;
	uint32_t tlen = 0, rlen = 0;

	if (smem_alloc_item(SMEM_HOST_ADSP, SMEM_GLINK_DESC_ITEM, 32, &dpa) != 0 ||
	    smem_alloc_item(SMEM_HOST_ADSP, SMEM_GLINK_TX_ITEM, 16384, &tpa) != 0 ||
	    smem_get_item(SMEM_HOST_ADSP, SMEM_GLINK_TX_ITEM, &tpa, &tlen) != 0) {
		adsp_log("glink: failed to allocate SMEM items 478/479 in ADSP partition");
		return -1;
	}
	/*
	 * Zero GLINK descriptor (TX_TAIL/TX_HEAD/RX_TAIL/RX_HEAD) and FIFOs
	 * BEFORE PAS_AUTH_AND_RESET while ADSP is held in reset so warm reboots
	 * from TWRP/Linux never leave non-zero ring indices in SMEM.
	 */
	(void)pm_wr(dpa, z32, sizeof(z32));
	for (uint32_t o = 0; o < tlen; o += sizeof(z4k)) {
		uint32_t n = tlen - o < sizeof(z4k) ? tlen - o : (uint32_t)sizeof(z4k);
		(void)pm_wr(tpa + o, z4k, n);
	}
	if (smem_get_item(SMEM_HOST_ADSP, SMEM_GLINK_RX_ITEM, &rpa, &rlen) == 0 && rlen > 0) {
		for (uint32_t o = 0; o < rlen; o += sizeof(z4k)) {
			uint32_t n = rlen - o < sizeof(z4k) ? rlen - o : (uint32_t)sizeof(z4k);
			(void)pm_wr(rpa + o, z4k, n);
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");

	desc_pa = dpa;
	tx_fifo_pa = tpa;
	tx_fifo_len = tlen;
	pdmapper_load_jsn_db();
	return 0;
}

static int
glink_ring_init(void)
{
	if ((desc_pa == 0 || tx_fifo_len == 0) && adsp_glink_prepare() != 0) {
		return -1;
	}
	for (int i = 0; i < 50; i++) {
		if (smem_get_item(SMEM_HOST_ADSP, SMEM_GLINK_RX_ITEM, &rx_fifo_pa, &rx_fifo_len) == 0 &&
		    rx_fifo_len > 0) {
			break;
		}
		delay_ms(20);
	}
	if (rx_fifo_pa == 0 || rx_fifo_len == 0) {
		adsp_log("glink: ADSP has not created RX FIFO (item 480)");
		return -1;
	}

	version_acked = false;
	for (int c = 0; c < ACTIVE_CH_COUNT; c++) {
		chans[c].rcid = 0;
		chans[c].open_tx = false;
		chans[c].open_acked = false;
		chans[c].opened_by_remote = false;
		chans[c].intent_req_acked = false;
		chans[c].next_iid = 100;
		chans[c].nrintent = 0;
		chans[c].rxoff = 0;
	}

	/* Note: DESC_TX_HEAD and DESC_RX_TAIL were zeroed in adsp_glink_prepare() before reset */
	adsp_log("glink: rings ready desc=0x%llx tx=0x%llx(%u) rx=0x%llx(%u) idx tx=%u/%u rx=%u/%u",
	    (unsigned long long)desc_pa,
	    (unsigned long long)tx_fifo_pa, tx_fifo_len,
	    (unsigned long long)rx_fifo_pa, rx_fifo_len,
	    rd32(DESC_TX_TAIL), rd32(DESC_TX_HEAD),
	    rd32(DESC_RX_TAIL), rd32(DESC_RX_HEAD));
	return 0;
}

static uint32_t
glink_tx_write(uint32_t head, const uint8_t *data, size_t n)
{
	size_t first = n <= tx_fifo_len - head ? n : tx_fifo_len - head;

	if (first > 0) {
		(void)pm_wr(tx_fifo_pa + head, data, first);
	}
	if (n > first) {
		(void)pm_wr(tx_fifo_pa, data + first, n - first);
	}
	head += (uint32_t)n;
	if (head >= tx_fifo_len) {
		head -= tx_fifo_len;
	}
	return head;
}

static int
glink_tx_cmd(uint16_t cmd, uint16_t p1, uint32_t p2, const void *payload, size_t len)
{
	uint8_t buf[8 + 8 + 8192 + 8];
	size_t total = 8 + len;
	size_t padded = (total + 7u) & ~(size_t)7u;
	uint32_t head, tail, avail;

	if (padded > sizeof(buf)) {
		return -1;
	}
	memset(buf, 0, padded);
	memcpy(buf + 0, &cmd, 2);
	memcpy(buf + 2, &p1, 2);
	memcpy(buf + 4, &p2, 4);
	if (len > 0 && payload != NULL) {
		memcpy(buf + 8, payload, len);
	}

	head = rd32(DESC_TX_HEAD);
	tail = rd32(DESC_TX_TAIL);
	avail = tail <= head ? (tx_fifo_len - head + tail) : (tail - head);
	if (avail < padded + 16u) {
		return -1;
	}

	head = glink_tx_write(head, buf, padded);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DESC_TX_HEAD, head);
	apcs_kick(IPC_BIT_ADSP_GLINK);
	last_rx_tx_ms = now_ms();
	return 0;
}

static size_t
glink_rx_avail_bytes(void)
{
	uint32_t head = rd32(DESC_RX_HEAD);
	uint32_t tail = rd32(DESC_RX_TAIL);

	if (head >= rx_fifo_len || tail >= rx_fifo_len) {
		return 0;
	}
	return head < tail ? (rx_fifo_len - tail + head) : (head - tail);
}

static void
glink_rx_peek(void *dst, size_t off, size_t len)
{
	uint8_t *b = dst;
	uint32_t t;
	size_t done = 0;

	if (len == 0) {
		return;
	}
	t = rd32(DESC_RX_TAIL) + (uint32_t)off;
	while (t >= rx_fifo_len) {
		t -= rx_fifo_len;
	}
	while (done < len) {
		if ((t & 3u) != 0 || (len - done) < 4u) {
			uint32_t w = rd32(rx_fifo_pa + (t & ~3u));
			b[done++] = (uint8_t)(w >> (8u * (t & 3u)));
			if (++t >= rx_fifo_len) {
				t = 0;
			}
		} else {
			size_t rem = len - done;
			size_t contig = rx_fifo_len - t;
			size_t chunk = (rem < contig ? rem : contig) & ~(size_t)3u;
			(void)pm_rd(rx_fifo_pa + t, b + done, chunk);
			done += chunk;
			t += (uint32_t)chunk;
			if (t >= rx_fifo_len) {
				t -= rx_fifo_len;
			}
		}
	}
}

static void
glink_rx_advance(size_t len)
{
	uint32_t t = rd32(DESC_RX_TAIL) + (uint32_t)len;

	while (t >= rx_fifo_len) {
		t -= rx_fifo_len;
	}
	wr32(DESC_RX_TAIL, t);
}

static struct glink_chan *
chan_by_lcid(uint16_t lcid)
{
	for (int i = 0; i < ACTIVE_CH_COUNT; i++) {
		if (chans[i].lcid == lcid) {
			return &chans[i];
		}
	}
	return NULL;
}

static struct glink_chan *
chan_by_rcid(uint16_t rcid)
{
	if (rcid == 0) {
		return NULL;
	}
	for (int i = 0; i < ACTIVE_CH_COUNT; i++) {
		if (chans[i].opened_by_remote && chans[i].rcid == rcid) {
			return &chans[i];
		}
	}
	return NULL;
}

static void
chan_send_open(struct glink_chan *ch)
{
	size_t nlen = strlen(ch->name) + 1;

	if (!ch->open_tx) {
		if (glink_tx_cmd(GLINK_CMD_OPEN, ch->lcid, (uint32_t)nlen, ch->name, nlen) == 0) {
			ch->open_tx = true;
			adsp_log("glink: > OPEN lcid=%u '%s'", ch->lcid, ch->name);
		}
	}
}

static void
chan_advertise_intents(struct glink_chan *ch)
{
	uint32_t pairs[N_LOCAL_INTENTS * 2];

	for (int i = 0; i < N_LOCAL_INTENTS; i++) {
		pairs[2 * i] = LOCAL_INTENT_SIZE;
		pairs[2 * i + 1] = (uint32_t)(i + 1);
	}
	(void)glink_tx_cmd(GLINK_CMD_INTENT, ch->lcid, N_LOCAL_INTENTS, pairs, sizeof(pairs));
}

static int
glink_rx_one(void)
{
	size_t avail = glink_rx_avail_bytes();
	uint8_t hdr[16];
	uint16_t cmd, p1;
	uint32_t p2;
	size_t adv = 8;

	if (avail < 8) {
		return 0;
	}
	glink_rx_peek(hdr, 0, 8);
	memcpy(&cmd, hdr + 0, 2);
	memcpy(&p1, hdr + 2, 2);
	memcpy(&p2, hdr + 4, 4);

	switch (cmd) {
	case GLINK_CMD_VERSION:
		adsp_log("glink: < VERSION %u features=0x%x", p1, p2);
		(void)glink_tx_cmd(GLINK_CMD_VERSION_ACK, 1, p2 & GLINK_FEAT_INTENT_REUSE, NULL, 0);
		if (!version_acked) {
			(void)glink_tx_cmd(GLINK_CMD_VERSION, 1, GLINK_FEAT_INTENT_REUSE, NULL, 0);
		}
		break;

	case GLINK_CMD_VERSION_ACK:
		adsp_log("glink: < VERSION_ACK %u features=0x%x", p1, p2);
		version_acked = true;
		break;

	case GLINK_CMD_OPEN: {
		size_t nl = (p2 + 7u) & ~(size_t)7u;
		char name[64];

		if (avail < 8 + nl) {
			return 0;
		}
		memset(name, 0, sizeof(name));
		glink_rx_peek(name, 8, p2 < sizeof(name) - 1 ? p2 : sizeof(name) - 1);
		adv += nl;
		adsp_log("glink: < OPEN rcid=%u '%s'", p1, name);
		/* Always ACK every remote channel so ADSP subsystems never block */
		(void)glink_tx_cmd(GLINK_CMD_OPEN_ACK, p1, 0, NULL, 0);
		for (int i = 0; i < ACTIVE_CH_COUNT; i++) {
			if (strcmp(name, chans[i].name) == 0) {
				chans[i].rcid = p1;
				chans[i].opened_by_remote = true;
				chan_send_open(&chans[i]);
				if (i == CH_IDX_IPCRTR && chans[i].open_acked) {
					g_status.glink_ipcrtr_up = true;
				}
				if (i == CH_IDX_APR && chans[i].open_acked) {
					g_status.glink_apr_up = true;
				}
				if (i == CH_IDX_FASTRPC && chans[i].open_acked) {
					g_status.glink_fastrpc_up = true;
				}
			}
		}
		break;
	}

	case GLINK_CMD_OPEN_ACK: {
		struct glink_chan *ch = chan_by_lcid(p1);
		if (ch != NULL && !ch->open_acked) {
			ch->open_acked = true;
			chan_advertise_intents(ch);
			adsp_log("glink: < OPEN_ACK lcid=%u ('%s')", p1, ch->name);
			if (ch == &chans[CH_IDX_IPCRTR] && ch->opened_by_remote) {
				g_status.glink_ipcrtr_up = true;
			}
			if (ch == &chans[CH_IDX_APR] && ch->opened_by_remote) {
				g_status.glink_apr_up = true;
			}
			if (ch == &chans[CH_IDX_FASTRPC] && ch->opened_by_remote) {
				g_status.glink_fastrpc_up = true;
			}
		}
		break;
	}

	case GLINK_CMD_CLOSE: {
		struct glink_chan *ch = chan_by_rcid(p1);
		(void)glink_tx_cmd(GLINK_CMD_CLOSE_ACK, p1, 0, NULL, 0);
		if (ch != NULL) {
			adsp_log("glink: < CLOSE rcid=%u ('%s')", p1, ch->name);
			ch->opened_by_remote = false;
			ch->nrintent = 0;
			if (ch == &chans[CH_IDX_APR]) {
				g_status.glink_apr_up = false;
			}
			if (ch == &chans[CH_IDX_FASTRPC]) {
				g_status.glink_fastrpc_up = false;
			}
		}
		break;
	}

	case GLINK_CMD_CLOSE_ACK:
		break;

	case GLINK_CMD_INTENT: {
		size_t len = 8u + (size_t)p2 * 8u;
		struct glink_chan *ch;

		if (avail < len) {
			return 0;
		}
		ch = chan_by_rcid(p1);
		for (uint32_t i = 0; i < p2; i++) {
			uint32_t pr[2];
			glink_rx_peek(pr, 8u + (size_t)i * 8u, 8);
			if (ch != NULL && ch->nrintent < MAX_REMOTE_INTENTS) {
				ch->rintent[ch->nrintent].size = pr[0];
				ch->rintent[ch->nrintent].iid = pr[1];
				ch->rintent[ch->nrintent].busy = false;
				ch->nrintent++;
			}
		}
		adv = (len + 7u) & ~(size_t)7u;
		break;
	}

	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE: {
		struct glink_chan *ch = chan_by_rcid(p1);
		if (ch != NULL) {
			for (int i = 0; i < ch->nrintent; i++) {
				if (ch->rintent[i].iid == p2) {
					if (cmd == GLINK_CMD_RX_DONE) {
						ch->rintent[i] = ch->rintent[--ch->nrintent];
					} else {
						ch->rintent[i].busy = false;
					}
					break;
				}
			}
		}
		break;
	}

	case GLINK_CMD_RX_INTENT_REQ: {
		struct glink_chan *ch = chan_by_rcid(p1);
		if (ch != NULL) {
			uint32_t sz = p2 > LOCAL_INTENT_SIZE ? p2 : LOCAL_INTENT_SIZE;
			uint32_t pr[2] = { sz, ch->next_iid++ };
			(void)glink_tx_cmd(GLINK_CMD_INTENT, ch->lcid, 1, pr, sizeof(pr));
			(void)glink_tx_cmd(GLINK_CMD_RX_INTENT_REQ_ACK, ch->lcid, 1, NULL, 0);
		} else {
			(void)glink_tx_cmd(GLINK_CMD_RX_INTENT_REQ_ACK, 0, 0, NULL, 0);
		}
		break;
	}

	case GLINK_CMD_RX_INTENT_REQ_ACK: {
		struct glink_chan *ch = chan_by_rcid(p1);
		if (ch != NULL) {
			ch->intent_req_acked = true;
		}
		break;
	}

	case GLINK_CMD_TX_DATA:
	case GLINK_CMD_TX_DATA_CONT: {
		uint32_t chunk, left;
		size_t pkt_len;
		struct glink_chan *ch;

		if (avail < 16) {
			return 0;
		}
		glink_rx_peek(hdr, 0, 16);
		memcpy(&chunk, hdr + 8, 4);
		memcpy(&left, hdr + 12, 4);
		pkt_len = 16u + (size_t)chunk;
		if (avail < pkt_len) {
			return 0;
		}
		adv = (pkt_len + 7u) & ~(size_t)7u;
		ch = chan_by_rcid(p1);
		if (ch == NULL) {
			glink_rx_advance(adv);
			last_rx_tx_ms = now_ms();
			return 1;
		}
		if (ch->rxoff + chunk <= sizeof(ch->rxbuf)) {
			glink_rx_peek(ch->rxbuf + ch->rxoff, 16, chunk);
			ch->rxoff += chunk;
		}
		if (left == 0) {
			uint32_t msg_len = ch->rxoff;
			uint8_t dispatch_buf[8192];
			uint8_t *ptr = ch->rxbuf;

			ch->rxoff = 0;
			if (msg_len <= sizeof(dispatch_buf)) {
				memcpy(dispatch_buf, ch->rxbuf, msg_len);
				ptr = dispatch_buf;
			}
			glink_rx_advance(adv);
			(void)glink_tx_cmd(GLINK_CMD_RX_DONE_W_REUSE, ch->lcid, p2, NULL, 0);
			last_rx_tx_ms = now_ms();
			if (ch == &chans[CH_IDX_IPCRTR]) {
				qrtr_on_packet(ptr, msg_len);
			} else if (ch == &chans[CH_IDX_APR]) {
				adsp_apr_rx(ptr, msg_len);
			} else if (ch == &chans[CH_IDX_FASTRPC]) {
				fastrpc_on_packet(ptr, msg_len);
			}
			return 1;
		}
		break;
	}

	case GLINK_CMD_READ_NOTIF:
		apcs_kick(IPC_BIT_ADSP_GLINK);
		break;

	case GLINK_CMD_SIGNALS:
		break;

	default:
		adsp_log("glink: unknown cmd %u (p1=%u p2=0x%x), flushing %zu B", cmd, p1, p2, avail);
		adv = avail;
		break;
	}

	glink_rx_advance(adv);
	last_rx_tx_ms = now_ms();
	return 1;
}

static int
chan_find_intent(struct glink_chan *ch, uint32_t len)
{
	int best = -1;

	for (int i = 0; i < ch->nrintent; i++) {
		if (!ch->rintent[i].busy && ch->rintent[i].size >= len) {
			if (best < 0 || ch->rintent[i].size < ch->rintent[best].size) {
				best = i;
			}
		}
	}
	return best;
}

static int
chan_send_pkt(struct glink_chan *ch, const void *data, uint32_t len)
{
	uint64_t end;
	int slot, asked = 0;
	const uint8_t *d = data;

	if (!ch->open_acked || !ch->opened_by_remote) {
		return -1;
	}
	end = now_ms() + 2500ULL;
	while ((slot = chan_find_intent(ch, len)) < 0) {
		if (asked == 0 || ch->intent_req_acked) {
			if (asked > 3) {
				return -1;
			}
			ch->intent_req_acked = false;
			(void)glink_tx_cmd(GLINK_CMD_RX_INTENT_REQ, ch->lcid, len, NULL, 0);
			asked++;
		}
		if (now_ms() >= end) {
			adsp_log("glink: '%s' TX timeout waiting for %u-byte intent", ch->name, len);
			return -1;
		}
		while (glink_rx_one() > 0) {
		}
		usleep(500);
	}

	ch->rintent[slot].busy = true;
	for (uint32_t off = 0; off < len;) {
		uint32_t chunk = len - off > 8192u ? 8192u : (len - off);
		uint8_t b[8 + 8192];
		uint32_t hdr[2] = { chunk, len - off - chunk };

		memcpy(b, hdr, 8);
		memcpy(b + 8, d + off, chunk);
		while (glink_tx_cmd(off ? GLINK_CMD_TX_DATA_CONT : GLINK_CMD_TX_DATA,
		    ch->lcid, ch->rintent[slot].iid, b, 8u + chunk) != 0) {
			if (now_ms() >= end) {
				return -1;
			}
			while (glink_rx_one() > 0) {
			}
			usleep(500);
		}
		off += chunk;
	}
	return 0;
}

int
adsp_apr_send(const void *pkt, uint32_t len)
{
	return chan_send_pkt(&chans[CH_IDX_APR], pkt, len);
}

bool
adsp_apr_is_open(void)
{
	return chans[CH_IDX_APR].open_acked && chans[CH_IDX_APR].opened_by_remote;
}

int
adsp_glink_poll(unsigned wait_ms)
{
	uint64_t end = now_ms() + wait_ms;
	int count = 0;

	do {
		smp2p_adsp_service();
		while (glink_rx_one() > 0) {
			count++;
		}
		if (wait_ms == 0 || now_ms() >= end) {
			break;
		}
		uint64_t cur = now_ms();
		unsigned step = (cur - last_rx_tx_ms < 40ULL) ? 1u : 4u;
		if (end - cur < step) {
			step = (unsigned)(end - cur);
		}
		if (step > 0) {
			delay_ms(step);
		}
	} while (now_ms() < end && !g_stop_requested);
	return count;
}

int
adsp_glink_bringup(unsigned timeout_ms)
{
	uint64_t start, end;
	bool opened_ipcrtr_proactive = false;
	bool opened_apr_proactive = false;
	bool rekick_500 = false, rekick_2000 = false;

	if (glink_ring_init() != 0) {
		return -1;
	}
	(void)glink_tx_cmd(GLINK_CMD_VERSION, 1, GLINK_FEAT_INTENT_REUSE, NULL, 0);

	start = now_ms();
	end = start + timeout_ms;
	while (now_ms() < end && !g_stop_requested) {
		uint64_t elapsed;

		(void)adsp_glink_poll(10);
		elapsed = now_ms() - start;
		if (!version_acked && !rekick_500 && elapsed > 500ULL) {
			rekick_500 = true;
			if (rd32(DESC_TX_HEAD) == 0) {
				(void)glink_tx_cmd(GLINK_CMD_VERSION, 1, GLINK_FEAT_INTENT_REUSE, NULL, 0);
			} else {
				apcs_kick(IPC_BIT_ADSP_GLINK);
			}
		}
		if (!version_acked && !rekick_2000 && elapsed > 2000ULL) {
			rekick_2000 = true;
			apcs_kick(IPC_BIT_ADSP_GLINK);
		}
		if (version_acked && !opened_ipcrtr_proactive && elapsed > 150ULL) {
			opened_ipcrtr_proactive = true;
			chan_send_open(&chans[CH_IDX_IPCRTR]);
		}
		if (g_status.glink_ipcrtr_up && !opened_apr_proactive && elapsed > 600ULL) {
			opened_apr_proactive = true;
			chan_send_open(&chans[CH_IDX_APR]);
		}
		if (g_status.glink_ipcrtr_up && g_status.glink_apr_up) {
			adsp_log("glink: IPCRTR (rcid=%u) and apr_audio_svc (rcid=%u) UP",
			    chans[CH_IDX_IPCRTR].rcid, chans[CH_IDX_APR].rcid);
			return 0;
		}
	}

	adsp_log("glink: bringup status after %u ms: ver=%d IPCRTR=%d apr_audio_svc=%d fastrpc=%d idx tx=%u/%u rx=%u/%u",
	    timeout_ms, version_acked ? 1 : 0,
	    g_status.glink_ipcrtr_up ? 1 : 0,
	    g_status.glink_apr_up ? 1 : 0,
	    g_status.glink_fastrpc_up ? 1 : 0,
	    rd32(DESC_TX_TAIL), rd32(DESC_TX_HEAD),
	    rd32(DESC_RX_TAIL), rd32(DESC_RX_HEAD));
	return g_status.glink_apr_up ? 0 : -1;
}

/* ============================================================================
 * Embedded QRTR Peer on ADSP "IPCRTR": pd-mapper (0x40) + Read-Only TFTP (4096)
 * ============================================================================ */

#define QRTR_NODE_APPS          1u
#define QRTR_PORT_CTRL          0xfffffffeu

#define QRTR_TYPE_DATA          1u
#define QRTR_TYPE_HELLO         2u
#define QRTR_TYPE_NEW_SERVER    4u
#define QRTR_TYPE_RESUME_TX     7u

#define ADSP_PDMAPPER_SVC       0x40u
#define ADSP_PDMAPPER_PORT      0x11u

#define ADSP_TFTP_SVC           4096u
#define ADSP_TFTP_PORT          0x12u
#define ADSP_TFTP_XFER_PORT0    0x100u
#define ADSP_TFTP_XFERS         8
#define ADSP_TFTP_MAX_BLKSIZE   1024u

struct qrtr_pkt_hdr {
	uint32_t version;
	uint32_t type;
	uint32_t src_node;
	uint32_t src_port;
	uint32_t confirm_rx;
	uint32_t size;
	uint32_t dst_node;
	uint32_t dst_port;
};

static uint32_t adsp_peer_node = 2u;
static bool qrtr_servers_announced;

static int
qrtr_tx(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
    const void *payload, uint32_t len)
{
	uint8_t buf[sizeof(struct qrtr_pkt_hdr) + 8192];
	struct qrtr_pkt_hdr hd = {
		.version = 1,
		.type = type,
		.src_node = QRTR_NODE_APPS,
		.src_port = src_port,
		.confirm_rx = 0,
		.size = len,
		.dst_node = dst_node,
		.dst_port = dst_port,
	};
	uint32_t padded = (len + 3u) & ~3u;

	if (sizeof(hd) + padded > sizeof(buf)) {
		return -1;
	}
	memset(buf, 0, sizeof(hd) + padded);
	memcpy(buf, &hd, sizeof(hd));
	if (len > 0 && payload != NULL) {
		memcpy(buf + sizeof(hd), payload, len);
	}
	return chan_send_pkt(&chans[CH_IDX_IPCRTR], buf, (uint32_t)sizeof(hd) + padded);
}

static void
qrtr_announce_servers(uint32_t peer_node)
{
	uint32_t pd_srv[5] = {
		QRTR_TYPE_NEW_SERVER,
		ADSP_PDMAPPER_SVC,
		1u | (1u << 8), /* v1, instance 1 */
		QRTR_NODE_APPS,
		ADSP_PDMAPPER_PORT,
	};
	uint32_t tftp_srv[5] = {
		QRTR_TYPE_NEW_SERVER,
		ADSP_TFTP_SVC,
		1u | (0u << 8), /* v1, instance 0 */
		QRTR_NODE_APPS,
		ADSP_TFTP_PORT,
	};

	(void)qrtr_tx(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, peer_node, QRTR_PORT_CTRL,
	    pd_srv, sizeof(pd_srv));
	(void)qrtr_tx(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, peer_node, QRTR_PORT_CTRL,
	    tftp_srv, sizeof(tftp_srv));
	qrtr_servers_announced = true;
	adsp_log("qrtr: advertised pd-mapper(0x40) & read-only tftp(4096) to ADSP node %u", peer_node);
}

/* ---- QMI SERVREG_LOCATOR (pd-mapper) ---- */

struct pd_entry {
	char     service[64];
	char     domain[64];
	uint32_t instance;
	uint8_t  data_valid;
	uint32_t data;
};

#define MAX_PD_ENTRIES 160
static struct pd_entry pd_db[MAX_PD_ENTRIES];
static unsigned npd_db;
static bool pd_loaded;

static void
pd_add_entry(const char *svc, const char *dom, uint32_t inst, uint8_t valid, uint32_t data)
{
	if (svc[0] == '\0' || dom[0] == '\0' || npd_db >= MAX_PD_ENTRIES) {
		return;
	}
	for (unsigned i = 0; i < npd_db; i++) {
		if (strcmp(pd_db[i].service, svc) == 0 && strcmp(pd_db[i].domain, dom) == 0) {
			return;
		}
	}
	strncpy(pd_db[npd_db].service, svc, sizeof(pd_db[npd_db].service) - 1);
	strncpy(pd_db[npd_db].domain, dom, sizeof(pd_db[npd_db].domain) - 1);
	pd_db[npd_db].instance = inst;
	pd_db[npd_db].data_valid = valid;
	pd_db[npd_db].data = data;
	npd_db++;
}

static bool
json_extract_str(const char *start, const char *end, const char *key, char *out, size_t out_sz)
{
	char pat[48];
	const char *p, *q;
	size_t n;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(start, pat);
	if (p == NULL || p >= end) {
		return false;
	}
	p = strchr(p + strlen(pat), ':');
	if (p == NULL || p >= end) {
		return false;
	}
	p = strchr(p + 1, '"');
	if (p == NULL || ++p >= end) {
		return false;
	}
	q = strchr(p, '"');
	if (q == NULL || q > end) {
		return false;
	}
	n = (size_t)(q - p);
	if (n >= out_sz) {
		n = out_sz - 1;
	}
	memcpy(out, p, n);
	out[n] = '\0';
	return true;
}

static bool
json_extract_u32(const char *start, const char *end, const char *key, uint32_t *out)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(start, pat);
	if (p == NULL || p >= end) {
		return false;
	}
	p = strchr(p + strlen(pat), ':');
	if (p == NULL || ++p >= end) {
		return false;
	}
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}
	if (p >= end || !isdigit((unsigned char)*p)) {
		return false;
	}
	*out = (uint32_t)strtoul(p, NULL, 10);
	return true;
}

static void
pd_parse_jsn_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	long sz;
	char *buf, *dom_blk, *svc_arr;
	char soc[24] = "", domain[24] = "", subdomain[32] = "", dom[64] = "";
	uint32_t inst = 74;

	if (f == NULL) {
		return;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 || sz > 262144 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return;
	}
	buf = malloc((size_t)sz + 1);
	if (buf == NULL) {
		fclose(f);
		return;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return;
	}
	buf[sz] = '\0';
	fclose(f);

	dom_blk = strstr(buf, "\"sr_domain\"");
	svc_arr = strstr(buf, "\"sr_service\"");
	if (dom_blk != NULL) {
		const char *dend = svc_arr != NULL && svc_arr > dom_blk ? svc_arr : (buf + sz);
		(void)json_extract_str(dom_blk, dend, "soc", soc, sizeof(soc));
		(void)json_extract_str(dom_blk, dend, "domain", domain, sizeof(domain));
		(void)json_extract_str(dom_blk, dend, "subdomain", subdomain, sizeof(subdomain));
		(void)json_extract_u32(dom_blk, dend, "qmi_instance_id", &inst);
	}
	if (soc[0] && domain[0] && subdomain[0]) {
		snprintf(dom, sizeof(dom), "%s/%s/%s", soc, domain, subdomain);
	}
	if (dom[0] != '\0' && svc_arr != NULL) {
		const char *cur = strchr(svc_arr, '[');
		const char *aend = cur != NULL ? strchr(cur, ']') : NULL;
		while (cur != NULL && aend != NULL && cur < aend) {
			const char *obj = strchr(cur, '{');
			const char *oend = obj != NULL ? strchr(obj, '}') : NULL;
			char prov[32] = "", sname[32] = "", full_svc[64];
			uint32_t dvalid = 0, dval = 0;

			if (obj == NULL || oend == NULL || obj >= aend) {
				break;
			}
			if (json_extract_str(obj, oend, "provider", prov, sizeof(prov)) &&
			    json_extract_str(obj, oend, "service", sname, sizeof(sname))) {
				(void)json_extract_u32(obj, oend, "service_data_valid", &dvalid);
				(void)json_extract_u32(obj, oend, "service_data", &dval);
				snprintf(full_svc, sizeof(full_svc), "%s/%s", prov, sname);
				pd_add_entry(full_svc, dom, inst, (uint8_t)dvalid, dval);
			}
			cur = oend + 1;
		}
	}
	free(buf);
}

static void
pdmapper_load_jsn_db(void)
{
	DIR *dir;
	struct dirent *de;

	if (pd_loaded) {
		return;
	}
	pd_loaded = true;
	dir = opendir(ADSP_FW_DIR);
	if (dir != NULL) {
		while ((de = readdir(dir)) != NULL) {
			size_t nl = strlen(de->d_name);
			if (nl > 4 && strcmp(de->d_name + nl - 4, ".jsn") == 0) {
				char full[256];
				snprintf(full, sizeof(full), "%s/%s", ADSP_FW_DIR, de->d_name);
				pd_parse_jsn_file(full);
			}
		}
		closedir(dir);
	}

	/* Built-in SM8150 ADSP + MPSS fallback entries if .jsn files are absent */
	pd_add_entry("avs/audio",         "msm/adsp/audio_pd",  74, 0, 0);
	pd_add_entry("kernel/elf_loader", "msm/adsp/audio_pd",  74, 0, 0);
	pd_add_entry("tms/servreg",       "msm/adsp/audio_pd",  74, 0, 0);
	pd_add_entry("kernel/elf_loader", "msm/adsp/sensor_pd", 74, 0, 0);
	pd_add_entry("tms/servreg",       "msm/adsp/sensor_pd", 74, 0, 0);
	pd_add_entry("tms/servreg",       "msm/adsp/root_pd",   74, 0, 0);
	pd_add_entry("tms/pdr_enabled",   "msm/adsp/root_pd",   74, 0, 0);
	pd_add_entry("tms/servreg",       "msm/modem/root_pd",  180, 0, 0);
	pd_add_entry("tms/pdr_enabled",   "msm/modem/root_pd",  180, 0, 0);
	pd_add_entry("gps/gps_service",   "msm/modem/root_pd",  180, 0, 0);
	pd_add_entry("kernel/elf_loader", "msm/modem/wlan_pd",  180, 0, 0);
	pd_add_entry("tms/servreg",       "msm/modem/wlan_pd",  180, 0, 0);
	pd_add_entry("wlan/fw",           "msm/modem/wlan_pd",  180, 0, 0);
	adsp_log("pd-mapper: loaded %u domain service entries", npd_db);
}

static const uint8_t *
qmi_find_tlv(const uint8_t *tlv, uint16_t tlv_len, uint8_t type, uint16_t *out_len)
{
	for (uint32_t o = 0; o + 3u <= tlv_len;) {
		uint16_t l;
		memcpy(&l, tlv + o + 1, 2);
		if (o + 3u + l > tlv_len) {
			break;
		}
		if (tlv[o] == type) {
			*out_len = l;
			return tlv + o + 3;
		}
		o += 3u + l;
	}
	return NULL;
}

static size_t
qmi_put_tlv(uint8_t *dst, size_t off, size_t cap, uint8_t type, const void *val, uint16_t len)
{
	if (off + 3u + len > cap) {
		return off;
	}
	dst[off] = type;
	memcpy(dst + off + 1, &len, 2);
	memcpy(dst + off + 3, val, len);
	return off + 3u + len;
}

static void
pdmapper_handle_pkt(const struct qrtr_pkt_hdr *h, const uint8_t *d, uint32_t n)
{
	uint16_t txn, msg_id, msg_len;
	uint8_t rsp[1024];
	size_t roff = 7;

	if (n < 7 || d[0] != 0x00) { /* QMI_REQUEST == 0 */
		return;
	}
	memcpy(&txn, d + 1, 2);
	memcpy(&msg_id, d + 3, 2);
	memcpy(&msg_len, d + 5, 2);
	if (msg_len > n - 7) {
		msg_len = (uint16_t)(n - 7);
	}

	rsp[0] = 0x02; /* QMI_RESPONSE */
	memcpy(rsp + 1, &txn, 2);
	memcpy(rsp + 3, &msg_id, 2);

	if (msg_id == 0x0024) { /* PFR */
		uint16_t l1 = 0, l2 = 0;
		const uint8_t *s1 = qmi_find_tlv(d + 7, msg_len, 0x01, &l1);
		const uint8_t *s2 = qmi_find_tlv(d + 7, msg_len, 0x02, &l2);
		uint16_t ok[2] = { 0, 0 };

		adsp_log("pd-mapper: PFR '%.*s': '%.*s'",
		    s1 ? (int)l1 : 0, s1 ? (const char *)s1 : "",
		    s2 ? (int)l2 : 0, s2 ? (const char *)s2 : "");
		roff = qmi_put_tlv(rsp, roff, sizeof(rsp), 0x02, ok, 4);
	} else if (msg_id == 0x0021) { /* GET_DOMAIN_LIST */
		uint16_t slen = 0, olen = 0;
		const uint8_t *sp = qmi_find_tlv(d + 7, msg_len, 0x01, &slen);
		const uint8_t *op = qmi_find_tlv(d + 7, msg_len, 0x10, &olen);
		char svc[80] = "";
		uint32_t offset = 0;
		uint8_t dom_list[640];
		size_t doff = 1;
		uint16_t total = 0, rev = 1;
		uint16_t ok[2] = { 0, 0 };

		if (sp != NULL) {
			size_t cp = slen < sizeof(svc) - 1 ? slen : sizeof(svc) - 1;
			memcpy(svc, sp, cp);
			svc[cp] = '\0';
		}
		if (op != NULL && olen >= 4) {
			memcpy(&offset, op, 4);
		}
		dom_list[0] = 0;
		for (unsigned i = 0; i < npd_db; i++) {
			if (strcmp(pd_db[i].service, svc) != 0) {
				continue;
			}
			if (total++ < offset) {
				continue;
			}
			uint8_t nl = (uint8_t)strlen(pd_db[i].domain);
			if (doff + 1u + nl + 9u > sizeof(dom_list)) {
				break;
			}
			dom_list[doff++] = nl;
			memcpy(dom_list + doff, pd_db[i].domain, nl);
			doff += nl;
			memcpy(dom_list + doff, &pd_db[i].instance, 4);
			doff += 4;
			dom_list[doff++] = pd_db[i].data_valid;
			memcpy(dom_list + doff, &pd_db[i].data, 4);
			doff += 4;
			dom_list[0]++;
		}
		g_status.pd_queries++;
		adsp_log("pd-mapper: GET_DOMAIN_LIST '%s' -> %u match(es)", svc, total);
		roff = qmi_put_tlv(rsp, roff, sizeof(rsp), 0x02, ok, 4);
		roff = qmi_put_tlv(rsp, roff, sizeof(rsp), 0x10, &total, 2);
		roff = qmi_put_tlv(rsp, roff, sizeof(rsp), 0x11, &rev, 2);
		roff = qmi_put_tlv(rsp, roff, sizeof(rsp), 0x12, dom_list, (uint16_t)doff);
	} else {
		uint16_t unsup[2] = { 1, 0x0047 };
		roff = qmi_put_tlv(rsp, roff, sizeof(rsp), 0x02, unsup, 4);
	}

	uint16_t body_len = (uint16_t)(roff - 7u);
	memcpy(rsp + 5, &body_len, 2);
	(void)qrtr_tx(QRTR_TYPE_DATA, ADSP_PDMAPPER_PORT, h->src_node, h->src_port,
	    rsp, (uint32_t)roff);
}

/* ---- Read-only TFTP over QRTR (service 4096 -> /var/local/fw/modem) ---- */

enum { TFTP_RRQ = 1, TFTP_WRQ = 2, TFTP_DATA = 3, TFTP_ACK = 4, TFTP_ERROR = 5, TFTP_OACK = 6 };

struct tftp_xfer {
	bool     used;
	int      fd;
	uint32_t node;
	uint32_t port;
	uint32_t blksize;
	uint32_t wsize;
	uint64_t seek;
	uint64_t rsize;
	uint64_t size;
	uint32_t last_sent;
	bool     done;
	bool     pending;
	uint16_t pending_blk;
};

static struct tftp_xfer tftp_xfers[ADSP_TFTP_XFERS];

static void
put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xffu);
}

static uint16_t
get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void
tftp_send_err(uint32_t sport, uint32_t node, uint32_t port, uint16_t code, const char *msg)
{
	uint8_t b[128];
	size_t ml = strlen(msg) + 1;

	if (ml > sizeof(b) - 4) {
		ml = sizeof(b) - 4;
	}
	put_be16(b, TFTP_ERROR);
	put_be16(b + 2, code);
	memcpy(b + 4, msg, ml);
	(void)qrtr_tx(QRTR_TYPE_DATA, sport, node, port, b, (uint32_t)(4u + ml));
}

static void
tftp_close_xfer(int idx)
{
	if (tftp_xfers[idx].fd >= 0) {
		close(tftp_xfers[idx].fd);
		tftp_xfers[idx].fd = -1;
	}
	tftp_xfers[idx].used = false;
}

static void
tftp_send_block(int idx, uint32_t blk)
{
	uint8_t b[4 + ADSP_TFTP_MAX_BLKSIZE];
	struct tftp_xfer *x = &tftp_xfers[idx];
	uint64_t off = (uint64_t)(blk - 1u) * x->blksize;
	uint64_t end = x->rsize ? x->rsize : (x->size > x->seek ? x->size - x->seek : 0);
	uint32_t len = off >= end ? 0 :
	    (uint32_t)(end - off < x->blksize ? end - off : x->blksize);
	ssize_t r;

	put_be16(b, TFTP_DATA);
	put_be16(b + 2, (uint16_t)blk);
	r = len > 0 ? pread(x->fd, b + 4, len, (off_t)(x->seek + off)) : 0;
	if (r < 0) {
		r = 0;
	}
	(void)qrtr_tx(QRTR_TYPE_DATA, ADSP_TFTP_XFER_PORT0 + (uint32_t)idx,
	    x->node, x->port, b, (uint32_t)(4 + r));
	x->last_sent = blk;
	if ((uint32_t)r < x->blksize) {
		x->done = true;
	}
}

static void
tftp_send_window(int idx, uint32_t acked)
{
	struct tftp_xfer *x = &tftp_xfers[idx];

	for (uint32_t k = 0; k < x->wsize && !x->done; k++) {
		tftp_send_block(idx, acked + 1u + k);
	}
}

static int
tftp_open_ro(const char *req_path)
{
	const char *ro_prefix = "/readonly/firmware/image/";
	const char *base;
	char local[256];
	int fd;

	if (strncmp(req_path, ro_prefix, strlen(ro_prefix)) == 0) {
		const char *rest = req_path + strlen(ro_prefix);
		if (rest[0] != '\0' && strstr(rest, "..") == NULL) {
			snprintf(local, sizeof(local), "%s/%s", ADSP_FW_DIR, rest);
			fd = open(local, O_RDONLY | O_CLOEXEC);
			if (fd >= 0) {
				return fd;
			}
			for (char *c = local + strlen(ADSP_FW_DIR) + 1; *c != '\0'; c++) {
				*c = (char)tolower((unsigned char)*c);
			}
			fd = open(local, O_RDONLY | O_CLOEXEC);
			if (fd >= 0) {
				return fd;
			}
		}
		base = strrchr(rest, '/');
		base = base != NULL ? base + 1 : rest;
	} else {
		base = strrchr(req_path, '/');
		base = base != NULL ? base + 1 : req_path;
	}
	if (base[0] == '\0' || strchr(base, '.') == base) {
		return -1;
	}
	snprintf(local, sizeof(local), "%s/%s", ADSP_FW_DIR, base);
	fd = open(local, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		return fd;
	}
	for (char *c = local + strlen(ADSP_FW_DIR) + 1; *c != '\0'; c++) {
		*c = (char)tolower((unsigned char)*c);
	}
	return open(local, O_RDONLY | O_CLOEXEC);
}

static void
tftp_handle_rrq(const struct qrtr_pkt_hdr *h, const uint8_t *d, uint32_t n)
{
	char buf[512];
	const char *file, *end, *mode, *o;
	int slot = -1;
	struct tftp_xfer *x;
	struct stat st;
	uint8_t oack[512];
	size_t on = 2;
	int nopts = 0;

	if (n >= sizeof(buf)) {
		n = sizeof(buf) - 1;
	}
	memcpy(buf, d, n);
	buf[n] = '\0';
	file = buf + 2;
	end = buf + n;
	mode = file + strlen(file) + 1;

	for (int i = 0; i < ADSP_TFTP_XFERS; i++) {
		if (!tftp_xfers[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		tftp_send_err(ADSP_TFTP_PORT, h->src_node, h->src_port, 0, "busy");
		return;
	}

	x = &tftp_xfers[slot];
	memset(x, 0, sizeof(*x));
	x->fd = tftp_open_ro(file);
	if (x->fd < 0 || fstat(x->fd, &st) != 0) {
		if (x->fd >= 0) {
			close(x->fd);
		}
		adsp_log("tftp: RRQ '%s' -> not found", file);
		tftp_send_err(ADSP_TFTP_PORT, h->src_node, h->src_port, 1, "file not found");
		return;
	}
	x->node = h->src_node;
	x->port = h->src_port;
	x->blksize = 512;
	x->wsize = 1;
	x->size = (uint64_t)st.st_size;

	put_be16(oack, TFTP_OACK);
	o = mode < end ? (mode + strlen(mode) + 1) : end;
	while (o < end && *o != '\0') {
		const char *k = o;
		const char *v = o + strlen(o) + 1;
		uint64_t val;
		const char *echo = v;
		char num_s[24];

		if (v >= end) {
			break;
		}
		val = strtoull(v, NULL, 10);
		if (strcmp(k, "blksize") == 0) {
			x->blksize = val > ADSP_TFTP_MAX_BLKSIZE ? ADSP_TFTP_MAX_BLKSIZE :
			    (val > 0 ? (uint32_t)val : 512u);
			snprintf(num_s, sizeof(num_s), "%u", x->blksize);
			echo = num_s;
		} else if (strcmp(k, "wsize") == 0) {
			x->wsize = val > 0 ? (uint32_t)val : 1u;
		} else if (strcmp(k, "rsize") == 0) {
			x->rsize = val;
		} else if (strcmp(k, "seek") == 0) {
			x->seek = val;
		} else if (strcmp(k, "tsize") == 0) {
			snprintf(num_s, sizeof(num_s), "%llu", (unsigned long long)x->size);
			echo = num_s;
		}
		size_t kl = strlen(k) + 1, el = strlen(echo) + 1;
		if (on + kl + el < sizeof(oack)) {
			memcpy(oack + on, k, kl);
			memcpy(oack + on + kl, echo, el);
			on += kl + el;
			nopts++;
		}
		o = v + strlen(v) + 1;
	}

	if (x->rsize > 0 && x->seek + x->rsize > x->size) {
		x->rsize = x->size > x->seek ? (x->size - x->seek) : 0;
	}
	x->used = true;
	g_status.tftp_reads++;
	adsp_log("tftp: RRQ '%s' (size=%llu seek=%llu rsize=%llu blk=%u)",
	    file, (unsigned long long)x->size, (unsigned long long)x->seek,
	    (unsigned long long)x->rsize, x->blksize);

	if (nopts > 0) {
		(void)qrtr_tx(QRTR_TYPE_DATA, ADSP_TFTP_XFER_PORT0 + (uint32_t)slot,
		    x->node, x->port, oack, (uint32_t)on);
	} else {
		tftp_send_window(slot, 0);
	}
}

static void
tftp_handle_pkt(const struct qrtr_pkt_hdr *h, const uint8_t *d, uint32_t n)
{
	uint16_t op;

	if (n < 4) {
		return;
	}
	op = get_be16(d);
	if (h->dst_port == ADSP_TFTP_PORT) {
		if (op == TFTP_RRQ) {
			tftp_handle_rrq(h, d, n);
		} else if (op == TFTP_WRQ) {
			/* Strictly read-only: never touch /var/local/fw/rw */
			tftp_send_err(ADSP_TFTP_PORT, h->src_node, h->src_port, 2,
			    "access violation (read-only)");
		}
		return;
	}

	int idx = (int)(h->dst_port - ADSP_TFTP_XFER_PORT0);
	if (idx < 0 || idx >= ADSP_TFTP_XFERS || !tftp_xfers[idx].used) {
		return;
	}
	struct tftp_xfer *x = &tftp_xfers[idx];
	uint16_t blk = get_be16(d + 2);
	if (op == TFTP_ACK) {
		static bool in_win;
		if (in_win) {
			x->pending = true;
			x->pending_blk = blk;
			return;
		}
		in_win = true;
	again:
		if (x->done && blk == (uint16_t)x->last_sent) {
			tftp_close_xfer(idx);
		} else {
			uint32_t acked = x->last_sent - (uint16_t)(x->last_sent - blk);
			x->done = false;
			tftp_send_window(idx, acked);
		}
		if (x->used && x->pending) {
			x->pending = false;
			blk = x->pending_blk;
			goto again;
		}
		in_win = false;
	} else if (op == TFTP_ERROR) {
		tftp_close_xfer(idx);
	}
}

static void
qrtr_on_packet(const uint8_t *pkt, uint32_t len)
{
	struct qrtr_pkt_hdr h;
	const uint8_t *body;
	uint32_t blen;

	if (len < sizeof(h)) {
		return;
	}
	memcpy(&h, pkt, sizeof(h));
	if (h.version != 1) {
		return;
	}
	body = pkt + sizeof(h);
	blen = len - (uint32_t)sizeof(h);
	if (blen > h.size) {
		blen = h.size;
	}

	if (h.type == QRTR_TYPE_DATA) {
		if (h.confirm_rx) {
			uint32_t resume[5] = { QRTR_TYPE_RESUME_TX, h.dst_node, h.dst_port, 0, 0 };
			(void)qrtr_tx(QRTR_TYPE_RESUME_TX, h.dst_port, h.src_node, h.src_port,
			    resume, sizeof(resume));
		}
		if (h.dst_port == ADSP_PDMAPPER_PORT) {
			pdmapper_handle_pkt(&h, body, blen);
		} else if (h.dst_port == ADSP_TFTP_PORT ||
		    (h.dst_port >= ADSP_TFTP_XFER_PORT0 &&
		     h.dst_port < ADSP_TFTP_XFER_PORT0 + ADSP_TFTP_XFERS)) {
			tftp_handle_pkt(&h, body, blen);
		}
		return;
	}

	uint32_t w[5] = { 0 };
	memcpy(w, body, blen < sizeof(w) ? blen : sizeof(w));
	if (w[0] == QRTR_TYPE_HELLO) {
		uint32_t hello[5] = { QRTR_TYPE_HELLO, 0, 0, 0, 0 };
		adsp_peer_node = h.src_node;
		adsp_log("qrtr: < HELLO from ADSP node %u", adsp_peer_node);
		(void)qrtr_tx(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, adsp_peer_node, QRTR_PORT_CTRL,
		    hello, sizeof(hello));
		qrtr_announce_servers(adsp_peer_node);
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		adsp_log("qrtr: ADSP server svc=0x%x inst=0x%x at %u:0x%x",
		    w[1], w[2], w[3], w[4]);
		if (!qrtr_servers_announced) {
			qrtr_announce_servers(h.src_node);
		}
	}
}

/* ============================================================================
 * FastRPC Static PD Spawner ("audiopd") & Reverse-RPC (apps_remotectl/apps_std)
 * Verified against RMX1931 MSM-4.14 drivers/char/adsprpc.c & libadsprpc.so
 * ============================================================================ */

#define FASTRPC_PGID                    1000
#define FASTRPC_TID_MAIN                1000u
#define FASTRPC_TID_LISTENER            1001u

#define FASTRPC_CTX_CREATE_STATIC       0x11ULL /* (1 << 4) | pd(1) */
#define FASTRPC_CTX_LISTENER_INIT2      0x21ULL /* (2 << 4) | pd(1) */
#define FASTRPC_CTX_LISTENER_NEXT2      0x31ULL /* (3 << 4) | pd(1) */

#define FASTRPC_SC_CREATE_STATIC        0x08030000u /* method=8, in=3, out=0 */
#define FASTRPC_SC_LISTENER_INIT2       0x03000000u /* method=3, in=0, out=0 */
#define FASTRPC_SC_LISTENER_NEXT2       0x04020200u /* method=4, in=2, out=2 */

#define FASTRPC_HANDLE_REMOTECTL        0u
#define FASTRPC_HANDLE_STATIC_PD        1u
#define FASTRPC_HANDLE_LISTENER         3u
#define FASTRPC_HANDLE_APPS_STD         0x100u

#define AEE_ENOSUCH                     0x80000406u
#define APPS_STD_ENOENT                 0x00000201u
#define FASTRPC_LST_IN_CAP              16384u

struct fastrpc_smq_msg {
	uint32_t pid;
	uint32_t tid;
	uint64_t ctx;
	uint32_t handle;
	uint32_t sc;
	uint64_t page_addr;
	uint64_t page_size;
} __attribute__((packed));

struct fastrpc_smq_rsp {
	uint64_t ctx;
	int32_t  retval;
	int32_t  pad;
} __attribute__((packed));

static struct {
	bool     waiting;
	uint64_t want_ctx;
	bool     done;
	int32_t  retval;
} frpc_sync_wait;

static bool     frpc_lst_parked;
static uint64_t frpc_lst_out0_pa;
static uint64_t frpc_lst_out1_pa;

#define MAX_APPS_STD_FILES 16
static struct {
	bool used;
	int  fd;
} apps_std_files[MAX_APPS_STD_FILES];

static int
fastrpc_build_and_send(bool kernel, uint32_t tid, uint64_t ctx, uint32_t handle,
    uint32_t sc, uint64_t base_pa, uint64_t max_bytes,
    unsigned n_in, const void *const *in_ptrs, const uint32_t *in_lens,
    unsigned n_out, const uint32_t *out_lens, uint64_t *out_pa_addrs)
{
	static uint8_t page_img[ADSP_FASTRPC_LST_SIZE];
	unsigned n_total = n_in + n_out;
	uint32_t metalen = 40u * n_total + 384u; /* rpra + list + pages + fdlist[16] + crclist[64] */
	uint32_t cur_off = (metalen + 127u) & ~127u;
	uint32_t page_aligned_used;
	struct fastrpc_smq_msg msg;

	if (n_total > 8 || max_bytes > sizeof(page_img) || cur_off > max_bytes) {
		return -1;
	}
	memset(page_img, 0, metalen < sizeof(page_img) ?
	    ((metalen + 0xfffu) & ~0xfffu) : sizeof(page_img));

	for (unsigned i = 0; i < n_total; i++) {
		uint32_t blen = (i < n_in) ? in_lens[i] : out_lens[i - n_in];
		struct {
			int32_t num;
			int32_t pgidx;
		} lb = { .num = blen ? 1 : 0, .pgidx = (int32_t)i };

		memcpy(page_img + 16u * n_total + 8u * i, &lb, sizeof(lb));
		if (blen == 0) {
			if (i >= n_in && out_pa_addrs != NULL) {
				out_pa_addrs[i - n_in] = 0;
			}
			continue;
		}
		cur_off = (cur_off + 127u) & ~127u;
		if ((uint64_t)cur_off + blen > max_bytes) {
			return -1;
		}

		uint64_t buf_pa = base_pa + cur_off;
		uint64_t pg_start = buf_pa & ~0xfffULL;
		uint64_t pg_end = (buf_pa + blen + 0xfffULL) & ~0xfffULL;
		struct {
			uint64_t pv;
			uint64_t len;
		} rp = { .pv = buf_pa, .len = blen };
		struct {
			uint64_t addr;
			uint64_t size;
		} pg = {
			.addr = ((uint64_t)ADSP_FASTRPC_CB_HIGH << 32) | pg_start,
			.size = pg_end - pg_start,
		};

		memcpy(page_img + 16u * i, &rp, sizeof(rp));
		memcpy(page_img + 24u * n_total + 16u * i, &pg, sizeof(pg));

		memset(page_img + cur_off, 0, (blen + 127u) & ~127u);
		if (i < n_in && in_ptrs[i] != NULL) {
			memcpy(page_img + cur_off, in_ptrs[i], blen);
		} else if (i >= n_in && out_pa_addrs != NULL) {
			out_pa_addrs[i - n_in] = buf_pa;
		}
		cur_off += blen;
	}

	page_aligned_used = (cur_off + 0xfffu) & ~0xfffu;
	if (page_aligned_used == 0) {
		page_aligned_used = 0x1000u;
	}
	if (page_aligned_used > max_bytes) {
		return -1;
	}
	(void)pm_wr(base_pa, page_img, page_aligned_used);
	__asm__ volatile("dsb sy" ::: "memory");

	msg.pid = kernel ? 0u : (uint32_t)FASTRPC_PGID;
	msg.tid = tid;
	msg.ctx = ctx;
	msg.handle = handle;
	msg.sc = sc;
	msg.page_addr = ((uint64_t)ADSP_FASTRPC_CB_HIGH << 32) | base_pa;
	msg.page_size = page_aligned_used;

	return chan_send_pkt(&chans[CH_IDX_FASTRPC], &msg, sizeof(msg));
}

static int
fastrpc_post_listener_next2(uint32_t prev_ctx, uint32_t prev_result,
    const uint8_t *out_stream, uint32_t out_stream_len)
{
	struct {
		uint32_t prev_ctx;
		uint32_t prev_result;
		uint32_t prev_out_len;
		uint32_t next_in_cap;
	} in0 = {
		.prev_ctx     = prev_ctx,
		.prev_result  = prev_result,
		.prev_out_len = out_stream_len,
		.next_in_cap  = FASTRPC_LST_IN_CAP,
	};
	const void *in_ptrs[2] = { &in0, out_stream_len ? out_stream : NULL };
	uint32_t in_lens[2] = { sizeof(in0), out_stream_len };
	uint32_t out_lens[2] = { 16u, FASTRPC_LST_IN_CAP };
	uint64_t out_pas[2] = { 0, 0 };

	if (fastrpc_build_and_send(false, FASTRPC_TID_LISTENER, FASTRPC_CTX_LISTENER_NEXT2,
	    FASTRPC_HANDLE_LISTENER, FASTRPC_SC_LISTENER_NEXT2,
	    ADSP_FASTRPC_LST_PA, ADSP_FASTRPC_LST_SIZE,
	    2, in_ptrs, in_lens, 2, out_lens, out_pas) != 0) {
		frpc_lst_parked = false;
		return -1;
	}
	frpc_lst_out0_pa = out_pas[0];
	frpc_lst_out1_pa = out_pas[1];
	frpc_lst_parked = true;
	return 0;
}

static int
apps_std_open_modem_ro(const char *raw_name)
{
	const char *base;
	char local[256];
	int fd;

	if (raw_name == NULL || raw_name[0] == '\0') {
		return -1;
	}
	base = strrchr(raw_name, '/');
	base = base != NULL ? base + 1 : raw_name;
	if (base[0] == '\0' || base[0] == '.' || strstr(base, "..") != NULL) {
		return -1;
	}
	snprintf(local, sizeof(local), "%s/%s", ADSP_FW_DIR, base);
	fd = open(local, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		return fd;
	}
	for (char *c = local + strlen(ADSP_FW_DIR) + 1; *c != '\0'; c++) {
		*c = (char)tolower((unsigned char)*c);
	}
	return open(local, O_RDONLY | O_CLOEXEC);
}

static int
apps_std_alloc_sin(int fd)
{
	for (int i = 0; i < MAX_APPS_STD_FILES; i++) {
		if (!apps_std_files[i].used) {
			apps_std_files[i].used = true;
			apps_std_files[i].fd = fd;
			return 100 + i;
		}
	}
	close(fd);
	return -1;
}

static int
apps_std_lookup_fd(int32_t sin, int *out_slot)
{
	int slot = (int)sin - 100;

	if (slot < 0 || slot >= MAX_APPS_STD_FILES || !apps_std_files[slot].used) {
		return -1;
	}
	if (out_slot != NULL) {
		*out_slot = slot;
	}
	return apps_std_files[slot].fd;
}

struct rev_buf_desc {
	const uint8_t *pv;
	uint32_t       len;
};

struct rev_out_desc {
	uint8_t  *pv;
	uint32_t  len;
};

static uint32_t
fastrpc_dispatch_apps_std(uint32_t method, unsigned n_in, const struct rev_buf_desc *inb,
    unsigned n_out, struct rev_out_desc *outb)
{
	switch (method) {
	case 0: { /* fopen(name, mode) -> sin */
		char name[256] = "";
		int fd, sin;

		if (n_in < 3 || n_out < 1 || outb[0].len < 4) {
			return 14u;
		}
		if (inb[1].pv != NULL && inb[1].len > 0) {
			size_t cp = inb[1].len < sizeof(name) - 1 ? inb[1].len : sizeof(name) - 1;
			memcpy(name, inb[1].pv, cp);
			name[cp] = '\0';
		}
		fd = apps_std_open_modem_ro(name);
		if (fd < 0) {
			adsp_log("fastrpc: apps_std fopen('%s') -> ENOENT", name);
			return 2u; /* ENOENT */
		}
		sin = apps_std_alloc_sin(fd);
		if (sin < 0) {
			return 24u; /* EMFILE */
		}
		int32_t s32 = (int32_t)sin;
		memcpy(outb[0].pv, &s32, 4);
		adsp_log("fastrpc: apps_std fopen('%s') -> sin=%d", name, sin);
		return 0;
	}

	case 19: { /* fopen_with_env(envvarname, delim, name, mode) -> sin */
		char envn[64] = "", name[256] = "";
		int fd, sin;

		if (n_in < 5 || n_out < 1 || outb[0].len < 4) {
			return 14u;
		}
		if (inb[1].pv != NULL && inb[1].len > 0) {
			size_t cp = inb[1].len < sizeof(envn) - 1 ? inb[1].len : sizeof(envn) - 1;
			memcpy(envn, inb[1].pv, cp);
			envn[cp] = '\0';
		}
		if (inb[3].pv != NULL && inb[3].len > 0) {
			size_t cp = inb[3].len < sizeof(name) - 1 ? inb[3].len : sizeof(name) - 1;
			memcpy(name, inb[3].pv, cp);
			name[cp] = '\0';
		}
		fd = apps_std_open_modem_ro(name);
		if (fd < 0) {
			adsp_log("fastrpc: apps_std fopen_with_env('%s', '%s') -> ENOENT", envn, name);
			return APPS_STD_ENOENT;
		}
		sin = apps_std_alloc_sin(fd);
		if (sin < 0) {
			return 24u;
		}
		int32_t s32 = (int32_t)sin;
		memcpy(outb[0].pv, &s32, 4);
		adsp_log("fastrpc: apps_std fopen_with_env('%s', '%s') -> sin=%d", envn, name, sin);
		return 0;
	}

	case 2: /* fflush(sin) */
	case 14: /* clearerr(sin) */
		return 0;

	case 3: { /* fclose(sin) */
		int32_t sin = 0;
		int slot = -1, fd;

		if (n_in < 1 || inb[0].len < 4) {
			return 14u;
		}
		memcpy(&sin, inb[0].pv, 4);
		fd = apps_std_lookup_fd(sin, &slot);
		if (fd < 0) {
			return 9u; /* EBADF */
		}
		close(fd);
		apps_std_files[slot].fd = -1;
		apps_std_files[slot].used = false;
		return 0;
	}

	case 4: { /* fread(sin, buf_len) -> (bytesRead, bEOF), buf */
		int32_t hdr[2] = { 0, 0 };
		int32_t out_hdr[2] = { 0, 0 };
		int fd;
		ssize_t got;
		uint32_t want;

		if (n_in < 1 || inb[0].len < 8 || n_out < 2 || outb[0].len < 8) {
			return 14u;
		}
		memcpy(hdr, inb[0].pv, 8);
		fd = apps_std_lookup_fd(hdr[0], NULL);
		if (fd < 0) {
			return 9u;
		}
		want = hdr[1] > 0 ? (uint32_t)hdr[1] : 0u;
		if (want > outb[1].len) {
			want = outb[1].len;
		}
		got = want > 0 ? read(fd, outb[1].pv, want) : 0;
		if (got < 0) {
			return 5u; /* EIO */
		}
		out_hdr[0] = (int32_t)got;
		if ((uint32_t)got < want) {
			out_hdr[1] = 1; /* bEOF */
		} else {
			struct stat st;
			off_t cur = lseek(fd, 0, SEEK_CUR);
			if (fstat(fd, &st) == 0 && cur >= st.st_size) {
				out_hdr[1] = 1;
			}
		}
		memcpy(outb[0].pv, out_hdr, 8);
		return 0;
	}

	case 8: { /* ftell(sin) -> pos */
		int32_t sin = 0, pos32 = 0;
		int fd;
		off_t pos;

		if (n_in < 1 || inb[0].len < 4 || n_out < 1 || outb[0].len < 4) {
			return 14u;
		}
		memcpy(&sin, inb[0].pv, 4);
		fd = apps_std_lookup_fd(sin, NULL);
		if (fd < 0) {
			return 9u;
		}
		pos = lseek(fd, 0, SEEK_CUR);
		if (pos < 0) {
			return 29u;
		}
		pos32 = (int32_t)pos;
		memcpy(outb[0].pv, &pos32, 4);
		return 0;
	}

	case 9: { /* fseek(sin, offset, whence) */
		int32_t args[3] = { 0, 0, 0 };
		int fd, whence;

		if (n_in < 1 || inb[0].len < 12) {
			return 14u;
		}
		memcpy(args, inb[0].pv, 12);
		fd = apps_std_lookup_fd(args[0], NULL);
		if (fd < 0) {
			return 9u;
		}
		whence = (args[2] == 1) ? SEEK_CUR : (args[2] == 2) ? SEEK_END : SEEK_SET;
		return lseek(fd, (off_t)args[1], whence) < 0 ? 22u : 0u;
	}

	case 10: { /* flen(sin) -> uint64_t size */
		int32_t sin = 0;
		int fd;
		struct stat st;
		uint64_t sz64;

		if (n_in < 1 || inb[0].len < 4 || n_out < 1 || outb[0].len < 8) {
			return 14u;
		}
		memcpy(&sin, inb[0].pv, 4);
		fd = apps_std_lookup_fd(sin, NULL);
		if (fd < 0 || fstat(fd, &st) != 0) {
			return 9u;
		}
		sz64 = (uint64_t)st.st_size;
		memcpy(outb[0].pv, &sz64, 8);
		return 0;
	}

	case 11: { /* rewind(sin) */
		int32_t sin = 0;
		int fd;

		if (n_in < 1 || inb[0].len < 4) {
			return 14u;
		}
		memcpy(&sin, inb[0].pv, 4);
		fd = apps_std_lookup_fd(sin, NULL);
		if (fd < 0) {
			return 9u;
		}
		(void)lseek(fd, 0, SEEK_SET);
		return 0;
	}

	case 12: /* feof(sin) */
	case 13: { /* ferror(sin) */
		int32_t sin = 0, flag = 0;
		int fd;

		if (n_in < 1 || inb[0].len < 4 || n_out < 1 || outb[0].len < 4) {
			return 14u;
		}
		memcpy(&sin, inb[0].pv, 4);
		fd = apps_std_lookup_fd(sin, NULL);
		if (fd < 0) {
			return 9u;
		}
		if (method == 12) {
			struct stat st;
			off_t cur = lseek(fd, 0, SEEK_CUR);
			if (fstat(fd, &st) == 0 && cur >= st.st_size) {
				flag = 1;
			}
		}
		memcpy(outb[0].pv, &flag, 4);
		return 0;
	}

	case 15: { /* print_string(str) */
		if (n_in >= 2 && inb[1].pv != NULL && inb[1].len > 0) {
			adsp_log("fastrpc: dsp_print: %.*s", (int)inb[1].len, (const char *)inb[1].pv);
		}
		return 0;
	}

	case 16: { /* getenv(name) -> (valLenReq, val) */
		char envn[64] = "";
		const char *val = ADSP_FW_DIR;
		int32_t vlen = (int32_t)strlen(val) + 1;

		if (n_in < 2 || inb[0].len < 8 || n_out < 2 || outb[0].len < 4) {
			return 14u;
		}
		if (inb[1].pv != NULL && inb[1].len > 0) {
			size_t cp = inb[1].len < sizeof(envn) - 1 ? inb[1].len : sizeof(envn) - 1;
			memcpy(envn, inb[1].pv, cp);
			envn[cp] = '\0';
		}
		if (strstr(envn, "ADSP") == NULL && strstr(envn, "DSP") == NULL) {
			return 0xffffffffu;
		}
		memcpy(outb[0].pv, &vlen, 4);
		if (outb[1].len > 0) {
			size_t cp = (uint32_t)vlen < outb[1].len ? (uint32_t)vlen : outb[1].len;
			memcpy(outb[1].pv, val, cp);
			outb[1].pv[cp - 1] = '\0';
		}
		return 0;
	}

	case 21: { /* get_search_paths_with_env -> 1 path: /var/local/fw/modem */
		const char *val = ADSP_FW_DIR;
		uint32_t npaths = 1u;
		uint16_t max_plen = (uint16_t)(strlen(val) + 1u);

		if (n_out < 1 || outb[0].len < 6) {
			return 14u;
		}
		memcpy(outb[0].pv + 0, &npaths, 4);
		memcpy(outb[0].pv + 4, &max_plen, 2);
		if (n_out >= 2 && outb[1].len > 0) {
			size_t cp = max_plen < outb[1].len ? max_plen : outb[1].len;
			memcpy(outb[1].pv, val, cp);
			outb[1].pv[cp - 1] = '\0';
		}
		return 0;
	}

	case 22: { /* fileExists(path) -> bool */
		char name[256] = "";
		int fd;

		if (n_in < 2 || n_out < 1 || outb[0].len < 1) {
			return 14u;
		}
		if (inb[1].pv != NULL && inb[1].len > 0) {
			size_t cp = inb[1].len < sizeof(name) - 1 ? inb[1].len : sizeof(name) - 1;
			memcpy(name, inb[1].pv, cp);
			name[cp] = '\0';
		}
		fd = apps_std_open_modem_ro(name);
		if (fd >= 0) {
			close(fd);
			outb[0].pv[0] = 1u;
		} else {
			outb[0].pv[0] = 0u;
		}
		return 0;
	}

	default:
		adsp_log("fastrpc: apps_std unsupported method=%u", method);
		return 20u; /* AEE_EUNSUPPORTED */
	}
}

static void
fastrpc_handle_reverse_rpc(void)
{
	static uint8_t in_stream[FASTRPC_LST_IN_CAP];
	static uint8_t out_stream[FASTRPC_LST_IN_CAP];
	struct {
		uint32_t rev_ctx;
		uint32_t rev_handle;
		uint32_t rev_sc;
		int32_t  in_sz;
	} hdr = { 0 };
	struct rev_buf_desc inb[8];
	struct rev_out_desc outb[8];
	uint32_t method, n_in, n_out, n_inh, n_outh;
	uint32_t pos = 0, opos = 0, result = AEE_ENOSUCH;

	if (frpc_lst_out0_pa == 0 || pm_rd(frpc_lst_out0_pa, &hdr, sizeof(hdr)) != 0) {
		(void)fastrpc_post_listener_next2(0, AEE_ENOSUCH, NULL, 0);
		return;
	}
	if (hdr.in_sz < 0 || (uint32_t)hdr.in_sz > sizeof(in_stream)) {
		adsp_log("fastrpc: rev_rpc ctx=0x%x oversized in_sz=%d", hdr.rev_ctx, hdr.in_sz);
		(void)fastrpc_post_listener_next2(hdr.rev_ctx, 14u, NULL, 0);
		return;
	}
	if (hdr.in_sz > 0 && frpc_lst_out1_pa != 0) {
		uint32_t rd_aligned = ((uint32_t)hdr.in_sz + 3u) & ~3u;
		(void)pm_rd(frpc_lst_out1_pa, in_stream, rd_aligned);
	}

	method = (hdr.rev_sc >> 24) & 0x1fu;
	n_in   = (hdr.rev_sc >> 16) & 0xffu;
	n_out  = (hdr.rev_sc >> 8) & 0xffu;
	n_inh  = (hdr.rev_sc >> 4) & 0x0fu;
	n_outh = hdr.rev_sc & 0x0fu;
	g_status.fastrpc_rev_calls++;

	if (n_in > 8 || n_out > 8 || n_inh != 0 || n_outh != 0) {
		adsp_log("fastrpc: rev_rpc unsupported sc=0x%x (h=0x%x)", hdr.rev_sc, hdr.rev_handle);
		(void)fastrpc_post_listener_next2(hdr.rev_ctx, 20u, NULL, 0);
		return;
	}

	memset(inb, 0, sizeof(inb));
	memset(outb, 0, sizeof(outb));
	for (unsigned i = 0; i < n_in; i++) {
		uint32_t l = 0;
		if (pos + 4u > (uint32_t)hdr.in_sz) {
			(void)fastrpc_post_listener_next2(hdr.rev_ctx, 14u, NULL, 0);
			return;
		}
		memcpy(&l, in_stream + pos, 4);
		pos += 4u;
		if (l > 0) {
			pos = (pos + 7u) & ~7u;
			if (pos + l > (uint32_t)hdr.in_sz) {
				(void)fastrpc_post_listener_next2(hdr.rev_ctx, 14u, NULL, 0);
				return;
			}
			inb[i].pv = in_stream + pos;
			inb[i].len = l;
			pos += l;
		}
	}

	memset(out_stream, 0, sizeof(out_stream));
	for (unsigned j = 0; j < n_out; j++) {
		uint32_t l = 0;
		if (pos + 4u <= (uint32_t)hdr.in_sz) {
			memcpy(&l, in_stream + pos, 4);
			pos += 4u;
		}
		if (opos + 4u > sizeof(out_stream)) {
			(void)fastrpc_post_listener_next2(hdr.rev_ctx, 14u, NULL, 0);
			return;
		}
		memcpy(out_stream + opos, &l, 4);
		opos += 4u;
		if (l > 0) {
			opos = (opos + 7u) & ~7u;
			if (opos + l > sizeof(out_stream)) {
				(void)fastrpc_post_listener_next2(hdr.rev_ctx, 14u, NULL, 0);
				return;
			}
			outb[j].pv = out_stream + opos;
			outb[j].len = l;
			opos += l;
		}
	}

	if (hdr.rev_handle == FASTRPC_HANDLE_REMOTECTL) {
		if (method == 0 && n_in >= 2 && n_out >= 1 && outb[0].len >= 8) {
			char mod[128] = "";
			if (inb[1].pv != NULL && inb[1].len > 0) {
				size_t cp = inb[1].len < sizeof(mod) - 1 ? inb[1].len : sizeof(mod) - 1;
				memcpy(mod, inb[1].pv, cp);
				mod[cp] = '\0';
			}
			if (strstr(mod, "apps_std") != NULL) {
				uint32_t h_out[2] = { FASTRPC_HANDLE_APPS_STD, 0u };
				memcpy(outb[0].pv, h_out, 8);
				result = 0;
				adsp_log("fastrpc: remotectl open('%s') -> handle=0x%x", mod, FASTRPC_HANDLE_APPS_STD);
			} else {
				result = AEE_ENOSUCH;
				adsp_log("fastrpc: remotectl open('%s') -> ENOSUCH", mod);
			}
		} else if (method == 1) {
			result = 0;
		} else {
			result = 20u;
		}
	} else if (hdr.rev_handle == FASTRPC_HANDLE_APPS_STD) {
		result = fastrpc_dispatch_apps_std(method, n_in, inb, n_out, outb);
	} else {
		adsp_log("fastrpc: rev_rpc unknown handle=0x%x sc=0x%x", hdr.rev_handle, hdr.rev_sc);
		result = AEE_ENOSUCH;
	}

	if (result != 0) {
		opos = 0;
	}
	(void)fastrpc_post_listener_next2(hdr.rev_ctx, result, opos ? out_stream : NULL, opos);
}

static void
fastrpc_on_packet(const uint8_t *pkt, uint32_t len)
{
	struct fastrpc_smq_rsp rsp;

	if (len < sizeof(rsp)) {
		return;
	}
	memcpy(&rsp, pkt, sizeof(rsp));

	if (frpc_sync_wait.waiting && rsp.ctx == frpc_sync_wait.want_ctx) {
		frpc_sync_wait.retval = rsp.retval;
		frpc_sync_wait.done = true;
		return;
	}
	if (rsp.ctx == FASTRPC_CTX_LISTENER_NEXT2) {
		frpc_lst_parked = false;
		if (rsp.retval == 0) {
			fastrpc_handle_reverse_rpc();
		} else {
			adsp_log("fastrpc: listener_next2 returned %d", rsp.retval);
		}
		return;
	}
	adsp_log("fastrpc: rx unhandled rsp ctx=0x%llx retval=%d",
	    (unsigned long long)rsp.ctx, rsp.retval);
}

static int
fastrpc_call_sync(bool kernel, uint32_t tid, uint64_t ctx, uint32_t handle,
    uint32_t sc, unsigned n_in, const void *const *in_ptrs, const uint32_t *in_lens,
    unsigned timeout_ms)
{
	uint64_t deadline;

	memset(&frpc_sync_wait, 0, sizeof(frpc_sync_wait));
	frpc_sync_wait.waiting = true;
	frpc_sync_wait.want_ctx = ctx;

	if (fastrpc_build_and_send(kernel, tid, ctx, handle, sc,
	    ADSP_FASTRPC_MSG_PA, ADSP_FASTRPC_MSG_SIZE,
	    n_in, in_ptrs, in_lens, 0, NULL, NULL) != 0) {
		frpc_sync_wait.waiting = false;
		return -1;
	}

	deadline = now_ms() + timeout_ms;
	while (!frpc_sync_wait.done && now_ms() < deadline && !g_stop_requested) {
		(void)adsp_glink_poll(5);
	}
	frpc_sync_wait.waiting = false;
	adsp_smmu_check_fault("fastrpc");
	if (!frpc_sync_wait.done) {
		adsp_log("fastrpc: timeout waiting for ctx=0x%llx (sc=0x%x)",
		    (unsigned long long)ctx, sc);
		return -1;
	}
	if (frpc_sync_wait.retval != 0) {
		adsp_log("fastrpc: ctx=0x%llx (sc=0x%x) failed retval=%d (0x%x)",
		    (unsigned long long)ctx, sc, frpc_sync_wait.retval,
		    (uint32_t)frpc_sync_wait.retval);
		return -1;
	}
	return 0;
}

int
adsp_fastrpc_spawn_audiopd(void)
{
	struct {
		int32_t  pgid;
		uint32_t namelen;
		uint32_t pageslen;
	} __attribute__((packed)) create_hdr = {
		.pgid     = FASTRPC_PGID,
		.namelen  = 8u,
		.pageslen = 1u,
	};
	static const char pd_name[8] = "audiopd";
	struct {
		uint64_t addr;
		uint64_t size;
	} __attribute__((packed)) rh_page = {
		.addr = ADSP_FASTRPC_HEAP_PA,   /* pure 32-bit PA per adsprpc.c:838 */
		.size = ADSP_FASTRPC_HEAP_SIZE, /* 2 MiB */
	};
	const void *in_ptrs[3] = { &create_hdr, pd_name, &rh_page };
	uint32_t in_lens[3] = { sizeof(create_hdr), sizeof(pd_name), sizeof(rh_page) };

	if (g_status.audiopd_spawned) {
		return 0;
	}
	if (!g_status.glink_fastrpc_up) {
		uint64_t end = now_ms() + 3000ULL;
		chan_send_open(&chans[CH_IDX_FASTRPC]);
		while (!g_status.glink_fastrpc_up && now_ms() < end && !g_stop_requested) {
			(void)adsp_glink_poll(10);
		}
		if (!g_status.glink_fastrpc_up) {
			adsp_log("fastrpc: 'fastrpcglink-apps-dsp' channel did not come up");
			return -1;
		}
	}

	adsp_log("fastrpc: spawning static PD 'audiopd' (heap=0x%llx size=0x%llx)...",
	    (unsigned long long)rh_page.addr, (unsigned long long)rh_page.size);
	if (fastrpc_call_sync(true, FASTRPC_TID_MAIN, FASTRPC_CTX_CREATE_STATIC,
	    FASTRPC_HANDLE_STATIC_PD, FASTRPC_SC_CREATE_STATIC,
	    3, in_ptrs, in_lens, 6000) != 0) {
		return -1;
	}
	g_status.audiopd_spawned = true;
	adsp_log("fastrpc: static PD 'audiopd' spawned (retval=0)");

	if (fastrpc_call_sync(false, FASTRPC_TID_LISTENER, FASTRPC_CTX_LISTENER_INIT2,
	    FASTRPC_HANDLE_LISTENER, FASTRPC_SC_LISTENER_INIT2,
	    0, NULL, NULL, 2500) == 0) {
		adsp_log("fastrpc: adsp_listener_init2 -> ok, parking adsp_listener_next2");
		(void)fastrpc_post_listener_next2(0, 0xffffffffu, NULL, 0);
	} else {
		adsp_log("fastrpc: warning: adsp_listener_init2 did not ACK");
	}
	return 0;
}
