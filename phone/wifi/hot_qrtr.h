#ifndef HOT_QRTR_H
#define HOT_QRTR_H

#include <stdint.h>

#define HOT_QRTR_SOCK		"/var/run/hot-qrtr.sock"
#define HOT_QRTR_MAX_DATA	16384u

/* Standard QRTR packet / control types (matches link.h & Linux qrtr.h) */
#define HOT_QRTR_TYPE_DATA		1u
#define HOT_QRTR_TYPE_HELLO		2u
#define HOT_QRTR_TYPE_BYE		3u
#define HOT_QRTR_TYPE_NEW_SERVER	4u
#define HOT_QRTR_TYPE_DEL_SERVER	5u
#define HOT_QRTR_TYPE_DEL_CLIENT	6u

/* Wire message over /var/run/hot-qrtr.sock (24-byte header + len bytes) */
struct hot_qrtr_msg {
	uint32_t type;		/* HOT_QRTR_TYPE_DATA / NEW_SERVER / DEL_SERVER */
	uint32_t node;		/* remote QRTR node (0 = MPSS) */
	uint32_t port;		/* remote QRTR port */
	uint32_t service;	/* QMI service id (for NEW_SERVER / DEL_SERVER) */
	uint32_t instance;	/* QRTR instance word: (version & 0xff) | (qmi_inst << 8) */
	uint32_t len;		/* payload length in bytes */
	uint8_t  data[];
};

#endif
