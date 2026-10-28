#ifndef QMI_CELLULAR_H
#define QMI_CELLULAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../wifi/sweet_qrtr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QMI Service IDs on SM8150 MPSS (QRTR node 0) */
#define QMI_SVC_CTL	0x00u
#define QMI_SVC_WDS	0x01u
#define QMI_SVC_DMS	0x02u
#define QMI_SVC_NAS	0x03u
#define QMI_SVC_QOS	0x04u
#define QMI_SVC_WMS	0x05u
#define QMI_SVC_AUTH	0x07u
#define QMI_SVC_AT	0x08u
#define QMI_SVC_VOICE	0x09u
#define QMI_SVC_CAT	0x0au
#define QMI_SVC_UIM	0x0bu
#define QMI_SVC_PBM	0x0cu
#define QMI_SVC_LOC	0x10u
#define QMI_SVC_SAR	0x11u
#define QMI_SVC_WDA	0x1au
#define QMI_SVC_PDC	0x24u
#define QMI_SVC_DPM	0x2fu
#define QMI_SVC_IPA	0x31u
#define QMI_SVC_WLFW	0x45u

/* QMI Header flags (7-byte QRTR QMI service header) */
#define QMI_FLG_REQ	0x00u
#define QMI_FLG_RSP	0x02u
#define QMI_FLG_IND	0x04u

/* QMI DMS message IDs (0x0025 inspected in RAM only for imei_present boolean; never stored/logged) */
#define QMI_DMS_SET_EVENT_REPORT		0x0001u
#define QMI_DMS_EVENT_REPORT_IND		0x0001u
#define QMI_DMS_GET_DEVICE_REV_ID		0x0023u
#define QMI_DMS_GET_DEVICE_SERIAL_NUMBERS	0x0025u
#define QMI_DMS_GET_OPERATING_MODE		0x002du
#define QMI_DMS_SET_OPERATING_MODE		0x002eu

/* QMI UIM message IDs (NOTE: ICCID/IMSI/MSISDN EF IDs never read) */
#define QMI_UIM_READ_TRANSPARENT		0x0020u
#define QMI_UIM_EVENT_REG			0x002eu
#define QMI_UIM_GET_CARD_STATUS			0x002fu
#define QMI_UIM_STATUS_CHANGE_IND		0x0032u
#define QMI_UIM_CHANGE_PROVISIONING_SESSION	0x0038u

/* QMI NAS message IDs */
#define QMI_NAS_INDICATION_REGISTER		0x0003u
#define QMI_NAS_GET_SIGNAL_STRENGTH		0x0020u
#define QMI_NAS_INITIATE_NETWORK_REGISTER	0x0022u
#define QMI_NAS_GET_SERVING_SYSTEM		0x0024u
#define QMI_NAS_SERVING_SYSTEM_IND		0x0024u
#define QMI_NAS_GET_HOME_NETWORK		0x0025u
#define QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE	0x0033u
#define QMI_NAS_GET_OPERATOR_NAME		0x0039u
#define QMI_NAS_OPERATOR_NAME_IND		0x003au
#define QMI_NAS_GET_SYS_INFO			0x004du
#define QMI_NAS_SYS_INFO_IND			0x004eu
#define QMI_NAS_GET_SIG_INFO			0x004fu
#define QMI_NAS_CONFIG_SIG_INFO			0x0050u
#define QMI_NAS_SIG_INFO_IND			0x0051u
#define QMI_NAS_NETWORK_REJECT_IND		0x0068u

/* QMI VOICE (0x09) message IDs */
#define QMI_VOICE_INDICATION_REGISTER		0x0003u
#define QMI_VOICE_DIAL_CALL			0x0020u
#define QMI_VOICE_END_CALL			0x0021u
#define QMI_VOICE_ANSWER_CALL			0x0022u
#define QMI_VOICE_BURST_DTMF			0x0028u
#define QMI_VOICE_START_CONT_DTMF		0x0029u
#define QMI_VOICE_STOP_CONT_DTMF		0x002au
#define QMI_VOICE_ALL_CALL_STATUS_IND		0x002eu
#define QMI_VOICE_GET_ALL_CALL_INFO		0x002fu
#define QMI_VOICE_MANAGE_CALLS			0x0031u

/* QMI VOICE call states */
#define QMI_VOICE_CALL_STATE_ORIGINATING	0x01u
#define QMI_VOICE_CALL_STATE_INCOMING		0x02u
#define QMI_VOICE_CALL_STATE_CONVERSATION	0x03u
#define QMI_VOICE_CALL_STATE_CC_IN_PROGRESS	0x04u
#define QMI_VOICE_CALL_STATE_ALERTING		0x05u
#define QMI_VOICE_CALL_STATE_HOLD		0x06u
#define QMI_VOICE_CALL_STATE_WAITING		0x07u
#define QMI_VOICE_CALL_STATE_DISCONNECTING	0x08u
#define QMI_VOICE_CALL_STATE_END		0x09u
#define QMI_VOICE_CALL_STATE_SETUP		0x0au
#define QMI_VOICE_CALL_STATE_PRE_ALERTING	0x0bu

/* Shared ADSP voice call state flag (/tmp/sweet-voice-call.state, mode 0666) */
#define SWEET_VOICE_CALL_STATE_PATH		"/tmp/sweet-voice-call.state"
#define SWEET_VOICE_CALL_STATE_MAGIC		0x53575643u /* 'SWVC' */
#define SWEET_CELLULARD_CTRL_SOCK		"/var/run/sweet-cellulard.sock"

struct sweet_voice_call_shm {
	uint32_t magic;
	uint32_t active;
	uint32_t call_state;
	uint32_t call_mode;
};

/* QMI WDA / DPM / WDS message IDs (Stage 7B packet data foundation) */
#define QMI_WDA_SET_DATA_FORMAT		0x0020u
#define QMI_WDA_GET_DATA_FORMAT		0x0021u
#define QMI_DPM_OPEN_PORT		0x0020u
#define QMI_WDS_SET_EVENT_REPORT	0x0001u
#define QMI_WDS_START_NET_IFACE		0x0020u
#define QMI_WDS_STOP_NET_IFACE		0x0021u
#define QMI_WDS_GET_PKT_SRVC_STATUS	0x0022u
#define QMI_WDS_PKT_SRVC_STATUS_IND	0x0022u
#define QMI_WDS_GET_RUNTIME_SETTINGS	0x002du
#define QMI_WDS_SET_IP_FAMILY_PREF	0x004du

#define QMI_MAX_SERVERS	64

struct qmi_srv_record {
	bool     valid;
	uint32_t service;
	uint32_t instance;
	uint32_t node;
	uint32_t port;
};

/* Redacted cellular state (NEVER holds IMEI, IMSI, ICCID, or MSISDN) */
struct cellular_state {
	bool     qrtr_connected;
	int      sim_slot;		/* 1 = Slot 1, 2 = Slot 2, 0 = none */
	bool     sim_present;
	char     sim_state[32];		/* "absent", "present", "pin_locked", "puk_locked", "error" */
	char     app_type[16];		/* "usim", "sim", "none" */
	char     app_state[32];		/* "ready", "detected", "pin1_req", "puk1_req", "none" */
	int      pin1_retries;
	int      puk1_retries;
	bool     gw_provisioned;

	bool     imei_present;		/* RAM-only validation of DMS 0x0025 TLV 0x11; never stores digits */
	uint8_t  dms_mode_raw;
	char     radio_mode[32];	/* "online", "low_power", "offline", ... */
	bool     has_dms_offline_reason;
	uint16_t dms_offline_reason;	/* QMI_DMS_GET_OPERATING_MODE TLV 0x10 bitmask */
	bool     dms_hw_restricted;	/* QMI_DMS_GET_OPERATING_MODE TLV 0x11 */
	char     modem_rev[96];		/* firmware build ID string only (no serials) */

	uint8_t  reg_state_raw;		/* 0=not_reg, 1=registered, 2=searching, 3=denied, 4=unknown */
	char     reg_state[32];
	uint16_t last_reject_cause;	/* QMI_NAS_NETWORK_REJECT_IND (0x0068) TLV 0x03 */
	uint8_t  last_reject_domain;	/* QMI_NAS_NETWORK_REJECT_IND (0x0068) TLV 0x02 */
	bool     roaming;
	bool     cs_attached;
	bool     ps_attached;
	char     rat[16];		/* "lte", "5g_nr", "umts", "gsm", "none" */
	char     mcc[8];		/* e.g. "404", "310" */
	char     mnc[8];		/* e.g. "45", "260" */
	char     spn_name[64];		/* EF_SPN or NAS SPN */
	char     home_name[64];		/* NAS Home Network name */
	char     plmn_name[64];		/* Serving PLMN description */
	char     operator_name[64];	/* Best resolved display carrier name */

	bool     has_rssi;
	int      rssi_dbm;
	bool     has_rsrp;
	int      rsrp_dbm;
	bool     has_rsrq;
	int      rsrq_db;
	bool     has_snr;
	int      snr_tenth_db;		/* 0.1 dB units */
	int      bars_0_to_4;		/* 0..4 signal bars */

	/* Stage 7B WDS / WDA packet data probe / session state */
	bool     wda_probed;
	uint32_t wda_link_prot;		/* 1=802.3, 2=raw-ip */
	uint32_t wda_dl_agg_prot;
	bool     data_connected;
	uint32_t wds_handle;
	int      wds_ip_family;		/* 4 or 6 */
	uint16_t wds_end_reason_type;
	uint16_t wds_end_reason;
	char     data_apn[64];
	char     data_ip[64];
	char     data_netmask[32];
	char     data_gateway[64];
	char     data_dns1[64];
	char     data_dns2[64];
	uint32_t data_mtu;

	/* Redacted QMI_SVC_VOICE (0x09) summary (NO phone numbers ever stored here) */
	bool     voice_svc_ready;
	uint32_t active_calls;
	char     last_call_state[24];
	uint16_t last_call_end_reason;

	uint32_t servers_count;
	uint32_t servers_list[QMI_MAX_SERVERS];
};

#define QMI_MAX_VOICE_CALLS	8

/* RAM-only call entry for CallKit UI; wiped on disconnect, never logged or serialized */
struct qmi_voice_call {
	bool     present;
	uint8_t  call_id;
	uint8_t  state;
	uint8_t  call_type;
	uint8_t  direction;	/* 1=MO, 2=MT */
	uint8_t  mode;		/* 2=GSM, 3=UMTS, 4=LTE */
	uint8_t  is_mpty;
	uint8_t  als;
	uint8_t  num_pi;
	uint16_t end_reason;
	char     number[40];
};

struct qmi_ctx;
#define qmi_cellular qmi_ctx

typedef void (*qmi_ind_fn)(struct qmi_ctx *ctx, uint32_t service, uint16_t msg_id,
			   const uint8_t *tlvs, size_t tlv_len, void *user);
typedef void (*on_voice_calls_changed_fn)(struct qmi_ctx *ctx,
					  const struct qmi_voice_call *calls,
					  size_t count, void *user);

#define QMI_IND_MASK_DMS	(1u << 0)
#define QMI_IND_MASK_UIM	(1u << 1)
#define QMI_IND_MASK_NAS	(1u << 2)
#define QMI_IND_MASK_WDS	(1u << 3)
#define QMI_IND_MASK_VOICE	(1u << 4)

struct qmi_ctx {
	int                  fd;
	uint16_t             next_txn;
	struct qmi_srv_record servers[QMI_MAX_SERVERS];
	size_t               rx_len;
	uint8_t              rx_buf[sizeof(struct sweet_qrtr_msg) + SWEET_QRTR_MAX_DATA];
	qmi_ind_fn           ind_cb;
	void                *ind_user;
	on_voice_calls_changed_fn on_voice_calls_changed;
	void                *voice_user;
	struct qmi_voice_call voice_calls[QMI_MAX_VOICE_CALLS];
	size_t               voice_call_count;
	bool                 provision_attempted;
	bool                 imei_checked;
	bool                 nas_pref_configured;
	bool                 nas_auto_reg_attempted;
	bool                 voice_initial_queried;
	uint32_t             ind_reg_mask;
};

/* Connection & framing */
int  qmi_open(struct qmi_ctx *ctx, const char *sock_path);
void qmi_close(struct qmi_ctx *ctx);
int  qmi_sync_servers(struct qmi_ctx *ctx, unsigned wait_ms);
int  qmi_drain_rx(struct qmi_ctx *ctx, unsigned wait_ms);
bool qmi_has_service(const struct qmi_ctx *ctx, uint32_t service);

/* TLV helpers */
const uint8_t *qmi_tlv_find(const uint8_t *buf, size_t len, uint8_t type, uint16_t *out_len);
int            qmi_check_result(const uint8_t *tlvs, size_t len, uint16_t *qmi_err);
int            qmi_tlv_put(uint8_t *buf, size_t cap, size_t *off, uint8_t type,
			   const void *val, uint16_t vlen);

/* Synchronous QMI request/response (dispatches interleaved indications to ind_cb) */
int qmi_request(struct qmi_ctx *ctx, uint32_t service, uint16_t msg_id,
		const uint8_t *req_tlvs, size_t req_len,
		uint8_t *rsp_tlvs, size_t rsp_cap, size_t *rsp_len,
		unsigned timeout_ms);

/* High-level state & operations */
void cellular_state_init(struct cellular_state *st);
void cellular_recompute_derived(struct cellular_state *st);
int  qmi_dms_set_operating_mode(struct qmi_ctx *ctx, uint8_t mode);
int  qmi_dms_set_operating_mode_ex(struct qmi_ctx *ctx, uint8_t mode, uint16_t *out_qmi_err);
int  qmi_register_indications(struct qmi_ctx *ctx);
int  qmi_poll_all(struct qmi_ctx *ctx, struct cellular_state *st, bool include_static);
int  qmi_poll_fast(struct qmi_ctx *ctx, struct cellular_state *st);
bool qmi_apply_indication(struct qmi_ctx *ctx, struct cellular_state *st,
			  uint32_t service, uint16_t msg_id,
			  const uint8_t *tlvs, size_t tlv_len);

/* Stage 7B WDS / WDA packet data session bring-up / test */
const char *qmi_resolve_fallback_apn(const struct cellular_state *st);
int  qmi_wds_test_session(struct qmi_ctx *ctx, struct cellular_state *st,
			  const char *apn, bool keep_up);
int  qmi_wds_stop_session(struct qmi_ctx *ctx, struct cellular_state *st);

/* QMI_SVC_VOICE (0x09) call control & state */
const char *qmi_voice_state_str(uint8_t state);
int  qmi_voice_query_calls(struct qmi_cellular *qc, struct cellular_state *st);
int  qmi_voice_dial(struct qmi_cellular *qc, const char *number, bool emergency, uint8_t *out_call_id);
int  qmi_voice_answer(struct qmi_cellular *qc, uint8_t call_id);
int  qmi_voice_end(struct qmi_cellular *qc, uint8_t call_id);
int  qmi_voice_manage(struct qmi_cellular *qc, uint8_t sups_type, uint8_t call_id);
int  qmi_voice_dtmf(struct qmi_cellular *qc, uint8_t call_id, char digit, bool start);
void sweet_voice_write_shm(uint32_t active, uint32_t call_state, uint32_t call_mode);

/* Redacted JSON serializer */
int  cellular_state_format_json(const struct cellular_state *st, char *buf, size_t cap);
int  cellular_state_write_json_file(const struct cellular_state *st, const char *path);

#ifdef __cplusplus
}
#endif

#endif
