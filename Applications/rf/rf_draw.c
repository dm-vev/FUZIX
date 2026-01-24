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

static int draw_text_box(struct rf_fb *fb, uint16_t x, uint16_t y, const char *utf8,
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
	for (int py = 0; py < RF_FONT_H; py++) {
		uint8_t *dst = payload + (size_t)py * (size_t)w * 3;

		/* Write glyphs sequentially. */
		const char *p = utf8 ? utf8 : "";
		int cx = 0;
		while (cx < cols) {
			uint32_t r = 0;
			size_t rsz = 0;
			if (*p) {
				rsz = rf_utf8_decode((const uint8_t *)p, strlen(p), &r);
				if (rsz == 0)
					break;
				p += rsz;
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
			if (!*p && cx < cols) {
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

void rf_draw_header(const struct rf_task *t)
{
	if (!t)
		return;

	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();
	struct rf_color header = rf_color_header_bg();

	/* Menu bar row. */
	fill_rect_chunk((struct rf_fb *)&t->fb, 0, 0, (int)t->fb.disp.width, RF_FONT_H, header);
	draw_text_box((struct rf_fb *)&t->fb, 2, 0, "View RF Capture Decode Display Advanced Help", fg, header, t->cols);

	/* Toolbar row. */
	fill_rect_chunk((struct rf_fb *)&t->fb, 0, RF_FONT_H, (int)t->fb.disp.width, RF_FONT_H, header);
	draw_text_box((struct rf_fb *)&t->fb, 2, RF_FONT_H,
		      "2.4GHz RF Analyzer  nRF24 scan+spectrum+waterfall+sniffer", dim, header, t->cols);
}

void rf_draw_status(const struct rf_task *t)
{
	if (!t)
		return;

	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();
	struct rf_color bg = rf_color_status_bg();

	int y0 = (t->rows - RF_STATUS_ROWS) * RF_FONT_H;
	fill_rect_chunk((struct rf_fb *)&t->fb, 0, y0, (int)t->fb.disp.width, RF_FONT_H, bg);
	fill_rect_chunk((struct rf_fb *)&t->fb, 0, y0 + RF_FONT_H, (int)t->fb.disp.width, RF_FONT_H, bg);

	char line1[96];
	snprintf(line1, sizeof(line1), "mode: (stub)  tick:%lu", (unsigned long)t->now_tick);
	draw_text_box((struct rf_fb *)&t->fb, 2, (uint16_t)y0, line1, fg, bg, t->cols);
	draw_text_box((struct rf_fb *)&t->fb, 2, (uint16_t)(y0 + RF_FONT_H),
		      "keys: m menu  h help  q quit", dim, bg, t->cols);
}

void rf_draw_present(const struct rf_task *t)
{
	if (!t)
		return;
	if (t->fb.mode == RF_FB_MODE_MEMORY)
		(void)rf_fb_flush((struct rf_fb *)&t->fb, NULL);
}

