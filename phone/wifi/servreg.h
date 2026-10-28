#ifndef SERVREG_H
#define SERVREG_H
#include "link.h"

#define SERVREG_SERVICE	0x42
#define SERVREG_PORT	0x21

void servreg_new_server(uint32_t instance, uint32_t node, uint32_t port);
void servreg_del_server(uint32_t node, uint32_t port);
void servreg_ready(void);
void servreg_on_elf_loader(void);
void servreg_on_wlfw_up(void);
void servreg_poll(void);
void servreg_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);

#endif
