/*
 * Local AF_UNIX QRTR multiplexer (/var/run/hot-qrtr.sock) inside wland.
 * Exposes MPSS (node 0) QMI services (DMS, UIM, NAS, WDS, WDA, DPM, IPA, PDC)
 * to local daemons/tools (cellulard, qmicli) on dedicated QRTR ports 0x40..0x47
 * without disturbing wland's own RMTFS/PDMAPPER/TFTP/WLFW ports or GLINK rings.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "link.h"
#include "pmem.h"
#include "qrtr_mux.h"
#include "hot_qrtr.h"

#define MUX_RX_CAP	(sizeof(struct hot_qrtr_msg) + HOT_QRTR_MAX_DATA)
#define MUX_TX_CAP	65536u

struct qrtr_srv_entry {
	int      used;
	uint32_t service;
	uint32_t instance;
	uint32_t node;
	uint32_t port;
};

struct qrtr_mux_client {
	int      fd;
	uint32_t port;
	size_t   rx_len;
	size_t   tx_head;
	size_t   tx_len;
	uint8_t  rx_buf[MUX_RX_CAP];
	uint8_t  tx_buf[MUX_TX_CAP];
};

static int listen_fd = -1;
static struct qrtr_srv_entry srv_cache[QRTR_MUX_MAX_SERVERS];
static struct qrtr_mux_client clients[QRTR_MUX_MAX_CLIENTS];
static int clients_inited;

static void init_slots(void)
{
	if (clients_inited)
		return;
	for (unsigned i = 0; i < QRTR_MUX_MAX_CLIENTS; i++) {
		clients[i].fd = -1;
		clients[i].port = QRTR_MUX_PORT_BASE + i;
	}
	clients_inited = 1;
}

static void client_drop(struct qrtr_mux_client *c, const char *why)
{
	if (c->fd < 0)
		return;
	printf("[%6llu] qrtr-mux: client port %#x closed (%s)\n",
	       (unsigned long long)link_ms(), c->port, why);
	close(c->fd);
	c->fd = -1;
	c->rx_len = 0;
	c->tx_head = 0;
	c->tx_len = 0;
	/* Notify MPSS (node 0) so QMI services release client state for this port */
	uint32_t pkt[5] = { QRTR_TYPE_DEL_CLIENT, QRTR_NODE_APPS, c->port, 0, 0 };
	(void)qrtr_send(QRTR_TYPE_DEL_CLIENT, QRTR_PORT_CTRL, QRTR_NODE_MODEM,
			QRTR_PORT_CTRL, pkt, sizeof(pkt));
	link_note_activity();
}

static void client_flush_tx(struct qrtr_mux_client *c)
{
	if (c->fd < 0 || !c->tx_len)
		return;
	while (c->tx_len > 0) {
		ssize_t w = send(c->fd, c->tx_buf + c->tx_head, c->tx_len, 0);
		if (w > 0) {
			c->tx_head += (size_t)w;
			c->tx_len -= (size_t)w;
			if (!c->tx_len)
				c->tx_head = 0;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		client_drop(c, "tx error");
		return;
	}
}

static int client_queue_msg(struct qrtr_mux_client *c, const struct hot_qrtr_msg *hdr,
			    const uint8_t *payload)
{
	if (c->fd < 0)
		return -1;
	size_t need = sizeof(*hdr) + hdr->len;
	if (need > MUX_TX_CAP)
		return -1;
	if (c->tx_head > 0 && c->tx_head + c->tx_len + need > MUX_TX_CAP) {
		memmove(c->tx_buf, c->tx_buf + c->tx_head, c->tx_len);
		c->tx_head = 0;
	}
	if (c->tx_head + c->tx_len + need > MUX_TX_CAP) {
		client_drop(c, "tx overflow");
		return -1;
	}
	size_t off = c->tx_head + c->tx_len;
	memcpy(c->tx_buf + off, hdr, sizeof(*hdr));
	if (hdr->len && payload)
		memcpy(c->tx_buf + off + sizeof(*hdr), payload, hdr->len);
	c->tx_len += need;
	client_flush_tx(c);
	return 0;
}

int qrtr_mux_open(void)
{
	init_slots();
	if (listen_fd >= 0)
		return 0;
	(void)mkdir("/var/run", 0755);
	(void)unlink(HOT_QRTR_SOCK);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		printf("qrtr-mux: socket: %s\n", strerror(errno));
		return -1;
	}
#ifdef SO_NOSIGPIPE
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", HOT_QRTR_SOCK);
#ifdef __APPLE__
	sun.sun_len = (uint8_t)sizeof(sun);
#endif

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		printf("qrtr-mux: bind %s: %s\n", HOT_QRTR_SOCK, strerror(errno));
		close(fd);
		return -1;
	}
	(void)chmod(HOT_QRTR_SOCK, 0660);
	if (listen(fd, QRTR_MUX_MAX_CLIENTS) < 0) {
		printf("qrtr-mux: listen: %s\n", strerror(errno));
		close(fd);
		(void)unlink(HOT_QRTR_SOCK);
		return -1;
	}
	listen_fd = fd;
	printf("[%6llu] qrtr-mux: listening on %s (ports %#x..%#x)\n",
	       (unsigned long long)link_ms(), HOT_QRTR_SOCK,
	       QRTR_MUX_PORT_BASE, QRTR_MUX_PORT_BASE + QRTR_MUX_MAX_CLIENTS - 1);
	return 0;
}

void qrtr_mux_on_ctrl(uint32_t cmd, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	init_slots();
	if (node == QRTR_NODE_APPS)
		return;
	if (cmd == QRTR_TYPE_NEW_SERVER) {
		if (service == 0 && port == 0)
			return;
		int slot = -1;
		for (unsigned i = 0; i < QRTR_MUX_MAX_SERVERS; i++) {
			if (srv_cache[i].used && srv_cache[i].node == node && srv_cache[i].port == port &&
			    srv_cache[i].service == service) {
				slot = (int)i;
				break;
			}
			if (!srv_cache[i].used && slot < 0)
				slot = (int)i;
		}
		if (slot >= 0) {
			srv_cache[slot].used = 1;
			srv_cache[slot].service = service;
			srv_cache[slot].instance = instance;
			srv_cache[slot].node = node;
			srv_cache[slot].port = port;
		}
		struct hot_qrtr_msg msg = {
			.type = HOT_QRTR_TYPE_NEW_SERVER,
			.node = node,
			.port = port,
			.service = service,
			.instance = instance,
			.len = 0,
		};
		for (unsigned i = 0; i < QRTR_MUX_MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0)
				(void)client_queue_msg(&clients[i], &msg, NULL);
		}
	} else if (cmd == QRTR_TYPE_DEL_SERVER) {
		for (unsigned i = 0; i < QRTR_MUX_MAX_SERVERS; i++) {
			if (srv_cache[i].used && srv_cache[i].node == node && srv_cache[i].port == port)
				srv_cache[i].used = 0;
		}
		struct hot_qrtr_msg msg = {
			.type = HOT_QRTR_TYPE_DEL_SERVER,
			.node = node,
			.port = port,
			.service = service,
			.instance = instance,
			.len = 0,
		};
		for (unsigned i = 0; i < QRTR_MUX_MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0)
				(void)client_queue_msg(&clients[i], &msg, NULL);
		}
	}
}

int qrtr_mux_deliver_data(uint32_t dst_port, uint32_t src_node, uint32_t src_port,
			  const uint8_t *data, uint32_t len)
{
	init_slots();
	if (dst_port < QRTR_MUX_PORT_BASE || dst_port >= QRTR_MUX_PORT_BASE + QRTR_MUX_MAX_CLIENTS)
		return -1;
	struct qrtr_mux_client *c = &clients[dst_port - QRTR_MUX_PORT_BASE];
	if (c->fd < 0 || len > HOT_QRTR_MAX_DATA)
		return 0;
	link_note_activity();
	struct hot_qrtr_msg hdr = {
		.type = HOT_QRTR_TYPE_DATA,
		.node = src_node,
		.port = src_port,
		.service = 0,
		.instance = 0,
		.len = len,
	};
	return client_queue_msg(c, &hdr, data);
}

static void accept_clients(void)
{
	if (listen_fd < 0)
		return;
	for (;;) {
		int cfd = accept(listen_fd, NULL, NULL);
		if (cfd < 0)
			break;
#ifdef SO_NOSIGPIPE
		int one = 1;
		(void)setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		int fl = fcntl(cfd, F_GETFL, 0);
		if (fl >= 0)
			(void)fcntl(cfd, F_SETFL, fl | O_NONBLOCK);

		int slot = -1;
		for (unsigned i = 0; i < QRTR_MUX_MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) {
				slot = (int)i;
				break;
			}
		}
		if (slot < 0) {
			printf("[%6llu] qrtr-mux: full, rejecting client\n",
			       (unsigned long long)link_ms());
			close(cfd);
			continue;
		}
		struct qrtr_mux_client *c = &clients[slot];
		c->fd = cfd;
		c->port = QRTR_MUX_PORT_BASE + (uint32_t)slot;
		c->rx_len = 0;
		c->tx_head = 0;
		c->tx_len = 0;
		link_note_activity();

		unsigned replayed = 0;
		for (unsigned i = 0; i < QRTR_MUX_MAX_SERVERS; i++) {
			if (!srv_cache[i].used)
				continue;
			struct hot_qrtr_msg msg = {
				.type = HOT_QRTR_TYPE_NEW_SERVER,
				.node = srv_cache[i].node,
				.port = srv_cache[i].port,
				.service = srv_cache[i].service,
				.instance = srv_cache[i].instance,
				.len = 0,
			};
			if (client_queue_msg(c, &msg, NULL) == 0)
				replayed++;
			if (c->fd < 0)
				break;
		}
		if (c->fd >= 0) {
			printf("[%6llu] qrtr-mux: client connected on port %#x (replayed %u servers)\n",
			       (unsigned long long)link_ms(), c->port, replayed);
		}
	}
}

static void client_poll_rx(struct qrtr_mux_client *c)
{
	if (c->fd < 0)
		return;
	for (int iter = 0; iter < 16 && c->fd >= 0; iter++) {
		size_t space = sizeof(c->rx_buf) - c->rx_len;
		if (!space) {
			client_drop(c, "rx overflow");
			return;
		}
		ssize_t r = recv(c->fd, c->rx_buf + c->rx_len, space, 0);
		if (r == 0) {
			client_drop(c, "eof");
			return;
		}
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			client_drop(c, "rx error");
			return;
		}
		c->rx_len += (size_t)r;
		link_note_activity();

		while (c->rx_len >= sizeof(struct hot_qrtr_msg)) {
			struct hot_qrtr_msg hdr;
			memcpy(&hdr, c->rx_buf, sizeof(hdr));
			if (hdr.len > HOT_QRTR_MAX_DATA) {
				client_drop(c, "bad frame len");
				return;
			}
			size_t frame_len = sizeof(hdr) + hdr.len;
			if (c->rx_len < frame_len)
				break;
			const uint8_t *payload = c->rx_buf + sizeof(hdr);
			if (hdr.type == HOT_QRTR_TYPE_DATA && hdr.len > 0) {
				(void)qrtr_send(QRTR_TYPE_DATA, c->port, hdr.node, hdr.port,
						payload, hdr.len);
			}
			size_t rem = c->rx_len - frame_len;
			if (rem > 0)
				memmove(c->rx_buf, c->rx_buf + frame_len, rem);
			c->rx_len = rem;
		}
	}
}

void qrtr_mux_poll(void)
{
	init_slots();
	accept_clients();
	for (unsigned i = 0; i < QRTR_MUX_MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0) {
			client_flush_tx(&clients[i]);
			client_poll_rx(&clients[i]);
		}
	}
}

void qrtr_mux_wait(unsigned ms)
{
	if (!ms)
		return;
	struct pollfd pfds[1 + QRTR_MUX_MAX_CLIENTS];
	nfds_t nfds = 0;
	if (listen_fd >= 0) {
		pfds[nfds].fd = listen_fd;
		pfds[nfds].events = POLLIN;
		pfds[nfds].revents = 0;
		nfds++;
	}
	if (clients_inited) {
		for (unsigned i = 0; i < QRTR_MUX_MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfds[nfds].fd = clients[i].fd;
			pfds[nfds].events = POLLIN | (clients[i].tx_len ? POLLOUT : 0);
			pfds[nfds].revents = 0;
			nfds++;
		}
	}
	if (!nfds) {
		delay_ms(ms);
		return;
	}
	int rc = poll(pfds, nfds, (int)ms);
	if (rc > 0)
		link_note_activity();
}
