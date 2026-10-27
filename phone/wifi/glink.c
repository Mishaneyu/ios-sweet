#include <stdio.h>
#include <string.h>
#include "glink.h"
#include "pmem.h"
#include "smem.h"

#define DESC_ITEM	478
#define FIFO_TX_ITEM	479	/* ours: apps -> modem */
#define FIFO_RX_ITEM	480	/* the modem's */
#define APCS_IPC	0x17c0000cull
#define IPC_GLINK	12

static uint64_t desc, tx_fifo, rx_fifo;
static uint32_t tx_len, rx_len;

/* descs: [0] tx tail, [1] tx head, [2] rx tail, [3] rx head (ours = tx) */
#define TX_TAIL	(desc + 0)
#define TX_HEAD	(desc + 4)
#define RX_TAIL	(desc + 8)
#define RX_HEAD	(desc + 12)

int glink_prepare(void)
{
	if (smem_alloc(SMEM_HOST_MODEM, DESC_ITEM, 32, &desc) ||
	    smem_alloc(SMEM_HOST_MODEM, FIFO_TX_ITEM, 16384, &tx_fifo) ||
	    smem_get(SMEM_HOST_MODEM, FIFO_TX_ITEM, &tx_fifo, &tx_len)) {
		fprintf(stderr, "glink: smem items\n");
		return -1;
	}
	return 0;
}

int glink_init(void)
{
	if ((!desc || !tx_len) && glink_prepare())
		return -1;
	if (smem_get(SMEM_HOST_MODEM, FIFO_RX_ITEM, &rx_fifo, &rx_len)) {
		fprintf(stderr, "glink: the modem has no rx fifo (item 480)\n");
		return -1;
	}
	/* as Linux qcom_glink_smem_register: our ends of both pipes start over */
	wr32(RX_TAIL, 0);
	wr32(TX_HEAD, 0);
	printf("glink: desc %llx tx fifo %llx (%u) rx fifo %llx (%u); tx %u/%u rx %u/%u\n",
	       (unsigned long long)desc, (unsigned long long)tx_fifo, tx_len, (unsigned long long)rx_fifo, rx_len,
	       rd32(TX_TAIL), rd32(TX_HEAD), rd32(RX_TAIL), rd32(RX_HEAD));
	return 0;
}

static uint32_t tx_write(uint32_t head, const uint8_t *data, size_t n)
{
	/* the FIFO is device memory here: whole words only (n and head are multiples of 8) */
	size_t first = n <= tx_len - head ? n : tx_len - head;
	if (first)
		pm_wr(tx_fifo + head, data, first);
	if (n > first)
		pm_wr(tx_fifo, data + first, n - first);
	head += (uint32_t)n;
	if (head >= tx_len)
		head -= tx_len;
	return head;
}

int glink_send(uint16_t cmd, uint16_t p1, uint32_t p2, const void *data, size_t len)
{
	uint8_t buf[8 + 8 + 8192 + 8] = { 0 };
	size_t n = 8 + len, padded = (n + 7) & ~(size_t)7;
	if (padded > sizeof(buf))
		return -1;
	memcpy(buf, &cmd, 2);
	memcpy(buf + 2, &p1, 2);
	memcpy(buf + 4, &p2, 4);
	if (len)
		memcpy(buf + 8, data, len);
	uint32_t head = rd32(TX_HEAD), tail = rd32(TX_TAIL);
	uint32_t avail = tail <= head ? tx_len - head + tail : tail - head;
	if (avail < padded + 16)
		return -1;	/* full: the caller polls and retries */
	head = tx_write(head, buf, padded);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(TX_HEAD, head);
	wr32(APCS_IPC, 1u << IPC_GLINK);
	return 0;
}

size_t glink_rx_avail(void)
{
	uint32_t head = rd32(RX_HEAD), tail = rd32(RX_TAIL);
	return head < tail ? rx_len - tail + head : head - tail;
}

void glink_rx_peek(void *buf, size_t off, size_t len)
{
	if (!len)
		return;
	uint32_t t = rd32(RX_TAIL) + (uint32_t)off;
	if (t >= rx_len)
		t -= rx_len;
	uint8_t *b = buf;
	size_t done = 0;
	while (done < len) {
		if ((t & 3u) || (len - done) < 4) {
			uint32_t w = rd32(rx_fifo + (t & ~3u));
			b[done++] = (uint8_t)(w >> (8 * (t & 3u)));
			if (++t >= rx_len)
				t = 0;
		} else {
			size_t rem = len - done;
			size_t contig = rx_len - t;
			size_t chunk = (rem < contig ? rem : contig) & ~(size_t)3;
			pm_rd(rx_fifo + t, b + done, chunk);
			done += chunk;
			t += (uint32_t)chunk;
			if (t >= rx_len)
				t -= rx_len;
		}
	}
}

void glink_rx_advance(size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)len;
	if (t >= rx_len)
		t -= rx_len;
	wr32(RX_TAIL, t);
}
