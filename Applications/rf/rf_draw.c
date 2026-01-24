#include "rf_draw.h"

#include "rf_font6x8cp1251.h"
#include "rf_utf8.h"
#include "rf_cp1251.h"

#include <stdio.h>
#include <string.h>

static void write_px(uint8_t **dst, struct rf_color c)
{
	uint8_t *p = *dst;
	*p++ = c.b;
	*p++ = c.g;
	*p++ = c.r;
	*dst = p;
}

struct rf_color rf_color_bg(void) { return (struct rf_color){0x00, 0x00, 0x00}; }
struct rf_color rf_color_panel_bg(void) { return (struct rf_color){0x08, 0x08, 0x08}; }
struct rf_color rf_color_header_bg(void) { return (struct rf_color){0x1C, 0x1C, 0x1C}; }
struct rf_color rf_color_status_bg(void) { return (struct rf_color){0x16, 0x16, 0x16}; }
struct rf_color rf_color_border(void) { return (struct rf_color){0x2E, 0x2E, 0x2E}; }
struct rf_color rf_color_fg(void) { return (struct rf_color){0xEE, 0xEE, 0xEE}; }
struct rf_color rf_color_dim(void) { return (struct rf_color){0x8A, 0x8A, 0x8A}; }
struct rf_color rf_color_accent(void) { return (struct rf_color){0x4A, 0xD1, 0xFF}; }
struct rf_color rf_color_warn(void) { return (struct rf_color){0xFF, 0xD1, 0x4A}; }
struct rf_color rf_color_sel_bg(void) { return (struct rf_color){0xE8, 0xE8, 0xE8}; }
struct rf_color rf_color_sel_fg(void) { return (struct rf_color){0x11, 0x11, 0x11}; }
struct rf_color rf_color_focus_mark(void) { return (struct rf_color){0x20, 0xA0, 0xFF}; }
struct rf_color rf_color_menu_bg(void) { return (struct rf_color){0x1A, 0x3D, 0x7A}; }
struct rf_color rf_color_menu_fg(void) { return (struct rf_color){0xFF, 0xFF, 0xFF}; }

static int rf_draw_text_box(struct rf_fb *fb, uint16_t x, uint16_t y, const char *utf8,
			    struct rf_color fg, struct rf_color bg, int cols)
{
	if (!fb || fb->fd < 0 || !fb->active)
		return -1;
	if (cols <= 0)
		return 0;

	uint16_t w = (uint16_t)(cols * RF_FONT_W);
	uint8_t *payload = rf_fb_begin_box(fb, x, y, w, RF_FONT_H);
	if (!payload)
		return -1;

	/* Fill background + draw glyphs line-by-line for cache locality. */
	const uint8_t *src = (const uint8_t *)(utf8 ? utf8 : "");
	size_t srclen = utf8 ? strlen(utf8) : 0;
	for (int py = 0; py < RF_FONT_H; py++) {
		uint8_t *dst = payload + (size_t)py * (size_t)w * 3;

		/* Write glyphs sequentially. */
		int cx = 0;
		while (cx < cols) {
			uint32_t r = 0;
			size_t rsz = 0;
			if (srclen) {
				rsz = rf_utf8_decode(src, srclen, &r);
				if (rsz == 0)
					break;
				src += rsz;
				srclen -= rsz;
			} else {
				r = ' ';
			}

			uint8_t b = rf_cp1251_from_rune(r);
			uint8_t glyph = rf_font6x8_cp1251_row(b, (uint8_t)py);
			for (int bit = 0; bit < RF_FONT_W; bit++) {
				int on = (glyph & (0x20 >> bit)) != 0;
				write_px(&dst, on ? fg : bg);
			}

			cx++;
			if (!srclen && cx < cols) {
				/* pad with spaces quickly */
				for (; cx < cols; cx++) {
					uint8_t g2 = rf_font6x8_cp1251_row(' ', (uint8_t)py);
					for (int bit = 0; bit < RF_FONT_W; bit++) {
						int on = (g2 & (0x20 >> bit)) != 0;
						write_px(&dst, on ? fg : bg);
					}
				}
				break;
			}
		}

		/* If we broke early due to bad UTF-8, pad remaining with bg. */
		while (cx < cols) {
			for (int bit = 0; bit < RF_FONT_W; bit++)
				write_px(&dst, bg);
			cx++;
		}
	}

	return rf_fb_write_box(fb);
}

static void fill_rect_chunk(struct rf_fb *fb, int x, int y, int w, int h, struct rf_color c)
{
	if (!fb || fb->fd < 0 || !fb->active)
		return;
	if (w <= 0 || h <= 0)
		return;
	if (x < 0 || y < 0)
		return;

	uint8_t *payload = rf_fb_begin_box(fb, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
	if (!payload)
		return;
	for (int py = 0; py < h; py++) {
		uint8_t *dst = payload + (size_t)py * (size_t)w * 3;
		for (int px = 0; px < w; px++)
			write_px(&dst, c);
	}
	(void)rf_fb_write_box(fb);
}

void rf_draw_clear(const struct rf_task *t, struct rf_color c)
{
	if (!t)
		return;

	/* Clear in chunks to avoid large buffers. */
	const int chunk_h = 16;
	int w = (int)t->fb.disp.width;
	int h = (int)t->fb.disp.height;
	for (int y = 0; y < h; y += chunk_h) {
		int ch = h - y;
		if (ch > chunk_h)
			ch = chunk_h;
		fill_rect_chunk((struct rf_fb *)&t->fb, 0, y, w, ch, c);
	}
}

void rf_draw_fill_rect(const struct rf_task *t, int16_t x, int16_t y, int16_t w, int16_t h, struct rf_color c)
{
	if (!t)
		return;
	if (w <= 0 || h <= 0)
		return;

	int x0 = x;
	int y0 = y;
	int x1 = x + w;
	int y1 = y + h;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int)t->fb.disp.width)
		x1 = (int)t->fb.disp.width;
	if (y1 > (int)t->fb.disp.height)
		y1 = (int)t->fb.disp.height;
	int cw = x1 - x0;
	int ch_total = y1 - y0;
	if (cw <= 0 || ch_total <= 0)
		return;

	const int chunk_h = 16;
	for (int oy = 0; oy < ch_total; oy += chunk_h) {
		int ch = ch_total - oy;
		if (ch > chunk_h)
			ch = chunk_h;
		fill_rect_chunk((struct rf_fb *)&t->fb, x0, y0 + oy, cw, ch, c);
	}
}

void rf_draw_text(const struct rf_task *t, int16_t x, int16_t y, const char *utf8, struct rf_color fg,
		  struct rf_color bg, int cols)
{
	if (!t)
		return;
	if (cols <= 0)
		return;

	int fb_w = (int)t->fb.disp.width;
	int max_cols = fb_w / RF_FONT_W;
	if (cols > max_cols)
		cols = max_cols;

	if (x < 0) {
		int skip = (-x) / RF_FONT_W;
		if (skip >= cols)
			return;
		cols -= skip;
		x = 0;
	}
	if (y < 0)
		return;
	if (y + RF_FONT_H > (int)t->fb.disp.height)
		return;

	if (x >= fb_w)
		return;
	int avail_cols = (fb_w - x) / RF_FONT_W;
	if (avail_cols <= 0)
		return;
	if (cols > avail_cols)
		cols = avail_cols;

	(void)rf_draw_text_box((struct rf_fb *)&t->fb, (uint16_t)x, (uint16_t)y, utf8, fg, bg, cols);
}

void rf_draw_present(const struct rf_task *t)
{
	if (!t)
		return;
	if (t->fb.mode == RF_FB_MODE_MEMORY)
		(void)rf_fb_flush((struct rf_fb *)&t->fb, NULL);
}
