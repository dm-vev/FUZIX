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

static void plot_idx_draw_line(uint8_t *idx, int w, int h, int x0, int y0, int x1, int y1, uint8_t v)
{
	int dx = abs(x1 - x0);
	int sx = (x0 < x1) ? 1 : -1;
	int dy = -abs(y1 - y0);
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
			idx[(size_t)y0 * (size_t)w + (size_t)x0] = v;
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

static void plot_idx_draw_func(uint8_t *idx, int w, int h, vec_env *env, const vec_node *expr,
			       double x_min, double x_max, double y_min, double y_max, uint8_t v)
{
	if (!idx || !env || !expr || w <= 1 || h <= 1)
		return;

	int prev_ok = 0;
	int prev_px = 0;
	int prev_py = 0;
	for (int ix = 0; ix < w; ix++) {
		double x = x_min + ((double)ix / (double)(w - 1)) * (x_max - x_min);
		vec_value out;
		memset(&out, 0, sizeof(out));
		char ebuf[96];
		ebuf[0] = 0;
		if (vec_eval_node_override(env, expr, "x", vec_value_number(vec_float(x)), &out, ebuf, sizeof(ebuf)) != 0) {
			prev_ok = 0;
			continue;
		}
		if (out.kind != VEC_VALUE_NUMBER) {
			prev_ok = 0;
			continue;
		}
		double y = vec_number_float64(out.num);
		if (!isfinite(y)) {
			prev_ok = 0;
			continue;
		}
		int px = ix;
		int py = map_y(y, y_min, y_max, h);
		if (prev_ok) {
			plot_idx_draw_line(idx, w, h, prev_px, prev_py, px, py, v);
		} else {
			idx[(size_t)py * (size_t)w + (size_t)px] = v;
		}
		prev_ok = 1;
		prev_px = px;
		prev_py = py;
	}
}

static void plot_idx_draw_series_f(uint8_t *idx, int w, int h, const float *xs, const float *ys, size_t n,
				   double x_min, double x_max, double y_min, double y_max, uint8_t v)
{
	if (!idx || !xs || !ys || n == 0 || w <= 1 || h <= 1)
		return;

	int prev_ok = 0;
	int prev_px = 0;
	int prev_py = 0;
	for (size_t i = 0; i < n; i++) {
		double x = (double)xs[i];
		double y = (double)ys[i];
		if (!isfinite(x) || !isfinite(y)) {
			prev_ok = 0;
			continue;
		}
		int px = map_x(x, x_min, x_max, w);
		int py = map_y(y, y_min, y_max, h);
		if (prev_ok) {
			plot_idx_draw_line(idx, w, h, prev_px, prev_py, px, py, v);
		} else {
			idx[(size_t)py * (size_t)w + (size_t)px] = v;
		}
		prev_ok = 1;
		prev_px = px;
		prev_py = py;
	}
}

static void plot_idx_draw_series_mat(uint8_t *idx, int w, int h, const double *mat, int rows,
				     double x_min, double x_max, double y_min, double y_max, uint8_t v)
{
	if (!idx || !mat || rows <= 0 || w <= 1 || h <= 1)
		return;

	int prev_ok = 0;
	int prev_px = 0;
	int prev_py = 0;
	for (int i = 0; i < rows; i++) {
		double x = mat[i * 2 + 0];
		double y = mat[i * 2 + 1];
		if (!isfinite(x) || !isfinite(y)) {
			prev_ok = 0;
			continue;
		}
		int px = map_x(x, x_min, x_max, w);
		int py = map_y(y, y_min, y_max, h);
		if (prev_ok) {
			plot_idx_draw_line(idx, w, h, prev_px, prev_py, px, py, v);
		} else {
			idx[(size_t)py * (size_t)w + (size_t)px] = v;
		}
		prev_ok = 1;
		prev_px = px;
		prev_py = py;
	}
}

int vec_plot_render_multi(struct vec_fb *fb, int x0, int y0, int w, int h,
			  vec_env *env, const vec_plot *plots, size_t plot_count,
			  const vec_node *fallback_expr,
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
	static const struct vec_color plot_colors[] = {
		{0x4A, 0xD1, 0xFF},
		{0xFF, 0xD1, 0x4A},
		{0x7F, 0xFF, 0x7F},
		{0xFF, 0x7F, 0xFF},
	};

	int axis_px = -1;
	if (x_min <= 0 && x_max >= 0)
		axis_px = map_x(0, x_min, x_max, w);
	int axis_py = -1;
	if (y_min <= 0 && y_max >= 0)
		axis_py = map_y(0, y_min, y_max, h);

	size_t wh = (size_t)w * (size_t)h;
	if (w <= 0 || h <= 0 || wh / (size_t)w != (size_t)h) {
		snprintf(err, errsz, "plot: bad size");
		return -1;
	}
	uint8_t *idx = calloc(wh, 1);
	if (!idx) {
		snprintf(err, errsz, "plot: out of memory");
		return -1;
	}

	if (plots && plot_count > 0) {
		for (size_t i = 0; i < plot_count; i++) {
			const vec_plot *p = &plots[i];
			uint8_t ci = (uint8_t)((i % (sizeof(plot_colors) / sizeof(plot_colors[0]))) + 1);
			if (p->kind == VEC_PLOT_SERIES) {
				plot_idx_draw_series_f(idx, w, h, p->xs, p->ys, p->len,
						       x_min, x_max, y_min, y_max, ci);
			} else if (p->kind == VEC_PLOT_FUNC) {
				plot_idx_draw_func(idx, w, h, env, p->expr,
						   x_min, x_max, y_min, y_max, ci);
			}
		}
	} else if (fallback_expr) {
		vec_value mv;
		memset(&mv, 0, sizeof(mv));
		char ebuf[96];
		ebuf[0] = 0;
		if (vec_eval_node(env, fallback_expr, &mv, ebuf, sizeof(ebuf)) == 0 &&
		    mv.kind == VEC_VALUE_MATRIX && mv.cols == 2 && mv.rows > 0 && mv.mat) {
			plot_idx_draw_series_mat(idx, w, h, mv.mat, mv.rows,
						 x_min, x_max, y_min, y_max, 1);
		} else {
			plot_idx_draw_func(idx, w, h, env, fallback_expr,
					   x_min, x_max, y_min, y_max, 1);
		}
		vec_value_destroy(&mv);
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
			free(idx);
			return -1;
		}

		for (int py = 0; py < bh; py++) {
			uint8_t *dst = payload + (size_t)py * (size_t)w * 3;
			for (int px = 0; px < w; px++) {
				struct vec_color c = bg;
				if (axis_px >= 0 && px == axis_px)
					c = axis;
				if (axis_py >= 0 && (by + py) == axis_py)
					c = axis;

				uint8_t pi = idx[(size_t)(by + py) * (size_t)w + (size_t)px];
				if (pi) {
					size_t ci = (size_t)(pi - 1) % (sizeof(plot_colors) / sizeof(plot_colors[0]));
					c = plot_colors[ci];
				}
				put_px(&dst, c);
			}
		}

		if (vec_fb_write_box(fb) != 0) {
			snprintf(err, errsz, "plot: fb write failed");
			free(idx);
			return -1;
		}
	}

	free(idx);
	return 0;
}

int vec_plot_render(struct vec_fb *fb, int x0, int y0, int w, int h,
		    vec_env *env, const vec_node *expr,
		    double x_min, double x_max, double y_min, double y_max,
		    char *err, size_t errsz)
{
	return vec_plot_render_multi(fb, x0, y0, w, h, env, NULL, 0, expr,
				     x_min, x_max, y_min, y_max, err, errsz);
}
