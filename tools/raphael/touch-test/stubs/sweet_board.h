/* Host stub of bsd/dev/sweet_board.h: the harness sets the board compatible. */
#ifndef _SWEET_BOARD_H_
#define _SWEET_BOARD_H_
#include <stdbool.h>
#include <string.h>
extern const char *test_board;  /* "xiaomi,raphael", "xiaomi,beryllium" or "xiaomi,sweet" */
static inline bool sweet_board_is(const char *want) { return test_board && strcmp(test_board, want) == 0; }
static inline bool sweet_board_beryllium(void) { return sweet_board_is("xiaomi,beryllium"); }
static inline bool sweet_board_raphael(void) { return sweet_board_is("xiaomi,raphael"); }
#endif
