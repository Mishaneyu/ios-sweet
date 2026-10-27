#ifndef SWEET_QRTR_H
#define SWEET_QRTR_H

#include <stdint.h>

#define SWEET_QRTR_SOCK		"/var/run/sweet-qrtr.sock"
#define SWEET_QRTR_MAX_DATA	16384u

/* Standard QRTR packet / control types (matches link.h & Linux qrtr.h) */
#define SWEET_QRTR_TYPE_DATA		1u
#define SWEET_QRTR_TYPE_HELLO		2u
#define SWEET_QRTR_TYPE_BYE		3u
#define SWEET_QRTR_TYPE_NEW_SERVER	4u
#define SWEET_QRTR_TYPE_DEL_SERVER	5u
#define SWEET_QRTR_TYPE_DEL_CLIENT	6u

/* Wire message over /var/run/sweet-qrtr.sock (24-byte header + len bytes) */
struct sweet_qrtr_msg {
	uint32_t type;		/* SWEET_QRTR_TYPE_DATA / NEW_SERVER / DEL_SERVER */
	uint32_t node;		/* remote QRTR node (0 = MPSS) */
	uint32_t port;		/* remote QRTR port */
	uint32_t service;	/* QMI service id (for NEW_SERVER / DEL_SERVER) */
	uint32_t instance;	/* QRTR instance word: (version & 0xff) | (qmi_inst << 8) */
	uint32_t len;		/* payload length in bytes */
	uint8_t  data[];
};

#endif
