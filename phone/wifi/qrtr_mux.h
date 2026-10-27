#ifndef QRTR_MUX_H
#define QRTR_MUX_H

#include <stdint.h>

#define QRTR_MUX_PORT_BASE	0x40u
#define QRTR_MUX_MAX_CLIENTS	8u
#define QRTR_MUX_MAX_SERVERS	64u

int  qrtr_mux_open(void);
void qrtr_mux_poll(void);
void qrtr_mux_wait(unsigned ms);
void qrtr_mux_on_ctrl(uint32_t cmd, uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
int  qrtr_mux_deliver_data(uint32_t dst_port, uint32_t src_node, uint32_t src_port,
			   const uint8_t *data, uint32_t len);

#endif
