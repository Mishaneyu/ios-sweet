/*
 * QMI_SERVREG_NOTIFIER (service 0x42, instance 180 / 0xb401 on MPSS node 0):
 * registers a listener for "msm/modem/wlan_pd" so MPSS spawns the dynamic
 * WLAN user PD (wlanmdsp.mbn over TFTP -> WLFW 0x45) in normal production
 * LTE mode (when oppo_pcba / IMEI NV is active), and ACKs PD state updates.
 *
 * Wire layout verified against stock RMX1931 icnss + service-notifier
 * (qmi_elem_info tables for 0x0020 REGISTER_LISTENER, 0x0022 STATE_UPDATED_IND,
 * and 0x0023 SET_ACK).
 */
#include <stdio.h>
#include <string.h>
#include "link.h"
#include "qmi.h"
#include "servreg.h"

#define SERVREG_NOTIF_REGISTER_LISTENER_REQ	0x0020
#define SERVREG_NOTIF_STATE_UPDATED_IND		0x0022
#define SERVREG_NOTIF_SET_ACK_REQ		0x0023

#define WLAN_PD_SERVICE_NAME			"msm/modem/wlan_pd"
#define MODEM_SERVREG_INSTANCE			180u

static int srv_up;
static uint32_t srv_node;
static uint32_t srv_port;
static uint32_t srv_inst;

static int host_ready;
static int elf_loader_seen;
static int reg_acked_ok;
static int wlfw_up;
static int state_ind_seen;
static unsigned reg_tries;
static uint16_t next_txn;
static uint64_t ready_ms;
static uint64_t next_try_ms;

static void send_register_listener(uint8_t enable)
{
	struct qmi_buf q;
	size_t slen = strlen(WLAN_PD_SERVICE_NAME);

	if (!srv_up)
		return;
	qmi_start(&q, QMI_REQUEST, ++next_txn, SERVREG_NOTIF_REGISTER_LISTENER_REQ);
	qmi_tlv_u8(&q, 0x01, enable);
	qmi_tlv(&q, 0x02, WLAN_PD_SERVICE_NAME, (uint16_t)slen);
	(void)qrtr_send(QRTR_TYPE_DATA, SERVREG_PORT, srv_node, srv_port,
			q.b, (uint32_t)qmi_finish(&q));
	printf("[%6llu] servreg: > REGISTER_LISTENER(enable=%u, \"%s\") -> %u:%x (inst %#x, try %u)\n",
	       (unsigned long long)link_ms(), enable, WLAN_PD_SERVICE_NAME,
	       srv_node, srv_port, srv_inst, reg_tries + 1);
}

static void send_state_ack(const char *svc_name, uint16_t svc_len, uint16_t ind_txn)
{
	struct qmi_buf q;

	if (!srv_up || !svc_len)
		return;
	qmi_start(&q, QMI_REQUEST, ++next_txn, SERVREG_NOTIF_SET_ACK_REQ);
	qmi_tlv(&q, 0x01, svc_name, svc_len);
	qmi_tlv(&q, 0x02, &ind_txn, sizeof(ind_txn));
	(void)qrtr_send(QRTR_TYPE_DATA, SERVREG_PORT, srv_node, srv_port,
			q.b, (uint32_t)qmi_finish(&q));
	printf("[%6llu] servreg: > SET_ACK(\"%.*s\", txn=%u)\n",
	       (unsigned long long)link_ms(), (int)svc_len, svc_name, ind_txn);
}

void servreg_new_server(uint32_t instance, uint32_t node, uint32_t port)
{
	if (node != QRTR_NODE_MODEM)
		return;
	/* QRTR v1 packs version in low byte and QMI instance (180 = 0xb4) in bits 8..15 */
	uint32_t inst_id = (instance >> 8) ? (instance >> 8) : instance;
	if (inst_id != MODEM_SERVREG_INSTANCE && srv_up)
		return;
	srv_node = node;
	srv_port = port;
	srv_inst = instance;
	srv_up = 1;
	printf("[%6llu] servreg: modem notifier at %u:%x (instance %#x)\n",
	       (unsigned long long)link_ms(), node, port, instance);
	if (host_ready && elf_loader_seen && !reg_acked_ok && !wlfw_up)
		next_try_ms = link_ms() + 15;
}

void servreg_del_server(uint32_t node, uint32_t port)
{
	if (srv_up && srv_node == node && srv_port == port)
		srv_up = 0;
}

void servreg_ready(void)
{
	host_ready = 1;
	ready_ms = link_ms();
	/* Fallback if pd-mapper "kernel/elf_loader" was already answered or skipped */
	if (!next_try_ms)
		next_try_ms = ready_ms + 2500;
}

void servreg_on_elf_loader(void)
{
	elf_loader_seen = 1;
	if (!wlfw_up && !reg_acked_ok) {
		reg_tries = 0;
		next_try_ms = link_ms() + 15;
	}
}

void servreg_on_wlfw_up(void)
{
	wlfw_up = 1;
}

void servreg_poll(void)
{
	if (!host_ready || !srv_up || wlfw_up)
		return;
	uint64_t now = link_ms();
	if (!next_try_ms || now < next_try_ms)
		return;

	if (!reg_acked_ok) {
		if (reg_tries >= 8) {
			next_try_ms = 0;
			return;
		}
		send_register_listener(1);
		reg_tries++;
		next_try_ms = now + 600;
		return;
	}

	/* Registered OK, but neither state ind nor WLFW arrived after 5s: re-poke once */
	if (!state_ind_seen && reg_tries < 10) {
		send_register_listener(0);
		send_register_listener(1);
		reg_tries = 10;
		next_try_ms = 0;
	} else {
		next_try_ms = 0;
	}
}

void servreg_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	(void)h;

	if (qmi_parse(d, n, &m))
		return;

	if (m.type == QMI_RESPONSE) {
		uint16_t rl = 0;
		const uint8_t *rv = qmi_get(&m, 0x02, &rl);
		uint16_t res = 0xffff, err = 0xffff;
		if (rv && rl >= 4) {
			memcpy(&res, rv, 2);
			memcpy(&err, rv + 2, 2);
		}
		if (m.id == SERVREG_NOTIF_REGISTER_LISTENER_REQ) {
			uint32_t curr_state = 0;
			int has_state = (qmi_get_u32(&m, 0x10, &curr_state) == 0);
			printf("[%6llu] servreg: < REGISTER_LISTENER resp res=%u err=%#x%s%#x\n",
			       (unsigned long long)link_ms(), res, err,
			       has_state ? " curr_state=" : " ", has_state ? curr_state : 0u);
			if (res == 0) {
				reg_acked_ok = 1;
				/* Give MPSS 5s to TFTP-load wlanmdsp.mbn before any re-poke */
				next_try_ms = link_ms() + 5000;
			}
		} else if (m.id == SERVREG_NOTIF_SET_ACK_REQ && res != 0) {
			printf("[%6llu] servreg: < SET_ACK resp res=%u err=%#x\n",
			       (unsigned long long)link_ms(), res, err);
		}
		return;
	}

	if (m.type == QMI_INDICATION && m.id == SERVREG_NOTIF_STATE_UPDATED_IND) {
		uint32_t curr_state = 0;
		uint16_t slen = 0, tl = 0, ind_txn = 0;
		const uint8_t *sp = qmi_get(&m, 0x02, &slen);
		const uint8_t *tp = qmi_get(&m, 0x03, &tl);
		char sname[80] = { 0 };

		(void)qmi_get_u32(&m, 0x01, &curr_state);
		if (sp && slen > 0) {
			uint16_t cp = slen < sizeof(sname) - 1 ? slen : (uint16_t)(sizeof(sname) - 1);
			memcpy(sname, sp, cp);
			slen = cp;
		}
		if (tp && tl >= 2)
			memcpy(&ind_txn, tp, 2);

		state_ind_seen = 1;
		printf("[%6llu] servreg: < STATE_UPDATED \"%s\" state=%#x ind_txn=%u\n",
		       (unsigned long long)link_ms(), sname, curr_state, ind_txn);
		if (slen > 0)
			send_state_ack(sname, slen, ind_txn);
	}
}
