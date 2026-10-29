/* Render the console layout on the host: ./out/host_test && view out/host.ppm */
#include <stdio.h>
#include "console.h"
#include "board.h"

static uint32_t host_fb[PANEL_W * PANEL_H];

int main(void)
{
	con_init((uintptr_t)host_fb, PANEL_W, PANEL_H, PANEL_W * 4, 4);
	con_clear(RED);
	con_str("SHIM", BOARD_NAME);
	con_hex2("FB", "base=", 0x9c000000, "sspp=", 4);
	con_hex2("FB", "w=", PANEL_W, "h=", PANEL_H);
	con_hex2("SDE", "cmd=", 1, "autoref=", 1);
	con_clear(BLUE);
	con_str("PANIC", "sync exception");
	con_hex("ESR", 0x96000010);
	FILE *f = fopen("out/host.ppm", "wb");
	fprintf(f, "P6 %d %d 255\n", PANEL_W, PANEL_H);
	for (int i = 0; i < PANEL_W * PANEL_H; i++) {
		unsigned char px[3] = { host_fb[i] >> 16, host_fb[i] >> 8, host_fb[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
	return 0;
}
