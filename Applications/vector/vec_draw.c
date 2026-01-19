#include "vec_draw.h"

#include <string.h>

#include "../util/termd_font8x8.h"

static void write_px(uint8_t **dst, struct vec_color c)
{
	uint8_t *p = *dst;
	*p++ = c.b;
	*p++ = c.g;
	*p++ = c.r;
	*dst = p;
}

int vec_draw_text_row(struct vec_fb *fb, int row, const char *s, struct vec_color fg,
		      struct vec_color bg, int cursor_col, int cursor_on)
{
	if (!fb || fb->fd < 0)
		return -1;
	if (row < 0)
		return -1;

	int width = fb->disp.width;
	int cols = width / VEC_FONT_W;
	if (cols <= 0)
		return -1;

	uint16_t w = (uint16_t)(cols * VEC_FONT_W);
	uint16_t y = (uint16_t)(row * VEC_FONT_H);

	uint8_t *payload = vec_fb_begin_box(fb, 0, y, w, VEC_FONT_H);
	if (!payload)
		return -1;

	size_t slen = s ? strlen(s) : 0;
	for (int py = 0; py < VEC_FONT_H; py++) {
		uint8_t *dst = payload + (size_t)py * (size_t)w * 3;
		for (int cx = 0; cx < cols; cx++) {
			unsigned char ch = ' ';
			if (slen && (size_t)cx < slen)
				ch = (unsigned char)s[cx];

			struct vec_color fgx = fg;
			struct vec_color bgx = bg;
			if (cursor_on && cx == cursor_col) {
				fgx = bg;
				bgx = fg;
			}

			unsigned char glyph = termd_font8x8[ch * 8 + py];
			for (int bit = 0; bit < VEC_FONT_W; bit++) {
				int on = (glyph & (0x80 >> bit)) != 0;
				write_px(&dst, on ? fgx : bgx);
			}
		}
	}

	return vec_fb_write_box(fb);
}

