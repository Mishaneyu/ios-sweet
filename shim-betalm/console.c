/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "board.h"
#include "font.h"

static int SCALE = 3;
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_FALLBACK;
uint32_t con_w = PANEL_W, con_h = PANEL_H, con_stride = PANEL_W * 4;
void (*con_flush)(void);
static uint32_t con_bg;
static int row, con_off, con_dry;
void con_set_dry(int d) { con_dry = d; }

void con_init(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp)
{
	con_fb = (volatile uint32_t *)base;
	con_w = w;
	con_h = h;
	con_stride = stride;
	con_off = bpp != 4;
}

static void flush(void)
{
	if (con_flush)
		con_flush();
}

void con_clear(uint32_t bg)
{
	con_bg = bg;
	row = 0;
	if (con_off || con_dry)
		return;
	for (uint32_t y = 0; y < con_h; y++) {
		volatile uint32_t *line = con_fb + y * (con_stride / 4);
		for (uint32_t x = 0; x < con_w; x++)
			line[x] = bg;
	}
	flush();
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (con_dry || con_off || x0 + CH_W > (int)con_w || y0 + CH_H > (int)con_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (con_stride / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
	flush();
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
	flush();
}

/* Small-font page of raw text (the previous kernel's log): returns bytes shown. */
void con_scale(int s) { SCALE = s; }
unsigned con_page(const char *t, unsigned len, uint32_t fg)
{
	int cols = ((int)con_w - 2 * MARGIN) / CH_W, rows = ((int)con_h - 2 * MARGIN) / CH_H;
	int col = 0;
	unsigned i = 0;
	for (; i < len && row < rows; i++) {
		char c = t[i];
		if (c == '\n' || col >= cols) {
			row++;
			col = 0;
			if (c == '\n')
				continue;
			if (row >= rows)
				break;
		}
		if (c == '\r' || c == '\t')
			c = ' ';
		put_char(col++, c, fg);
	}
	if (!con_dry)
		flush();
	return i;
}
int con_rows(void) { return ((int)con_h - 2 * MARGIN) / CH_H; }
