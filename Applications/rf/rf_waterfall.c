#include "rf_waterfall.h"

#include "rf.h"
#include "rf_fb.h"
#include "rf_task.h"

#include <stdlib.h>
#include <string.h>

int rf_waterfall_plot_rect(const struct rf_task *t, struct rf_layout l, struct rf_rect *plot, int16_t *header_y)
{
	if (!t || !plot || !header_y)
		return 0;

	struct rf_rect inner = rf_rect_inset(l.waterfall, 2, 2);
	*header_y = (int16_t)(inner.y + RF_FONT_H + 1);
	*plot = (struct rf_rect){
		.x = inner.x,
		.y = (int16_t)(*header_y + RF_FONT_H + 1),
		.w = inner.w,
		.h = (int16_t)(inner.h - 2 * RF_FONT_H - 3),
	};
	if (plot->w <= 0 || plot->h <= 0)
		return 0;
	return 1;
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, uint8_t t)
{
	return (uint8_t)(((uint16_t)a * (uint16_t)(255 - t) + (uint16_t)b * (uint16_t)t) / 255);
}

static struct rf_color lerp_rgb(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t t)
{
	return (struct rf_color){
		.r = lerp_u8(r0, r1, t),
		.g = lerp_u8(g0, g1, t),
		.b = lerp_u8(b0, b1, t),
	};
}

void rf_waterfall_rebuild_palette(struct rf_task *t)
{
	if (!t)
		return;

	switch (t->wf_palette) {
	case RF_WF_PAL_FIRE:
		for (int i = 0; i < 256; i++) {
			uint8_t v = (uint8_t)i;
			uint8_t r = v;
			uint8_t g = 0;
			uint8_t b = 0;
			if (v > 64)
				g = rf_clamp_u8_int((int)(v - 64) * 2);
			if (v > 160)
				b = rf_clamp_u8_int((int)(v - 160) * 2);
			t->wf_palette888[i] = (struct rf_color){.r = r, .g = g, .b = b};
		}
		break;
	case RF_WF_PAL_GRAY:
		for (int i = 0; i < 256; i++) {
			uint8_t v = (uint8_t)i;
			t->wf_palette888[i] = (struct rf_color){.r = v, .g = v, .b = v};
		}
		break;
	case RF_WF_PAL_CUBIC:
		for (int i = 0; i < 256; i++) {
			uint8_t v = (uint8_t)i;
			struct rf_color c;
			if (v < 64) {
				c = lerp_rgb(0x00, 0x00, 0x06, 0x00, 0x00, 0x60, (uint8_t)(v * 4));
			} else if (v < 128) {
				c = lerp_rgb(0x00, 0x00, 0x60, 0x00, 0xA0, 0xFF, (uint8_t)((v - 64) * 4));
			} else if (v < 176) {
				uint8_t tw = (uint8_t)(((uint16_t)(v - 128) * 255) / 48);
				c = lerp_rgb(0x00, 0xA0, 0xFF, 0x00, 0xFF, 0x30, tw);
			} else if (v < 208) {
				uint8_t tw = (uint8_t)(((uint16_t)(v - 176) * 255) / 32);
				c = lerp_rgb(0x00, 0xFF, 0x30, 0xFF, 0xFF, 0x00, tw);
			} else if (v < 232) {
				uint8_t tw = (uint8_t)(((uint16_t)(v - 208) * 255) / 24);
				c = lerp_rgb(0xFF, 0xFF, 0x00, 0xFF, 0x20, 0x00, tw);
			} else {
				uint8_t tw = (uint8_t)(((uint16_t)(v - 232) * 255) / 23);
				c = lerp_rgb(0xFF, 0x20, 0x00, 0xFF, 0xFF, 0xFF, tw);
			}
			t->wf_palette888[i] = c;
		}
		break;
	case RF_WF_PAL_CYAN:
	default:
		for (int i = 0; i < 256; i++) {
			uint8_t v = (uint8_t)i;
			uint8_t r = 0;
			uint8_t g = (uint8_t)((int)v * 7 / 8);
			uint8_t b = v;
			if (v > 200)
				r = rf_clamp_u8_int((int)(v - 200) * 2);
			t->wf_palette888[i] = (struct rf_color){.r = r, .g = g, .b = b};
		}
		break;
	}
}

int rf_waterfall_ensure_alloc(struct rf_task *t)
{
	if (!t)
		return 0;

	struct rf_layout l = rf_compute_layout(t);
	struct rf_rect plot;
	int16_t header_y;
	if (!rf_waterfall_plot_rect(t, l, &plot, &header_y))
		return 0;

	int plot_w = (int)plot.w;
	int plot_h = (int)plot.h;
	if (plot_w <= 0 || plot_h <= 0)
		return 0;

	size_t need = (size_t)plot_w * (size_t)plot_h;
	if (t->wf_w != plot_w || t->wf_h != plot_h || !t->wf_buf || t->wf_cap < need) {
		uint8_t *nb = (uint8_t *)malloc(need);
		if (!nb)
			return 0;
		free(t->wf_buf);
		t->wf_buf = nb;
		t->wf_cap = need;
		t->wf_w = plot_w;
		t->wf_h = plot_h;
		t->wf_head = 0;
		memset(t->wf_buf, 0, need);
	}

	rf_waterfall_rebuild_palette(t);
	return 1;
}

void rf_waterfall_push_row(struct rf_task *t)
{
	if (!t || !t->wf_buf || t->wf_w <= 0 || t->wf_h <= 0)
		return;

	int row = t->wf_head;
	int base = row * t->wf_w;
	if (base < 0 || (size_t)(base + t->wf_w) > t->wf_cap)
		return;

	for (int x = 0; x < t->wf_w; x++) {
		int lo = x * RF_NUM_CHANNELS / t->wf_w;
		int hi = (x + 1) * RF_NUM_CHANNELS / t->wf_w - 1;
		if (lo < 0)
			lo = 0;
		if (hi < lo)
			hi = lo;
		if (hi >= RF_NUM_CHANNELS)
			hi = RF_NUM_CHANNELS - 1;

		uint8_t mx = 0;
		for (int ch = lo; ch <= hi; ch++) {
			if (t->energy_avg[ch] > mx)
				mx = t->energy_avg[ch];
		}
		t->wf_buf[base + x] = mx;
	}

	t->wf_head++;
	if (t->wf_head >= t->wf_h)
		t->wf_head = 0;
}

static void write_px(uint8_t **dst, struct rf_color c)
{
	uint8_t *p = *dst;
	*p++ = c.b;
	*p++ = c.g;
	*p++ = c.r;
	*dst = p;
}

void rf_waterfall_blit(const struct rf_task *t, struct rf_rect plot)
{
	if (!t || !t->wf_buf || t->wf_w <= 0 || t->wf_h <= 0)
		return;
	if (plot.w <= 0 || plot.h <= 0)
		return;

	int fb_w = (int)t->fb.disp.width;
	int fb_h = (int)t->fb.disp.height;
	int px0 = plot.x;
	int py0 = plot.y;
	int pw = plot.w;
	int ph = plot.h;
	if (px0 < 0 || py0 < 0 || px0 + pw > fb_w || py0 + ph > fb_h)
		return;
	if (pw != t->wf_w || ph != t->wf_h)
		return;

	for (int y = 0; y < ph; y++) {
		int row = t->wf_head - 1 - y;
		while (row < 0)
			row += t->wf_h;
		if (row >= t->wf_h)
			row %= t->wf_h;

		const uint8_t *src = t->wf_buf + (size_t)row * (size_t)t->wf_w;
		uint8_t *payload = rf_fb_begin_box((struct rf_fb *)&t->fb, (uint16_t)px0, (uint16_t)(py0 + y),
						   (uint16_t)pw, 1);
		if (!payload)
			return;
		uint8_t *dst = payload;
		for (int x = 0; x < pw; x++) {
			uint8_t level = src[x];
			write_px(&dst, t->wf_palette888[level]);
		}
		(void)rf_fb_write_box((struct rf_fb *)&t->fb);
	}
}

