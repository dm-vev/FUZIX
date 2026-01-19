#include "vec_plot.h"

#include "vec_eval.h"
#include "vec_value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_px(uint8_t **dst, struct vec_color c)
{
	uint8_t *p = *dst;
	*p++ = c.b;
	*p++ = c.g;
	*p++ = c.r;
	*dst = p;
}

static int clamp_int(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void mask_set(uint8_t *mask, size_t bit)
{
	mask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static int mask_get(const uint8_t *mask, size_t bit)
{
	return (mask[bit >> 3] & (uint8_t)(1u << (bit & 7))) != 0;
}

static void mask_draw_line(uint8_t *mask, int w, int h, int x0, int y0, int x1, int y1)
{
	int dx = abs(x1 - x0);
	int sx = (x0 < x1) ? 1 : -1;
	int dy = -abs(y1 - y0);
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
			mask_set(mask, (size_t)y0 * (size_t)w + (size_t)x0);
		}
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = err * 2;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static int map_x(double x, double x_min, double x_max, int w)
{
	double t = (x - x_min) / (x_max - x_min);
	int px = (int)lround(t * (double)(w - 1));
	return clamp_int(px, 0, w - 1);
}

static int map_y(double y, double y_min, double y_max, int h)
{
	double t = (y - y_min) / (y_max - y_min);
	int yp = (int)lround(t * (double)(h - 1));
	int py = (h - 1) - yp;
	return clamp_int(py, 0, h - 1);
}

int vec_plot_render(struct vec_fb *fb, int x0, int y0, int w, int h,
		    vec_env *env, const vec_node *expr,
		    double x_min, double x_max, double y_min, double y_max,
		    char *err, size_t errsz)
{
	if (!fb || fb->fd < 0 || !env) {
		snprintf(err, errsz, "plot: bad args");
		return -1;
	}
	if (w <= 0 || h <= 0) {
		snprintf(err, errsz, "plot: bad size");
		return -1;
	}
	if (x_max <= x_min || y_max <= y_min) {
		snprintf(err, errsz, "plot: bad view");
		return -1;
	}

	struct vec_color bg = {0x08, 0x08, 0x08};
	struct vec_color axis = {0x55, 0x55, 0x55};
	struct vec_color plot = {0x4A, 0xD1, 0xFF};

	int axis_x = -1;
	if (x_min <= 0 && x_max >= 0) {
		double t = (0 - x_min) / (x_max - x_min);
		axis_x = x0 + (int)lround(t * (double)(w - 1));
		axis_x = clamp_int(axis_x, x0, x0 + w - 1);
	}
	int axis_y = -1;
	if (y_min <= 0 && y_max >= 0) {
		double t = (0 - y_min) / (y_max - y_min);
		int yp = (int)lround(t * (double)(h - 1));
		axis_y = y0 + (h - 1 - yp);
		axis_y = clamp_int(axis_y, y0, y0 + h - 1);
	}

	int16_t ylo[512];
	int16_t yhi[512];
	if (w > (int)(sizeof(ylo) / sizeof(ylo[0]))) {
		snprintf(err, errsz, "plot: width too large");
		return -1;
	}
	for (int i = 0; i < w; i++) {
		ylo[i] = -1;
		yhi[i] = -1;
	}

	uint8_t *mask = NULL;
	int use_mask = 0;
	vec_value mv;
	memset(&mv, 0, sizeof(mv));

	if (expr) {
		char ebuf[96];
		ebuf[0] = 0;
		if (vec_eval_node(env, expr, &mv, ebuf, sizeof(ebuf)) == 0 &&
		    mv.kind == VEC_VALUE_MATRIX && mv.cols == 2 && mv.rows > 0 && mv.mat) {
			size_t bits = (size_t)w * (size_t)h;
			size_t bytes = (bits + 7) / 8;
			mask = calloc(bytes, 1);
			if (!mask) {
				snprintf(err, errsz, "plot: out of memory");
				vec_value_destroy(&mv);
				return -1;
			}
			int prev_valid = 0;
			int prev_px = 0;
			int prev_py = 0;
			for (int i = 0; i < mv.rows; i++) {
				double x = mv.mat[i * 2 + 0];
				double y = mv.mat[i * 2 + 1];
				if (!isfinite(x) || !isfinite(y)) {
					prev_valid = 0;
					continue;
				}
				int px = map_x(x, x_min, x_max, w);
				int py = map_y(y, y_min, y_max, h);
				if (prev_valid) {
					mask_draw_line(mask, w, h, prev_px, prev_py, px, py);
				} else {
					mask_set(mask, (size_t)py * (size_t)w + (size_t)px);
				}
				prev_valid = 1;
				prev_px = px;
				prev_py = py;
			}
			use_mask = 1;
		} else {
			vec_value_destroy(&mv);
			memset(&mv, 0, sizeof(mv));

			int prev_valid = 0;
			int prev_y = 0;
			for (int px = 0; px < w; px++) {
				double x = x_min + ((double)px / (double)(w - 1)) * (x_max - x_min);
				vec_value v;
				memset(&v, 0, sizeof(v));
				ebuf[0] = 0;
				if (vec_eval_node_override(env, expr, "x", vec_value_number(vec_float(x)), &v, ebuf, sizeof(ebuf)) != 0) {
					prev_valid = 0;
					continue;
				}
				if (v.kind != VEC_VALUE_NUMBER) {
					prev_valid = 0;
					continue;
				}
				double y = vec_number_float64(v.num);
				if (!isfinite(y)) {
					prev_valid = 0;
					continue;
				}
				double ty = (y - y_min) / (y_max - y_min);
				int yp = y0 + (h - 1 - (int)lround(ty * (double)(h - 1)));
				yp = clamp_int(yp, y0, y0 + h - 1);
				int relx = px;
				if (prev_valid) {
					int lo = prev_y < yp ? prev_y : yp;
					int hi = prev_y > yp ? prev_y : yp;
					ylo[relx] = (int16_t)lo;
					yhi[relx] = (int16_t)hi;
				} else {
					ylo[relx] = (int16_t)yp;
					yhi[relx] = (int16_t)yp;
				}
				prev_valid = 1;
				prev_y = yp;
			}
		}
	}

	const int block_h = 8;
	for (int by = 0; by < h; by += block_h) {
		int bh = h - by;
		if (bh > block_h)
			bh = block_h;

		uint8_t *payload = vec_fb_begin_box(fb, (uint16_t)x0, (uint16_t)(y0 + by),
						    (uint16_t)w, (uint16_t)bh);
		if (!payload) {
			snprintf(err, errsz, "plot: out of memory");
			return -1;
		}

		for (int py = 0; py < bh; py++) {
			int y = y0 + by + py;
			uint8_t *dst = payload + (size_t)py * (size_t)w * 3;
			for (int px = 0; px < w; px++) {
				int x = x0 + px;
				struct vec_color c = bg;
				if (axis_x >= 0 && x == axis_x)
					c = axis;
				if (axis_y >= 0 && y == axis_y)
					c = axis;
				if (use_mask) {
					size_t bit = (size_t)(by + py) * (size_t)w + (size_t)px;
					if (mask_get(mask, bit))
						c = plot;
				} else {
					if (expr && ylo[px] >= 0 && yhi[px] >= 0 && y >= ylo[px] && y <= yhi[px])
						c = plot;
				}
				put_px(&dst, c);
			}
		}

		if (vec_fb_write_box(fb) != 0) {
			snprintf(err, errsz, "plot: fb write failed");
			free(mask);
			if (use_mask)
				vec_value_destroy(&mv);
			return -1;
		}
	}
	free(mask);
	if (use_mask)
		vec_value_destroy(&mv);
	return 0;
}
