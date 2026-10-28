/*
 * Shared QMI-over-QRTR client library for qmicli and cellulard.
 * Connects to /var/run/sweet-qrtr.sock (multiplexed by wland onto MPSS node 0).
 *
 * PRIVACY: Never requests, parses, logs, or serializes IMEI, IMEISV, IMSI,
 * ICCID, or subscriber phone numbers (MSISDN).
 */
#include "qmi_cellular.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void copy_clean_ascii(char *dst, size_t cap, const uint8_t *src, size_t slen)
{
	if (!cap)
		return;
	size_t o = 0;
	for (size_t i = 0; i < slen && o + 1 < cap; i++) {
		uint8_t c = src[i];
		if (c == 0 || c == 0xff)
			break;
		if (c >= 0x20 && c < 0x7f)
			dst[o++] = (char)c;
	}
	while (o > 0 && dst[o - 1] == ' ')
		o--;
	dst[o] = '\0';
}

/* Decode 3GPP TS 31.102 EF_SPN (0x6F46): byte 0 = display condition, bytes 1..16 = name */
static void decode_ef_spn(char *dst, size_t cap, const uint8_t *raw, size_t rlen)
{
	dst[0] = '\0';
	if (rlen < 2 || !cap)
		return;
	const uint8_t *p = raw + 1;
	size_t n = rlen - 1;
	if (n >= 3 && p[0] == 0x80) {
		/* UCS2 big-endian */
		size_t o = 0;
		for (size_t i = 1; i + 1 < n && o + 1 < cap; i += 2) {
			if (p[i] == 0xff && p[i + 1] == 0xff)
				break;
			if (p[i] == 0x00 && p[i + 1] >= 0x20 && p[i + 1] < 0x7f)
				dst[o++] = (char)p[i + 1];
		}
		while (o > 0 && dst[o - 1] == ' ')
			o--;
		dst[o] = '\0';
		return;
	}
	copy_clean_ascii(dst, cap, p, n);
}

void cellular_state_init(struct cellular_state *st)
{
	memset(st, 0, sizeof(*st));
	snprintf(st->sim_state, sizeof(st->sim_state), "unknown");
	snprintf(st->app_type, sizeof(st->app_type), "none");
	snprintf(st->app_state, sizeof(st->app_state), "none");
	snprintf(st->radio_mode, sizeof(st->radio_mode), "unknown");
	snprintf(st->reg_state, sizeof(st->reg_state), "not_registered");
	snprintf(st->rat, sizeof(st->rat), "none");
	st->dms_mode_raw = 0xff;
}

int qmi_open(struct qmi_ctx *ctx, const char *sock_path)
{
	qmi_ind_fn saved_cb = ctx->ind_cb;
	void *saved_user = ctx->ind_user;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fd = -1;
	ctx->next_txn = 1;
	ctx->ind_cb = saved_cb;
	ctx->ind_user = saved_user;

	const char *path = (sock_path && sock_path[0]) ? sock_path : SWEET_QRTR_SOCK;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
#ifdef SO_NOSIGPIPE
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);
#ifdef __APPLE__
	sun.sun_len = (uint8_t)sizeof(sun);
#endif
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		close(fd);
		return -1;
	}
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	ctx->fd = fd;
	return 0;
}

void qmi_close(struct qmi_ctx *ctx)
{
	if (ctx->fd >= 0) {
		close(ctx->fd);
		ctx->fd = -1;
	}
	ctx->rx_len = 0;
	ctx->ind_reg_mask = 0;
	ctx->provision_attempted = false;
	memset(ctx->servers, 0, sizeof(ctx->servers));
}

static int send_all_nb(int fd, const void *buf, size_t len, unsigned timeout_ms)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t off = 0;
	uint64_t end = mono_ms() + timeout_ms;
	while (off < len) {
		ssize_t w = send(fd, p + off, len - off, 0);
		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			uint64_t now = mono_ms();
			if (now >= end)
				return -1;
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			(void)poll(&pfd, 1, (int)(end - now));
			continue;
		}
		return -1;
	}
	return 0;
}

static void record_server(struct qmi_ctx *ctx, const struct sweet_qrtr_msg *m)
{
	if (m->type == SWEET_QRTR_TYPE_NEW_SERVER) {
		int free_idx = -1;
		for (int i = 0; i < QMI_MAX_SERVERS; i++) {
			if (ctx->servers[i].valid &&
			    ctx->servers[i].node == m->node &&
			    ctx->servers[i].port == m->port &&
			    ctx->servers[i].service == m->service) {
				free_idx = i;
				break;
			}
			if (!ctx->servers[i].valid && free_idx < 0)
				free_idx = i;
		}
		if (free_idx >= 0) {
			ctx->servers[free_idx].valid = true;
			ctx->servers[free_idx].service = m->service;
			ctx->servers[free_idx].instance = m->instance;
			ctx->servers[free_idx].node = m->node;
			ctx->servers[free_idx].port = m->port;
		}
	} else if (m->type == SWEET_QRTR_TYPE_DEL_SERVER) {
		for (int i = 0; i < QMI_MAX_SERVERS; i++) {
			if (ctx->servers[i].valid &&
			    ctx->servers[i].node == m->node &&
			    ctx->servers[i].port == m->port) {
				uint32_t svc = ctx->servers[i].service;
				ctx->servers[i].valid = false;
				if (svc == QMI_SVC_DMS)
					ctx->ind_reg_mask &= ~QMI_IND_MASK_DMS;
				else if (svc == QMI_SVC_UIM) {
					ctx->ind_reg_mask &= ~QMI_IND_MASK_UIM;
					ctx->provision_attempted = false;
				} else if (svc == QMI_SVC_NAS)
					ctx->ind_reg_mask &= ~QMI_IND_MASK_NAS;
				else if (svc == QMI_SVC_WDS)
					ctx->ind_reg_mask &= ~QMI_IND_MASK_WDS;
			}
		}
	}
}

static uint32_t service_for_addr(const struct qmi_ctx *ctx, uint32_t node, uint32_t port)
{
	for (int i = 0; i < QMI_MAX_SERVERS; i++) {
		if (ctx->servers[i].valid &&
		    ctx->servers[i].node == node &&
		    ctx->servers[i].port == port)
			return ctx->servers[i].service;
	}
	return 0;
}

static const struct qmi_srv_record *find_service_rec(const struct qmi_ctx *ctx, uint32_t service)
{
	const struct qmi_srv_record *fallback = NULL;
	for (int i = 0; i < QMI_MAX_SERVERS; i++) {
		if (!ctx->servers[i].valid || ctx->servers[i].service != service)
			continue;
		/* Prefer QMI instance 0 (low byte = version, upper = instance) if multiple exist */
		if ((ctx->servers[i].instance >> 8) == 0)
			return &ctx->servers[i];
		if (!fallback)
			fallback = &ctx->servers[i];
	}
	return fallback;
}

bool qmi_has_service(const struct qmi_ctx *ctx, uint32_t service)
{
	return find_service_rec(ctx, service) != NULL;
}

static int pump_once(struct qmi_ctx *ctx, uint32_t want_svc, uint16_t want_txn,
		     uint16_t want_msg, uint8_t *rsp_tlvs, size_t rsp_cap,
		     size_t *rsp_len, bool *got_rsp)
{
	if (ctx->fd < 0)
		return -1;
	for (;;) {
		size_t space = sizeof(ctx->rx_buf) - ctx->rx_len;
		if (!space)
			return -1;
		ssize_t r = recv(ctx->fd, ctx->rx_buf + ctx->rx_len, space, 0);
		if (r == 0)
			return -1;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -1;
		}
		ctx->rx_len += (size_t)r;

		while (ctx->rx_len >= sizeof(struct sweet_qrtr_msg)) {
			struct sweet_qrtr_msg hdr;
			memcpy(&hdr, ctx->rx_buf, sizeof(hdr));
			if (hdr.len > SWEET_QRTR_MAX_DATA)
				return -1;
			size_t frame_len = sizeof(hdr) + hdr.len;
			if (ctx->rx_len < frame_len)
				break;
			const uint8_t *p = ctx->rx_buf + sizeof(hdr);
			if (hdr.type == SWEET_QRTR_TYPE_NEW_SERVER ||
			    hdr.type == SWEET_QRTR_TYPE_DEL_SERVER) {
				record_server(ctx, &hdr);
			} else if (hdr.type == SWEET_QRTR_TYPE_DATA && hdr.len >= 7) {
				uint8_t flags = p[0];
				uint16_t txn = rd_le16(p + 1);
				uint16_t msg_id = rd_le16(p + 3);
				uint16_t mlen = rd_le16(p + 5);
				size_t avail_tlv = hdr.len - 7;
				if (mlen < avail_tlv)
					avail_tlv = mlen;
				uint32_t svc = service_for_addr(ctx, hdr.node, hdr.port);
				if ((flags & QMI_FLG_IND) && ctx->ind_cb && svc) {
					ctx->ind_cb(ctx, svc, msg_id, p + 7, avail_tlv, ctx->ind_user);
				} else if ((flags & QMI_FLG_RSP) && want_txn &&
					   svc == want_svc && txn == want_txn && msg_id == want_msg) {
					if (rsp_tlvs && rsp_cap > 0) {
						size_t cp = avail_tlv < rsp_cap ? avail_tlv : rsp_cap;
						memcpy(rsp_tlvs, p + 7, cp);
						if (rsp_len)
							*rsp_len = cp;
					} else if (rsp_len) {
						*rsp_len = 0;
					}
					if (got_rsp)
						*got_rsp = true;
				}
			}
			size_t rem = ctx->rx_len - frame_len;
			if (rem > 0)
				memmove(ctx->rx_buf, ctx->rx_buf + frame_len, rem);
			ctx->rx_len = rem;
		}
	}
	return 0;
}

int qmi_drain_rx(struct qmi_ctx *ctx, unsigned wait_ms)
{
	if (ctx->fd < 0)
		return -1;
	if (wait_ms > 0) {
		struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
		(void)poll(&pfd, 1, (int)wait_ms);
	}
	return pump_once(ctx, 0, 0, 0, NULL, 0, NULL, NULL);
}

int qmi_sync_servers(struct qmi_ctx *ctx, unsigned wait_ms)
{
	uint64_t end = mono_ms() + wait_ms;
	do {
		if (qmi_drain_rx(ctx, 20) < 0)
			return -1;
		if (qmi_has_service(ctx, QMI_SVC_DMS) &&
		    qmi_has_service(ctx, QMI_SVC_UIM) &&
		    qmi_has_service(ctx, QMI_SVC_NAS))
			return 0;
	} while (mono_ms() < end);
	return 0;
}

const uint8_t *qmi_tlv_find(const uint8_t *buf, size_t len, uint8_t type, uint16_t *out_len)
{
	size_t off = 0;
	while (off + 3 <= len) {
		uint8_t t = buf[off];
		uint16_t l = rd_le16(buf + off + 1);
		if (off + 3 + l > len)
			break;
		if (t == type) {
			if (out_len)
				*out_len = l;
			return buf + off + 3;
		}
		off += 3 + l;
	}
	return NULL;
}

int qmi_check_result(const uint8_t *tlvs, size_t len, uint16_t *qmi_err)
{
	uint16_t rlen = 0;
	const uint8_t *r = qmi_tlv_find(tlvs, len, 0x02, &rlen);
	if (!r || rlen < 4) {
		if (qmi_err)
			*qmi_err = 0;
		return 0;
	}
	uint16_t res = rd_le16(r);
	uint16_t err = rd_le16(r + 2);
	if (qmi_err)
		*qmi_err = err;
	return res == 0 ? 0 : -1;
}

int qmi_tlv_put(uint8_t *buf, size_t cap, size_t *off, uint8_t type,
		const void *val, uint16_t vlen)
{
	if (*off + 3 + vlen > cap)
		return -1;
	buf[*off] = type;
	wr_le16(buf + *off + 1, vlen);
	if (vlen && val)
		memcpy(buf + *off + 3, val, vlen);
	*off += 3 + vlen;
	return 0;
}

int qmi_request(struct qmi_ctx *ctx, uint32_t service, uint16_t msg_id,
		const uint8_t *req_tlvs, size_t req_len,
		uint8_t *rsp_tlvs, size_t rsp_cap, size_t *rsp_len,
		unsigned timeout_ms)
{
	if (ctx->fd < 0 || req_len > 4096)
		return -1;
	const struct qmi_srv_record *srv = find_service_rec(ctx, service);
	if (!srv)
		return -ENOENT;

	uint16_t txn = ctx->next_txn++;
	if (!txn)
		txn = ctx->next_txn++;

	uint8_t frame[sizeof(struct sweet_qrtr_msg) + 7 + 4096];
	struct sweet_qrtr_msg hdr = {
		.type = SWEET_QRTR_TYPE_DATA,
		.node = srv->node,
		.port = srv->port,
		.service = service,
		.instance = srv->instance,
		.len = (uint32_t)(7 + req_len),
	};
	memcpy(frame, &hdr, sizeof(hdr));
	uint8_t *q = frame + sizeof(hdr);
	q[0] = QMI_FLG_REQ;
	wr_le16(q + 1, txn);
	wr_le16(q + 3, msg_id);
	wr_le16(q + 5, (uint16_t)req_len);
	if (req_len && req_tlvs)
		memcpy(q + 7, req_tlvs, req_len);

	if (send_all_nb(ctx->fd, frame, sizeof(hdr) + hdr.len, 500) < 0)
		return -EIO;

	bool got_rsp = false;
	uint64_t end = mono_ms() + (timeout_ms ? timeout_ms : 1500);
	while (!got_rsp) {
		if (pump_once(ctx, service, txn, msg_id, rsp_tlvs, rsp_cap, rsp_len, &got_rsp) < 0)
			return -EIO;
		if (got_rsp)
			return 0;
		uint64_t now = mono_ms();
		if (now >= end)
			return -ETIMEDOUT;
		struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
		(void)poll(&pfd, 1, (int)(end - now));
	}
	return 0;
}

/* ---- PLMN fallback lookup for common carriers when SIM/NITZ omits ASCII name ---- */
static const char *lookup_plmn_fallback(const char *mcc, const char *mnc)
{
	static const struct { const char *mcc; const char *mnc; const char *name; } tbl[] = {
		/* India (404 / 405) */
		{ "405", "840", "Jio" }, { "405", "854", "Jio" }, { "405", "855", "Jio" },
		{ "405", "856", "Jio" }, { "405", "857", "Jio" }, { "405", "858", "Jio" },
		{ "405", "859", "Jio" }, { "405", "860", "Jio" }, { "405", "861", "Jio" },
		{ "405", "862", "Jio" }, { "405", "863", "Jio" }, { "405", "864", "Jio" },
		{ "405", "865", "Jio" }, { "405", "866", "Jio" }, { "405", "867", "Jio" },
		{ "405", "868", "Jio" }, { "405", "869", "Jio" }, { "405", "870", "Jio" },
		{ "405", "871", "Jio" }, { "405", "872", "Jio" }, { "405", "873", "Jio" },
		{ "405", "874", "Jio" },
		{ "404", "10", "Airtel" }, { "404", "31", "Airtel" }, { "404", "40", "Airtel" },
		{ "404", "45", "Airtel" }, { "404", "49", "Airtel" }, { "404", "70", "Airtel" },
		{ "404", "90", "Airtel" }, { "404", "92", "Airtel" }, { "404", "93", "Airtel" },
		{ "404", "94", "Airtel" }, { "404", "95", "Airtel" }, { "404", "96", "Airtel" },
		{ "404", "97", "Airtel" }, { "404", "98", "Airtel" }, { "405", "51", "Airtel" },
		{ "405", "52", "Airtel" }, { "405", "53", "Airtel" }, { "405", "54", "Airtel" },
		{ "405", "55", "Airtel" }, { "405", "56", "Airtel" },
		{ "404", "04", "Vi India" }, { "404", "05", "Vi India" }, { "404", "11", "Vi India" },
		{ "404", "13", "Vi India" }, { "404", "15", "Vi India" }, { "404", "20", "Vi India" },
		{ "404", "22", "Vi India" }, { "404", "27", "Vi India" }, { "404", "30", "Vi India" },
		{ "404", "43", "Vi India" }, { "404", "46", "Vi India" }, { "404", "84", "Vi India" },
		{ "404", "86", "Vi India" }, { "404", "88", "Vi India" },
		{ "404", "34", "BSNL" }, { "404", "38", "BSNL" }, { "404", "51", "BSNL" },
		{ "404", "53", "BSNL" }, { "404", "54", "BSNL" }, { "404", "55", "BSNL" },
		{ "404", "57", "BSNL" }, { "404", "58", "BSNL" }, { "404", "59", "BSNL" },
		{ "404", "62", "BSNL" }, { "404", "64", "BSNL" }, { "404", "66", "BSNL" },
		{ "404", "71", "BSNL" }, { "404", "72", "BSNL" }, { "404", "73", "BSNL" },
		{ "404", "74", "BSNL" }, { "404", "75", "BSNL" }, { "404", "76", "BSNL" },
		{ "404", "77", "BSNL" }, { "404", "80", "BSNL" }, { "404", "81", "BSNL" },
		/* US / EU / Global */
		{ "310", "260", "T-Mobile" }, { "310", "410", "AT&T" }, { "311", "480", "Verizon" },
		{ "234", "10", "O2 - UK" }, { "234", "15", "Vodafone UK" }, { "234", "30", "EE" },
		{ "262", "01", "Telekom.de" }, { "262", "02", "Vodafone.de" }, { "262", "03", "o2 - de" },
	};
	if (!mcc[0] || !mnc[0])
		return NULL;
	int mnc_num = atoi(mnc);
	for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		if (!strcmp(mcc, tbl[i].mcc) &&
		    (!strcmp(mnc, tbl[i].mnc) || atoi(tbl[i].mnc) == mnc_num))
			return tbl[i].name;
	}
	return NULL;
}

static const char *dms_mode_str(uint8_t m)
{
	switch (m) {
	case 0: return "online";
	case 1: return "low_power";
	case 2: return "factory_test";
	case 3: return "offline";
	case 4: return "resetting";
	case 5: return "shutting_down";
	case 6: return "persistent_low_power";
	case 7: return "mode_only_low_power";
	default: return "unknown";
	}
}

static const char *nas_reg_str(uint8_t r)
{
	switch (r) {
	case 0: return "not_registered";
	case 1: return "registered";
	case 2: return "searching";
	case 3: return "denied";
	default: return "unknown";
	}
}

static const char *nas_rat_str(int8_t rif)
{
	switch (rif) {
	case 1:  return "cdma_1x";
	case 2:  return "evdo";
	case 4:  return "gsm";
	case 5:  return "umts";
	case 8:  return "lte";
	case 9:  return "tdscdma";
	case 12: return "5g_nr";
	default: return "none";
	}
}

void cellular_recompute_derived(struct cellular_state *st)
{
	/* Resolve operator_name priority: serving PLMN -> SPN -> home network -> PLMN table -> MCC-MNC */
	const char *best = NULL;
	if (st->plmn_name[0])
		best = st->plmn_name;
	else if (st->spn_name[0])
		best = st->spn_name;
	else if (st->home_name[0])
		best = st->home_name;
	else
		best = lookup_plmn_fallback(st->mcc, st->mnc);

	if (best && best[0]) {
		snprintf(st->operator_name, sizeof(st->operator_name), "%s", best);
	} else if (st->mcc[0] && st->mnc[0]) {
		snprintf(st->operator_name, sizeof(st->operator_name), "%s %s", st->mcc, st->mnc);
	} else {
		st->operator_name[0] = '\0';
	}

	/* Compute 0..4 signal bars from LTE/NR RSRP (primary) or RSSI (fallback) */
	if (!st->sim_present || st->reg_state_raw != 1 ||
	    (st->dms_mode_raw != 0 && st->dms_mode_raw != 0xff)) {
		st->bars_0_to_4 = 0;
		return;
	}
	if (st->has_rsrp && st->rsrp_dbm >= -145 && st->rsrp_dbm <= -40) {
		if (st->rsrp_dbm >= -85)
			st->bars_0_to_4 = 4;
		else if (st->rsrp_dbm >= -95)
			st->bars_0_to_4 = 3;
		else if (st->rsrp_dbm >= -105)
			st->bars_0_to_4 = 2;
		else if (st->rsrp_dbm >= -122)
			st->bars_0_to_4 = 1;
		else
			st->bars_0_to_4 = 0;
	} else if (st->has_rssi && st->rssi_dbm >= -120 && st->rssi_dbm <= -30) {
		if (st->rssi_dbm >= -70)
			st->bars_0_to_4 = 4;
		else if (st->rssi_dbm >= -80)
			st->bars_0_to_4 = 3;
		else if (st->rssi_dbm >= -90)
			st->bars_0_to_4 = 2;
		else if (st->rssi_dbm >= -104)
			st->bars_0_to_4 = 1;
		else
			st->bars_0_to_4 = 0;
	} else {
		/* Registered on cell, signal info pending -> show 1 bar minimum */
		st->bars_0_to_4 = 1;
	}
}

int qmi_dms_set_operating_mode_ex(struct qmi_ctx *ctx, uint8_t mode, uint16_t *out_qmi_err)
{
	uint8_t req[8], rsp[256];
	size_t req_len = 0, rsp_len = 0;
	if (out_qmi_err)
		*out_qmi_err = 0;
	qmi_tlv_put(req, sizeof(req), &req_len, 0x01, &mode, 1);
	int rc = qmi_request(ctx, QMI_SVC_DMS, QMI_DMS_SET_OPERATING_MODE,
			     req, req_len, rsp, sizeof(rsp), &rsp_len, 3000);
	if (rc < 0) {
		if (out_qmi_err)
			*out_qmi_err = 0xffffu;
		return rc;
	}
	return qmi_check_result(rsp, rsp_len, out_qmi_err);
}

int qmi_dms_set_operating_mode(struct qmi_ctx *ctx, uint8_t mode)
{
	return qmi_dms_set_operating_mode_ex(ctx, mode, NULL);
}

/* Parse UIM Card Status TLV 0x10 (shared by UIM_GET_CARD_STATUS and UIM_STATUS_CHANGE_IND) */
static void parse_uim_card_status(struct qmi_ctx *ctx, struct cellular_state *st,
				  const uint8_t *v, size_t vlen,
				  uint8_t *out_aid, uint8_t *out_aid_len,
				  int *out_slot_idx, uint8_t *out_app_type)
{
	if (out_aid_len) *out_aid_len = 0;
	if (out_slot_idx) *out_slot_idx = -1;
	if (out_app_type) *out_app_type = 0;

	if (vlen < 9)
		return;
	uint16_t idx_gw_pri = rd_le16(v + 0);
	uint8_t num_slots = v[8];
	st->gw_provisioned = (idx_gw_pri != 0xffffu);

	size_t off = 9;
	bool found_present = false;

	for (uint8_t s = 0; s < num_slots; s++) {
		if (off + 6 > vlen)
			break;
		uint8_t card_state = v[off + 0];
		uint8_t upin_state = v[off + 1];
		uint8_t upin_retries = v[off + 2];
		uint8_t upuk_retries = v[off + 3];
		uint8_t err_code = v[off + 4];
		uint8_t num_app = v[off + 5];
		off += 6;

		uint8_t best_app_type = 0;
		uint8_t best_app_state = 0;
		uint8_t best_pin1_state = upin_state;
		uint8_t best_pin1_ret = upin_retries;
		uint8_t best_puk1_ret = upuk_retries;
		uint8_t best_aid[32];
		uint8_t best_aid_len = 0;

		for (uint8_t a = 0; a < num_app; a++) {
			if (off + 7 > vlen) {
				off = vlen;
				break;
			}
			uint8_t app_type = v[off + 0];
			uint8_t app_state = v[off + 1];
			uint8_t aid_len = v[off + 6];
			off += 7;
			if (off + aid_len + 7 > vlen) {
				off = vlen;
				break;
			}
			const uint8_t *aid_ptr = v + off;
			off += aid_len;
			uint8_t univ_pin = v[off + 0];
			uint8_t pin1_state = v[off + 1];
			uint8_t pin1_ret = v[off + 2];
			uint8_t puk1_ret = v[off + 3];
			off += 7;

			/* Prefer USIM (2) over SIM (1) over others */
			if (app_type == 2 || (app_type == 1 && best_app_type != 2) || best_app_type == 0) {
				best_app_type = app_type;
				best_app_state = app_state;
				if (!univ_pin) {
					best_pin1_state = pin1_state;
					best_pin1_ret = pin1_ret;
					best_puk1_ret = puk1_ret;
				}
				best_aid_len = aid_len < sizeof(best_aid) ? aid_len : sizeof(best_aid);
				if (best_aid_len)
					memcpy(best_aid, aid_ptr, best_aid_len);
			}
		}

		/* card_state == 1 means physically present and ATR succeeded */
		if (card_state == 1 && !found_present) {
			found_present = true;
			st->sim_present = true;
			st->sim_slot = (int)s + 1;
			st->pin1_retries = best_pin1_ret;
			st->puk1_retries = best_puk1_ret;

			if (best_app_type == 2)
				snprintf(st->app_type, sizeof(st->app_type), "usim");
			else if (best_app_type == 1)
				snprintf(st->app_type, sizeof(st->app_type), "sim");
			else
				snprintf(st->app_type, sizeof(st->app_type), "other");

			switch (best_app_state) {
			case 7:
				snprintf(st->app_state, sizeof(st->app_state), "ready");
				snprintf(st->sim_state, sizeof(st->sim_state), "present");
				break;
			case 2:
				snprintf(st->app_state, sizeof(st->app_state), "pin1_req");
				snprintf(st->sim_state, sizeof(st->sim_state), "pin_locked");
				break;
			case 3:
				snprintf(st->app_state, sizeof(st->app_state), "puk1_req");
				snprintf(st->sim_state, sizeof(st->sim_state), "puk_locked");
				break;
			case 4:
				snprintf(st->app_state, sizeof(st->app_state), "perso_req");
				snprintf(st->sim_state, sizeof(st->sim_state), "network_locked");
				break;
			case 1:
				snprintf(st->app_state, sizeof(st->app_state), "detected");
				snprintf(st->sim_state, sizeof(st->sim_state), "present");
				break;
			default:
				snprintf(st->app_state, sizeof(st->app_state),
					 best_pin1_state == 1 ? "pin1_req" : "unknown");
				snprintf(st->sim_state, sizeof(st->sim_state), "present");
				break;
			}

			if (out_slot_idx) *out_slot_idx = (int)s;
			if (out_app_type) *out_app_type = best_app_type;
			if (out_aid && out_aid_len && best_aid_len) {
				memcpy(out_aid, best_aid, best_aid_len);
				*out_aid_len = best_aid_len;
			}
		} else if (!found_present && s == 0) {
			/* Record slot 1 empty/error reason if no SIM in any slot */
			st->sim_present = false;
			st->sim_slot = 0;
			snprintf(st->app_type, sizeof(st->app_type), "none");
			snprintf(st->app_state, sizeof(st->app_state), "none");
			if (card_state == 0 || (card_state == 2 && (err_code == 3 || err_code == 6 || err_code == 1)))
				snprintf(st->sim_state, sizeof(st->sim_state), "absent");
			else
				snprintf(st->sim_state, sizeof(st->sim_state), "absent");
		}
	}
	(void)ctx;
}

static void uim_read_spn(struct qmi_ctx *ctx, struct cellular_state *st,
			 int slot_idx, uint8_t app_type, const uint8_t *aid, uint8_t aid_len)
{
	if (!st->sim_present || strcmp(st->app_state, "ready") != 0)
		return;
	uint8_t req[64], rsp[256];
	size_t req_len = 0, rsp_len = 0;

	/* TLV 0x01: Session Info. Use primary GW (0x00, aid_len=0) if provisioned, else slot non-prov */
	uint8_t sess[34];
	size_t sess_len = 0;
	if (st->gw_provisioned) {
		sess[0] = 0x00;
		sess[1] = 0x00;
		sess_len = 2;
	} else if (slot_idx >= 0 && aid_len > 0 && aid_len <= 32) {
		sess[0] = (slot_idx == 0) ? 0x06 : 0x07;
		sess[1] = aid_len;
		memcpy(sess + 2, aid, aid_len);
		sess_len = 2 + aid_len;
	} else {
		return;
	}
	qmi_tlv_put(req, sizeof(req), &req_len, 0x01, sess, (uint16_t)sess_len);

	/* TLV 0x02: File ID 0x6F46 (EF_SPN) under MF 3F00 + ADF_USIM 7FFF (or DF_GSM 7F20) */
	uint8_t fid[7];
	wr_le16(fid, 0x6f46u);
	fid[2] = 4;
	fid[3] = 0x00; fid[4] = 0x3f;
	if (app_type == 1) {
		fid[5] = 0x20; fid[6] = 0x7f;
	} else {
		fid[5] = 0xff; fid[6] = 0x7f;
	}
	qmi_tlv_put(req, sizeof(req), &req_len, 0x02, fid, sizeof(fid));

	/* TLV 0x03: Read Transparent offset=0, length=0 (whole file) */
	uint8_t rd[4] = { 0, 0, 0, 0 };
	qmi_tlv_put(req, sizeof(req), &req_len, 0x03, rd, sizeof(rd));

	if (qmi_request(ctx, QMI_SVC_UIM, QMI_UIM_READ_TRANSPARENT,
			req, req_len, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
	    qmi_check_result(rsp, rsp_len, NULL) == 0) {
		uint16_t clen = 0;
		const uint8_t *cv = qmi_tlv_find(rsp, rsp_len, 0x11, &clen);
		if (cv && clen >= 2) {
			uint16_t n = rd_le16(cv);
			if ((size_t)n + 2 <= clen)
				decode_ef_spn(st->spn_name, sizeof(st->spn_name), cv + 2, n);
		}
	}
}

static void parse_nas_serving(struct cellular_state *st, const uint8_t *tlvs, size_t len)
{
	uint16_t l = 0;
	const uint8_t *v = qmi_tlv_find(tlvs, len, 0x01, &l);
	if (v && l >= 5) {
		st->reg_state_raw = v[0];
		snprintf(st->reg_state, sizeof(st->reg_state), "%s", nas_reg_str(v[0]));
		st->cs_attached = (v[1] == 1);
		st->ps_attached = (v[2] == 1);
		uint8_t nrif = v[4];
		int8_t best_rif = 0;
		for (uint8_t i = 0; i < nrif && (size_t)(5 + i) < l; i++) {
			int8_t rif = (int8_t)v[5 + i];
			if (rif > best_rif)
				best_rif = rif;
		}
		snprintf(st->rat, sizeof(st->rat), "%s", nas_rat_str(best_rif));
	}
	/* TLV 0x10: Roaming Indicator (0 = roaming ON, 1 = home/OFF in 3GPP2/QMI NAS) */
	v = qmi_tlv_find(tlvs, len, 0x10, &l);
	if (v && l >= 1)
		st->roaming = (v[0] == 0);

	/* Check TLV 0x21 for 3-digit MNC flag */
	bool mnc_3dig = false;
	v = qmi_tlv_find(tlvs, len, 0x21, &l);
	if (v && l >= 5)
		mnc_3dig = (v[4] != 0);

	/* TLV 0x12: Current PLMN (uint16 mcc, uint16 mnc, uint8 desc_len, char desc[]) */
	v = qmi_tlv_find(tlvs, len, 0x12, &l);
	if (v && l >= 5) {
		uint16_t mcc = rd_le16(v);
		uint16_t mnc = rd_le16(v + 2);
		uint8_t dlen = v[4];
		if (mcc >= 100 && mcc <= 999)
			snprintf(st->mcc, sizeof(st->mcc), "%03u", mcc);
		if (mnc <= 999) {
			if (mnc_3dig || mnc >= 100)
				snprintf(st->mnc, sizeof(st->mnc), "%03u", mnc);
			else
				snprintf(st->mnc, sizeof(st->mnc), "%02u", mnc);
		}
		if (dlen > 0 && (size_t)(5 + dlen) <= l)
			copy_clean_ascii(st->plmn_name, sizeof(st->plmn_name), v + 5, dlen);
	}
}

static void parse_nas_sys_info(struct cellular_state *st, const uint8_t *tlvs, size_t len)
{
	uint16_t l = 0;
	const uint8_t *lte_srv = qmi_tlv_find(tlvs, len, 0x14, &l);
	if (lte_srv && l >= 1 && lte_srv[0] == 2) {
		st->reg_state_raw = 1;
		snprintf(st->reg_state, sizeof(st->reg_state), "registered");
		if (!strcmp(st->rat, "none"))
			snprintf(st->rat, sizeof(st->rat), "lte");
	}
	/* TLV 0x19 (LTE Sys Info), 0x18 (WCDMA), 0x17 (GSM): common 8B + LAC(3) + CID(5) + PLMN(7) = 23B */
	static const uint8_t sys_tlvs[] = { 0x19, 0x18, 0x17 };
	for (size_t idx = 0; idx < sizeof(sys_tlvs); idx++) {
		const uint8_t *si = qmi_tlv_find(tlvs, len, sys_tlvs[idx], &l);
		if (!si || l < 8)
			continue;
		if (si[0]) { /* domain_valid */
			uint8_t dom = si[1];
			if (dom == 1 || dom == 3) st->cs_attached = true;
			if (dom == 2 || dom == 3) st->ps_attached = true;
		}
		if (si[4]) /* roam_valid: 0 = off(home), 1 = on(roaming) */
			st->roaming = (si[5] != 0);
		if (l >= 23 && si[16]) { /* net_info_valid: mcc[3], mnc[3] ASCII digits */
			char mcc_b[4] = { 0 }, mnc_b[4] = { 0 };
			int mi = 0, ni = 0;
			for (int k = 0; k < 3; k++) {
				if (isdigit(si[17 + k])) mcc_b[mi++] = (char)si[17 + k];
				if (isdigit(si[20 + k])) mnc_b[ni++] = (char)si[20 + k];
			}
			if (mi == 3 && !st->mcc[0])
				snprintf(st->mcc, sizeof(st->mcc), "%s", mcc_b);
			if (ni >= 2 && !st->mnc[0])
				snprintf(st->mnc, sizeof(st->mnc), "%s", mnc_b);
		}
		break;
	}
}

static void parse_nas_sig_info(struct cellular_state *st, const uint8_t *tlvs, size_t len)
{
	uint16_t l = 0;
	/* TLV 0x17: 5G NR Signal Info (int16 rsrp, int16 snr, int16 rsrq) */
	const uint8_t *nr = qmi_tlv_find(tlvs, len, 0x17, &l);
	if (nr && l >= 6) {
		int16_t rsrp = (int16_t)rd_le16(nr + 0);
		int16_t snr  = (int16_t)rd_le16(nr + 2);
		int16_t rsrq = (int16_t)rd_le16(nr + 4);
		if (rsrp >= -145 && rsrp <= -40) {
			st->has_rsrp = true;
			st->rsrp_dbm = rsrp;
		}
		if (rsrq >= -30 && rsrq <= 0) {
			st->has_rsrq = true;
			st->rsrq_db = rsrq;
		}
		if (snr >= -200 && snr <= 500) {
			st->has_snr = true;
			st->snr_tenth_db = snr;
		}
	}
	/* TLV 0x14: LTE Signal Info (int8 rssi, int8 rsrq, int16 rsrp, int16 snr) */
	const uint8_t *lte = qmi_tlv_find(tlvs, len, 0x14, &l);
	if (lte && l >= 6) {
		int8_t  rssi = (int8_t)lte[0];
		int8_t  rsrq = (int8_t)lte[1];
		int16_t rsrp = (int16_t)rd_le16(lte + 2);
		int16_t snr  = (int16_t)rd_le16(lte + 4);
		if (rssi >= -120 && rssi <= -25) {
			st->has_rssi = true;
			st->rssi_dbm = rssi;
		}
		if (rsrp >= -145 && rsrp <= -40) {
			st->has_rsrp = true;
			st->rsrp_dbm = rsrp;
		}
		if (rsrq >= -30 && rsrq <= 0) {
			st->has_rsrq = true;
			st->rsrq_db = rsrq;
		}
		if (snr >= -200 && snr <= 500) {
			st->has_snr = true;
			st->snr_tenth_db = snr;
		}
		return;
	}
	/* TLV 0x13: WCDMA Signal Info (int8 rssi, int16 ecio) */
	const uint8_t *wcdma = qmi_tlv_find(tlvs, len, 0x13, &l);
	if (wcdma && l >= 1) {
		int8_t rssi = (int8_t)wcdma[0];
		if (rssi >= -120 && rssi <= -25) {
			st->has_rssi = true;
			st->rssi_dbm = rssi;
		}
		return;
	}
	/* TLV 0x12: GSM Signal Info (int8 rssi) */
	const uint8_t *gsm = qmi_tlv_find(tlvs, len, 0x12, &l);
	if (gsm && l >= 1) {
		int8_t rssi = (int8_t)gsm[0];
		if (rssi >= -120 && rssi <= -25) {
			st->has_rssi = true;
			st->rssi_dbm = rssi;
		}
	}
}

int qmi_register_indications(struct qmi_ctx *ctx)
{
	uint8_t req[64], rsp[128];
	size_t req_len = 0, rsp_len = 0;
	uint32_t prev_mask = ctx->ind_reg_mask;

	/* 1. DMS_SET_EVENT_REPORT (0x0001): TLV 0x13 = report_op_mode (1) */
	if (!(ctx->ind_reg_mask & QMI_IND_MASK_DMS) && qmi_has_service(ctx, QMI_SVC_DMS)) {
		uint8_t one = 1;
		req_len = 0;
		qmi_tlv_put(req, sizeof(req), &req_len, 0x13, &one, 1);
		if (qmi_request(ctx, QMI_SVC_DMS, QMI_DMS_SET_EVENT_REPORT,
				req, req_len, rsp, sizeof(rsp), &rsp_len, 1000) == 0)
			ctx->ind_reg_mask |= QMI_IND_MASK_DMS;
	}

	/* 2. UIM_EVENT_REG (0x002e): bit 0 = card status change */
	if (!(ctx->ind_reg_mask & QMI_IND_MASK_UIM) && qmi_has_service(ctx, QMI_SVC_UIM)) {
		uint8_t mask[4];
		wr_le32(mask, 0x00000001u);
		req_len = 0;
		qmi_tlv_put(req, sizeof(req), &req_len, 0x01, mask, 4);
		if (qmi_request(ctx, QMI_SVC_UIM, QMI_UIM_EVENT_REG,
				req, req_len, rsp, sizeof(rsp), &rsp_len, 1000) == 0)
			ctx->ind_reg_mask |= QMI_IND_MASK_UIM;
	}

	/* 3. NAS_CONFIG_SIG_INFO (0x0050) + NAS_INDICATION_REGISTER (0x0003) */
	if (!(ctx->ind_reg_mask & QMI_IND_MASK_NAS) && qmi_has_service(ctx, QMI_SVC_NAS)) {
		/* TLV 0x13: LTE RSRP thresholds (uint8 len=4, int16[4]) */
		uint8_t lte_th[1 + 8];
		lte_th[0] = 4;
		wr_le16(lte_th + 1, (uint16_t)(int16_t)-122);
		wr_le16(lte_th + 3, (uint16_t)(int16_t)-105);
		wr_le16(lte_th + 5, (uint16_t)(int16_t)-95);
		wr_le16(lte_th + 7, (uint16_t)(int16_t)-85);
		req_len = 0;
		qmi_tlv_put(req, sizeof(req), &req_len, 0x13, lte_th, sizeof(lte_th));
		(void)qmi_request(ctx, QMI_SVC_NAS, QMI_NAS_CONFIG_SIG_INFO,
				  req, req_len, rsp, sizeof(rsp), &rsp_len, 1000);

		/* serving_system(0x13), sys_info(0x19), sig_info(0x1a) */
		uint8_t one = 1;
		req_len = 0;
		qmi_tlv_put(req, sizeof(req), &req_len, 0x13, &one, 1);
		qmi_tlv_put(req, sizeof(req), &req_len, 0x19, &one, 1);
		qmi_tlv_put(req, sizeof(req), &req_len, 0x1a, &one, 1);
		if (qmi_request(ctx, QMI_SVC_NAS, QMI_NAS_INDICATION_REGISTER,
				req, req_len, rsp, sizeof(rsp), &rsp_len, 1000) == 0)
			ctx->ind_reg_mask |= QMI_IND_MASK_NAS;
	}

	/* 4. WDS_SET_EVENT_REPORT (0x0001): TLV 0x15 = dormancy status */
	if (!(ctx->ind_reg_mask & QMI_IND_MASK_WDS) && qmi_has_service(ctx, QMI_SVC_WDS)) {
		uint8_t one = 1;
		req_len = 0;
		qmi_tlv_put(req, sizeof(req), &req_len, 0x15, &one, 1);
		if (qmi_request(ctx, QMI_SVC_WDS, QMI_WDS_SET_EVENT_REPORT,
				req, req_len, rsp, sizeof(rsp), &rsp_len, 1000) == 0)
			ctx->ind_reg_mask |= QMI_IND_MASK_WDS;
	}

	return (int)(ctx->ind_reg_mask & ~prev_mask);
}

static void refresh_servers_snapshot(const struct qmi_ctx *ctx, struct cellular_state *st)
{
	st->servers_count = 0;
	for (int i = 0; i < QMI_MAX_SERVERS; i++) {
		if (!ctx->servers[i].valid)
			continue;
		bool dup = false;
		for (uint32_t j = 0; j < st->servers_count; j++) {
			if (st->servers_list[j] == ctx->servers[i].service) {
				dup = true;
				break;
			}
		}
		if (!dup && st->servers_count < QMI_MAX_SERVERS)
			st->servers_list[st->servers_count++] = ctx->servers[i].service;
	}
}

static void poll_dms_operating_mode(struct qmi_ctx *ctx, struct cellular_state *st)
{
	uint8_t rsp[256];
	size_t rsp_len = 0;
	if (!qmi_has_service(ctx, QMI_SVC_DMS))
		return;
	if (qmi_request(ctx, QMI_SVC_DMS, QMI_DMS_GET_OPERATING_MODE,
			NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
	    qmi_check_result(rsp, rsp_len, NULL) == 0) {
		uint16_t ml = 0;
		const uint8_t *mv = qmi_tlv_find(rsp, rsp_len, 0x01, &ml);
		if (mv && ml >= 1) {
			st->dms_mode_raw = mv[0];
			snprintf(st->radio_mode, sizeof(st->radio_mode), "%s", dms_mode_str(mv[0]));
		}
		uint16_t ol = 0;
		const uint8_t *ov = qmi_tlv_find(rsp, rsp_len, 0x10, &ol);
		if (ov && ol >= 2) {
			st->has_dms_offline_reason = true;
			st->dms_offline_reason = rd_le16(ov);
		} else {
			st->has_dms_offline_reason = false;
			st->dms_offline_reason = 0;
		}
		uint16_t hl = 0;
		const uint8_t *hv = qmi_tlv_find(rsp, rsp_len, 0x11, &hl);
		st->dms_hw_restricted = (hv && hl >= 1 && hv[0] != 0);
	}
}

int qmi_poll_fast(struct qmi_ctx *ctx, struct cellular_state *st)
{
	uint8_t rsp[1024];
	size_t rsp_len = 0;

	st->qrtr_connected = (ctx->fd >= 0);
	refresh_servers_snapshot(ctx, st);

	if (st->dms_mode_raw != 0)
		poll_dms_operating_mode(ctx, st);

	if (qmi_has_service(ctx, QMI_SVC_NAS)) {
		if (qmi_request(ctx, QMI_SVC_NAS, QMI_NAS_GET_SERVING_SYSTEM,
				NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
		    qmi_check_result(rsp, rsp_len, NULL) == 0) {
			parse_nas_serving(st, rsp, rsp_len);
		}
		if (qmi_request(ctx, QMI_SVC_NAS, QMI_NAS_GET_SYS_INFO,
				NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
		    qmi_check_result(rsp, rsp_len, NULL) == 0) {
			parse_nas_sys_info(st, rsp, rsp_len);
		}
		if (qmi_request(ctx, QMI_SVC_NAS, QMI_NAS_GET_SIG_INFO,
				NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
		    qmi_check_result(rsp, rsp_len, NULL) == 0) {
			parse_nas_sig_info(st, rsp, rsp_len);
		}
	}
	cellular_recompute_derived(st);
	return 0;
}

int qmi_poll_all(struct qmi_ctx *ctx, struct cellular_state *st, bool include_static)
{
	uint8_t rsp[1024];
	size_t rsp_len = 0;

	st->qrtr_connected = (ctx->fd >= 0);
	refresh_servers_snapshot(ctx, st);

	/* 1. QMI_DMS: operating mode + optional offline reason/hw-restricted + firmware rev */
	if (qmi_has_service(ctx, QMI_SVC_DMS)) {
		poll_dms_operating_mode(ctx, st);
		if (include_static && !st->modem_rev[0]) {
			if (qmi_request(ctx, QMI_SVC_DMS, QMI_DMS_GET_DEVICE_REV_ID,
					NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
			    qmi_check_result(rsp, rsp_len, NULL) == 0) {
				uint16_t rl = 0;
				const uint8_t *rv = qmi_tlv_find(rsp, rsp_len, 0x01, &rl);
				if (rv && rl > 0)
					copy_clean_ascii(st->modem_rev, sizeof(st->modem_rev), rv, rl);
			}
		}
	}

	/* 2. QMI_UIM: card status + auto-provision GW session if needed + EF_SPN */
	if (qmi_has_service(ctx, QMI_SVC_UIM)) {
		uint8_t aid[32] = { 0 };
		uint8_t aid_len = 0, app_type = 0;
		int slot_idx = -1;
		if (qmi_request(ctx, QMI_SVC_UIM, QMI_UIM_GET_CARD_STATUS,
				NULL, 0, rsp, sizeof(rsp), &rsp_len, 1200) == 0 &&
		    qmi_check_result(rsp, rsp_len, NULL) == 0) {
			uint16_t cl = 0;
			const uint8_t *cv = qmi_tlv_find(rsp, rsp_len, 0x10, &cl);
			if (cv && cl > 0) {
				parse_uim_card_status(ctx, st, cv, cl, aid, &aid_len, &slot_idx, &app_type);
			}
		}
		/* Auto-bind Primary GW Provisioning Session on dual-SIM MPSS if SIM is unlocked & unprovisioned */
		if (st->sim_present && !st->gw_provisioned && !ctx->provision_attempted &&
		    slot_idx >= 0 && aid_len > 0 &&
		    (!strcmp(st->app_state, "detected") || !strcmp(st->app_state, "ready"))) {
			ctx->provision_attempted = true;
			uint8_t preq[64], prsp[128];
			size_t preq_len = 0, prsp_len = 0;
			uint8_t sc[2] = { 0x00, 0x01 }; /* PRI_GW_PROV, activate=1 */
			uint8_t ai[34];
			ai[0] = (uint8_t)(slot_idx + 1); /* 1-based slot ID */
			ai[1] = aid_len;
			memcpy(ai + 2, aid, aid_len);
			qmi_tlv_put(preq, sizeof(preq), &preq_len, 0x01, sc, 2);
			qmi_tlv_put(preq, sizeof(preq), &preq_len, 0x10, ai, (uint16_t)(2 + aid_len));
			if (qmi_request(ctx, QMI_SVC_UIM, QMI_UIM_CHANGE_PROVISIONING_SESSION,
					preq, preq_len, prsp, sizeof(prsp), &prsp_len, 2000) == 0 &&
			    qmi_check_result(prsp, prsp_len, NULL) == 0) {
				st->gw_provisioned = true;
				/* Re-query card status after provisioning */
				if (qmi_request(ctx, QMI_SVC_UIM, QMI_UIM_GET_CARD_STATUS,
						NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
				    qmi_check_result(rsp, rsp_len, NULL) == 0) {
					uint16_t cl = 0;
					const uint8_t *cv = qmi_tlv_find(rsp, rsp_len, 0x10, &cl);
					if (cv && cl > 0)
						parse_uim_card_status(ctx, st, cv, cl, aid, &aid_len, &slot_idx, &app_type);
				}
			}
		}
		if (st->sim_present && (include_static || !st->spn_name[0]))
			uim_read_spn(ctx, st, slot_idx, app_type, aid, aid_len);
	}

	/* 3. QMI_NAS: home network + serving system + sys info + signal info */
	if (qmi_has_service(ctx, QMI_SVC_NAS)) {
		if (st->sim_present && (include_static || !st->home_name[0])) {
			if (qmi_request(ctx, QMI_SVC_NAS, QMI_NAS_GET_HOME_NETWORK,
					NULL, 0, rsp, sizeof(rsp), &rsp_len, 1000) == 0 &&
			    qmi_check_result(rsp, rsp_len, NULL) == 0) {
				uint16_t hl = 0;
				const uint8_t *hv = qmi_tlv_find(rsp, rsp_len, 0x01, &hl);
				if (hv && hl >= 5) {
					uint16_t mcc = rd_le16(hv);
					uint16_t mnc = rd_le16(hv + 2);
					uint8_t nlen = hv[4];
					if (!st->mcc[0] && mcc >= 100 && mcc <= 999)
						snprintf(st->mcc, sizeof(st->mcc), "%03u", mcc);
					if (!st->mnc[0] && mnc <= 999)
						snprintf(st->mnc, sizeof(st->mnc),
							 mnc >= 100 ? "%03u" : "%02u", mnc);
					if (nlen > 0 && (size_t)(5 + nlen) <= hl)
						copy_clean_ascii(st->home_name, sizeof(st->home_name), hv + 5, nlen);
				}
			}
		}
	}

	return qmi_poll_fast(ctx, st);
}

bool qmi_apply_indication(struct qmi_ctx *ctx, struct cellular_state *st,
			  uint32_t service, uint16_t msg_id,
			  const uint8_t *tlvs, size_t tlv_len)
{
	bool changed = false;
	if (service == QMI_SVC_DMS && msg_id == QMI_DMS_EVENT_REPORT_IND) {
		uint16_t ml = 0;
		const uint8_t *mv = qmi_tlv_find(tlvs, tlv_len, 0x14, &ml);
		if (mv && ml >= 1) {
			st->dms_mode_raw = mv[0];
			snprintf(st->radio_mode, sizeof(st->radio_mode), "%s", dms_mode_str(mv[0]));
			if (mv[0] == 0)
				st->dms_offline_reason = 0;
			changed = true;
		}
	} else if (service == QMI_SVC_UIM && msg_id == QMI_UIM_STATUS_CHANGE_IND) {
		uint16_t cl = 0;
		const uint8_t *cv = qmi_tlv_find(tlvs, tlv_len, 0x10, &cl);
		if (cv && cl > 0) {
			parse_uim_card_status(ctx, st, cv, cl, NULL, NULL, NULL, NULL);
			if (!st->sim_present) {
				st->spn_name[0] = '\0';
				st->home_name[0] = '\0';
				st->plmn_name[0] = '\0';
				ctx->provision_attempted = false;
			}
			changed = true;
		}
	} else if (service == QMI_SVC_NAS) {
		if (msg_id == QMI_NAS_SERVING_SYSTEM_IND) {
			parse_nas_serving(st, tlvs, tlv_len);
			changed = true;
		} else if (msg_id == QMI_NAS_SYS_INFO_IND) {
			parse_nas_sys_info(st, tlvs, tlv_len);
			changed = true;
		} else if (msg_id == QMI_NAS_SIG_INFO_IND) {
			parse_nas_sig_info(st, tlvs, tlv_len);
			changed = true;
		}
	} else if (service == QMI_SVC_WDS && msg_id == QMI_WDS_PKT_SRVC_STATUS_IND) {
		uint16_t l = 0;
		const uint8_t *v = qmi_tlv_find(tlvs, tlv_len, 0x01, &l);
		if (v && l >= 1) {
			st->data_connected = (v[0] == 2);
			changed = true;
		}
	}
	if (changed)
		cellular_recompute_derived(st);
	return changed;
}

/* ---- Stage 7B WDS / WDA packet data session test & bring-up ---- */

static void fmt_ipv4_le(char *dst, size_t cap, uint32_t le_ip)
{
	struct in_addr a;
	a.s_addr = htonl(le_ip);
	if (!inet_ntop(AF_INET, &a, dst, (socklen_t)cap))
		dst[0] = '\0';
}

static void fmt_ipv6_be(char *dst, size_t cap, const uint8_t ip6[16])
{
	if (!inet_ntop(AF_INET6, ip6, dst, (socklen_t)cap))
		dst[0] = '\0';
}

static int wds_try_start_family(struct qmi_ctx *ctx, struct cellular_state *st,
				const char *apn, uint8_t ip_family)
{
	uint8_t req[256], rsp[1024];
	size_t req_len = 0, rsp_len = 0;

	/* Set client IP family preference (0x004d) */
	qmi_tlv_put(req, sizeof(req), &req_len, 0x01, &ip_family, 1);
	(void)qmi_request(ctx, QMI_SVC_WDS, QMI_WDS_SET_IP_FAMILY_PREF,
			  req, req_len, rsp, sizeof(rsp), &rsp_len, 1500);

	/* Start Network Interface (0x0020) */
	req_len = 0;
	if (apn && apn[0])
		qmi_tlv_put(req, sizeof(req), &req_len, 0x14, apn, (uint16_t)strlen(apn));
	qmi_tlv_put(req, sizeof(req), &req_len, 0x19, &ip_family, 1);

	int rc = qmi_request(ctx, QMI_SVC_WDS, QMI_WDS_START_NET_IFACE,
			     req, req_len, rsp, sizeof(rsp), &rsp_len, 10000);
	if (rc < 0)
		return rc;

	uint16_t el = 0;
	const uint8_t *ev = qmi_tlv_find(rsp, rsp_len, 0x10, &el);
	if (ev && el >= 4) {
		st->wds_end_reason_type = rd_le16(ev);
		st->wds_end_reason = rd_le16(ev + 2);
	}
	uint16_t qerr = 0;
	if (qmi_check_result(rsp, rsp_len, &qerr) != 0)
		return -(int)(qerr ? qerr : 1);

	uint16_t hl = 0;
	const uint8_t *hv = qmi_tlv_find(rsp, rsp_len, 0x01, &hl);
	if (!hv || hl < 4)
		return -1;
	st->wds_handle = rd_le32(hv);
	st->wds_ip_family = ip_family;
	st->data_connected = true;

	/* Query runtime settings (0x002d): APN, DNS, Gateway, IP, Mask, MTU */
	req_len = 0;
	uint8_t mask[4];
	wr_le32(mask, 0x0000ffffu);
	qmi_tlv_put(req, sizeof(req), &req_len, 0x10, mask, 4);
	if (qmi_request(ctx, QMI_SVC_WDS, QMI_WDS_GET_RUNTIME_SETTINGS,
			req, req_len, rsp, sizeof(rsp), &rsp_len, 2000) == 0 &&
	    qmi_check_result(rsp, rsp_len, NULL) == 0) {
		uint16_t l = 0;
		const uint8_t *v;
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x14, &l)) && l > 0)
			copy_clean_ascii(st->data_apn, sizeof(st->data_apn), v, l);
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x1e, &l)) && l >= 4)
			fmt_ipv4_le(st->data_ip, sizeof(st->data_ip), rd_le32(v));
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x20, &l)) && l >= 4)
			fmt_ipv4_le(st->data_gateway, sizeof(st->data_gateway), rd_le32(v));
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x21, &l)) && l >= 4)
			fmt_ipv4_le(st->data_netmask, sizeof(st->data_netmask), rd_le32(v));
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x15, &l)) && l >= 4)
			fmt_ipv4_le(st->data_dns1, sizeof(st->data_dns1), rd_le32(v));
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x16, &l)) && l >= 4)
			fmt_ipv4_le(st->data_dns2, sizeof(st->data_dns2), rd_le32(v));
		if (!st->data_ip[0] && (v = qmi_tlv_find(rsp, rsp_len, 0x25, &l)) && l >= 16)
			fmt_ipv6_be(st->data_ip, sizeof(st->data_ip), v);
		if (!st->data_gateway[0] && (v = qmi_tlv_find(rsp, rsp_len, 0x26, &l)) && l >= 16)
			fmt_ipv6_be(st->data_gateway, sizeof(st->data_gateway), v);
		if (!st->data_dns1[0] && (v = qmi_tlv_find(rsp, rsp_len, 0x27, &l)) && l >= 16)
			fmt_ipv6_be(st->data_dns1, sizeof(st->data_dns1), v);
		if (!st->data_dns2[0] && (v = qmi_tlv_find(rsp, rsp_len, 0x28, &l)) && l >= 16)
			fmt_ipv6_be(st->data_dns2, sizeof(st->data_dns2), v);
		if ((v = qmi_tlv_find(rsp, rsp_len, 0x29, &l)) && l >= 4)
			st->data_mtu = rd_le32(v);
	}
	return 0;
}

int qmi_wds_stop_session(struct qmi_ctx *ctx, struct cellular_state *st)
{
	if (!st->wds_handle || !qmi_has_service(ctx, QMI_SVC_WDS))
		return 0;
	uint8_t req[16], rsp[128];
	size_t req_len = 0, rsp_len = 0;
	uint8_t h[4];
	wr_le32(h, st->wds_handle);
	qmi_tlv_put(req, sizeof(req), &req_len, 0x01, h, 4);
	int rc = qmi_request(ctx, QMI_SVC_WDS, QMI_WDS_STOP_NET_IFACE,
			     req, req_len, rsp, sizeof(rsp), &rsp_len, 3000);
	st->wds_handle = 0;
	st->data_connected = false;
	return rc;
}

static bool contains_icase(const char *hay, const char *needle)
{
	if (!hay || !needle || !needle[0])
		return false;
	size_t nlen = strlen(needle);
	for (size_t i = 0; hay[i]; i++) {
		size_t j = 0;
		while (j < nlen && hay[i + j] &&
		       tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
			j++;
		if (j == nlen)
			return true;
	}
	return false;
}

const char *qmi_resolve_fallback_apn(const struct cellular_state *st)
{
	const char *plmn_op = lookup_plmn_fallback(st->mcc, st->mnc);
	const char *cands[4] = { st->operator_name, st->spn_name, st->home_name, plmn_op };
	for (int i = 0; i < 4; i++) {
		const char *s = cands[i];
		if (!s || !s[0])
			continue;
		if (contains_icase(s, "bsnl"))
			return "bsnlnet";
		if (contains_icase(s, "jio"))
			return "jionet";
		if (contains_icase(s, "airtel"))
			return "airtelgprs.com";
		if (contains_icase(s, "vodafone") || contains_icase(s, "idea") ||
		    !strcasecmp(s, "vi") || !strncasecmp(s, "vi ", 3))
			return "www";
	}
	return NULL;
}

int qmi_wds_test_session(struct qmi_ctx *ctx, struct cellular_state *st,
			 const char *apn, bool keep_up)
{
	uint8_t rsp[512];
	size_t rsp_len = 0;

	/* Probe WDA data format (non-destructive) */
	if (qmi_has_service(ctx, QMI_SVC_WDA)) {
		if (qmi_request(ctx, QMI_SVC_WDA, QMI_WDA_GET_DATA_FORMAT,
				NULL, 0, rsp, sizeof(rsp), &rsp_len, 1500) == 0 &&
		    qmi_check_result(rsp, rsp_len, NULL) == 0) {
			st->wda_probed = true;
			uint16_t l = 0;
			const uint8_t *v;
			if ((v = qmi_tlv_find(rsp, rsp_len, 0x11, &l)) && l >= 4)
				st->wda_link_prot = rd_le32(v);
			if ((v = qmi_tlv_find(rsp, rsp_len, 0x13, &l)) && l >= 4)
				st->wda_dl_agg_prot = rd_le32(v);
		}
	}

	if (!qmi_has_service(ctx, QMI_SVC_WDS))
		return -ENOENT;

	/* Try IPv4 first; if carrier is IPv6-only (e.g. Jio 464XLAT), retry IPv6 */
	int rc = wds_try_start_family(ctx, st, apn, 4);
	if (rc != 0)
		rc = wds_try_start_family(ctx, st, apn, 6);

	/* If no explicit APN was configured and default attach profile failed, retry carrier APN */
	if (rc != 0 && (!apn || !apn[0])) {
		const char *fb_apn = qmi_resolve_fallback_apn(st);
		if (fb_apn && fb_apn[0]) {
			rc = wds_try_start_family(ctx, st, fb_apn, 4);
			if (rc != 0)
				rc = wds_try_start_family(ctx, st, fb_apn, 6);
			if (rc == 0 && !st->data_apn[0])
				snprintf(st->data_apn, sizeof(st->data_apn), "%s", fb_apn);
		}
	}

	if (rc == 0 && !keep_up) {
		/* Keep the discovered runtime settings in st for JSON output, but release call */
		uint32_t saved_fam = (uint32_t)st->wds_ip_family;
		(void)qmi_wds_stop_session(ctx, st);
		st->wds_ip_family = (int)saved_fam;
	}
	return rc;
}

/* ---- Redacted JSON output ---- */

static void json_escape(char *dst, size_t cap, const char *src)
{
	size_t o = 0;
	if (!cap)
		return;
	for (size_t i = 0; src && src[i] && o + 2 < cap; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c == '"' || c == '\\') {
			if (o + 3 >= cap)
				break;
			dst[o++] = '\\';
			dst[o++] = (char)c;
		} else if (c >= 0x20 && c < 0x7f) {
			dst[o++] = (char)c;
		}
	}
	dst[o] = '\0';
}

int cellular_state_format_json(const struct cellular_state *st, char *buf, size_t cap)
{
	char op_esc[128], rev_esc[192], apn_esc[128], srv_buf[512];
	json_escape(op_esc, sizeof(op_esc), st->operator_name);
	json_escape(rev_esc, sizeof(rev_esc), st->modem_rev);
	json_escape(apn_esc, sizeof(apn_esc), st->data_apn);

	size_t so = 0;
	srv_buf[0] = '\0';
	for (uint32_t i = 0; i < st->servers_count; i++) {
		int w = snprintf(srv_buf + so, sizeof(srv_buf) - so,
				 "%s\"0x%02x\"", i ? ", " : "", st->servers_list[i]);
		if (w < 0 || (size_t)w >= sizeof(srv_buf) - so)
			break;
		so += (size_t)w;
	}

	char rssi_s[16] = "null", rsrp_s[16] = "null", rsrq_s[16] = "null";
	if (st->has_rssi)
		snprintf(rssi_s, sizeof(rssi_s), "%d", st->rssi_dbm);
	if (st->has_rsrp)
		snprintf(rsrp_s, sizeof(rsrp_s), "%d", st->rsrp_dbm);
	if (st->has_rsrq)
		snprintf(rsrq_s, sizeof(rsrq_s), "%d", st->rsrq_db);

	int n = snprintf(buf, cap,
		"{\n"
		"  \"qrtr_connected\": %s,\n"
		"  \"sim_present\": %s,\n"
		"  \"sim_slot\": %d,\n"
		"  \"sim_state\": \"%s\",\n"
		"  \"app_type\": \"%s\",\n"
		"  \"app_state\": \"%s\",\n"
		"  \"gw_provisioned\": %s,\n"
		"  \"radio_mode\": \"%s\",\n"
		"  \"dms_offline_reason\": %u,\n"
		"  \"dms_hw_restricted\": %s,\n"
		"  \"reg_state\": \"%s\",\n"
		"  \"roaming\": %s,\n"
		"  \"cs_attached\": %s,\n"
		"  \"ps_attached\": %s,\n"
		"  \"rat\": \"%s\",\n"
		"  \"mcc\": \"%s\",\n"
		"  \"mnc\": \"%s\",\n"
		"  \"operator_name\": \"%s\",\n"
		"  \"rssi_dbm\": %s,\n"
		"  \"rsrp_dbm\": %s,\n"
		"  \"rsrq_db\": %s,\n"
		"  \"bars_0_to_4\": %d,\n"
		"  \"wda_link_prot\": %u,\n"
		"  \"data_connected\": %s,\n"
		"  \"data_ip_family\": %d,\n"
		"  \"data_apn\": \"%s\",\n"
		"  \"data_ip\": \"%s\",\n"
		"  \"data_gateway\": \"%s\",\n"
		"  \"data_dns1\": \"%s\",\n"
		"  \"data_dns2\": \"%s\",\n"
		"  \"data_mtu\": %u,\n"
		"  \"wds_end_reason_type\": %u,\n"
		"  \"wds_end_reason\": %u,\n"
		"  \"modem_rev\": \"%s\",\n"
		"  \"servers_seen\": [%s],\n"
		"  \"updated_unix\": %lld\n"
		"}\n",
		st->qrtr_connected ? "true" : "false",
		st->sim_present ? "true" : "false",
		st->sim_slot,
		st->sim_state,
		st->app_type,
		st->app_state,
		st->gw_provisioned ? "true" : "false",
		st->radio_mode,
		st->dms_offline_reason,
		st->dms_hw_restricted ? "true" : "false",
		st->reg_state,
		st->roaming ? "true" : "false",
		st->cs_attached ? "true" : "false",
		st->ps_attached ? "true" : "false",
		st->rat,
		st->mcc,
		st->mnc,
		op_esc,
		rssi_s,
		rsrp_s,
		rsrq_s,
		st->bars_0_to_4,
		st->wda_link_prot,
		st->data_connected ? "true" : "false",
		st->wds_ip_family,
		apn_esc,
		st->data_ip,
		st->data_gateway,
		st->data_dns1,
		st->data_dns2,
		st->data_mtu,
		st->wds_end_reason_type,
		st->wds_end_reason,
		rev_esc,
		srv_buf,
		(long long)time(NULL));
	return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int cellular_state_write_json_file(const struct cellular_state *st, const char *path)
{
	char buf[4096];
	if (cellular_state_format_json(st, buf, sizeof(buf)) < 0)
		return -1;
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	size_t l = strlen(buf);
	bool ok = (fwrite(buf, 1, l, f) == l);
	fclose(f);
	if (!ok || rename(tmp, path) < 0) {
		(void)unlink(tmp);
		return -1;
	}
	return 0;
}
