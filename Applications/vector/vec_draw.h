#ifndef VEC_DRAW_H
#define VEC_DRAW_H

#include "vec_fb.h"

/* 8x8 bitmap font rendering (BGR888). */
#define VEC_FONT_W 8
#define VEC_FONT_H 8

int vec_draw_text_row(struct vec_fb *fb, int row, const char *s, struct vec_color fg,
		      struct vec_color bg, int cursor_col, int cursor_on);

#endif

