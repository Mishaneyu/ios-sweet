#pragma once
#include <stdint.h>

/*
 * The framebuffer geometry is detected at runtime (sde.c); until then it is
 * board.h's splash fallback. a8r8g8b8 only: con_init() with any other depth
 * disables drawing.
 */
extern uint32_t con_w, con_h, con_stride;
void con_init(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp);
/* Called after every screen (sde.c: one frame on command mode without autorefresh). */
extern void (*con_flush)(void);

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
void con_scale(int s);
unsigned con_page(const char *t, unsigned len, uint32_t fg);
int con_rows(void);
void con_set_dry(int d);
