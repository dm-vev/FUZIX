#include "vec_plot.h"

#include "vec_eval.h"
#include "vec_value.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double clamp_double(double v, double lo, double hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static double clamp01(double t)
{
	return clamp_double(t, 0.0, 1.0);
}

static void put_px(uint8_t **dst, struct vec_color c)
{
	uint8_t *p = *dst;
	*p++ = c.b;
	*p++ = c.g;
	*p++ = c.r;
	*dst = p;
}

static void put_px_at(uint8_t *payload, int w, int x, int y, struct vec_color c)
{
	if (!payload || w <= 0)
		return;
	if (x < 0 || y < 0)
		return;
	size_t off = ((size_t)y * (size_t)w + (size_t)x) * 3;
	payload[off + 0] = c.b;
	payload[off + 1] = c.g;
	payload[off + 2] = c.r;
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

static uint8_t lerp_u8(uint8_t a, uint8_t b, double t)
{
	t = clamp01(t);
	return (uint8_t)((double)a + ((double)b - (double)a) * t);
}

static struct vec_color plot_color_ramp(double t)
{
	static const struct vec_color plot0 = {0x4A, 0xD1, 0xFF};
	t = clamp01(t);
	if (t < 0.5) {
		double u = t / 0.5;
		struct vec_color c = {
			lerp_u8(0x10, plot0.r, u),
			lerp_u8(0x20, plot0.g, u),
			lerp_u8(0x80, plot0.b, u),
		};
		return c;
	}
	double u = (t - 0.5) / 0.5;
	struct vec_color c = {
		lerp_u8(plot0.r, 0xFF, u),
		lerp_u8(plot0.g, 0xF0, u),
		lerp_u8(plot0.b, 0x40, u),
	};
	return c;
}

static struct vec_color plot_3d_wire_color(int mode, double xN, double yN, double z01)
{
	static const struct vec_color plot0 = {0x4A, 0xD1, 0xFF};

	switch (mode) {
	case 1:
		return plot_color_ramp(z01);
	case 2: {
		double u = clamp01(0.5 * (xN + 1));
		double v = clamp01(0.5 * (yN + 1));
		double b = 0.35 + 0.65 * clamp01(z01);
		struct vec_color c = {
			(uint8_t)(255 * clamp01(u * b)),
			(uint8_t)(255 * clamp01(v * b)),
			(uint8_t)(255 * clamp01((1 - u) * b)),
		};
		return c;
	}
	default:
		return plot0;
	}
}

static uint8_t depth_to_byte(double denom)
{
	if (denom < 0 || isnan(denom) || isinf(denom))
		return 0xFF;
	int v = (int)(denom * 50.0);
	if (v < 0)
		v = 0;
	if (v > 0xFF)
		v = 0xFF;
	return (uint8_t)v;
}

static int clip_line_to_rect_with_t(double x0, double y0, double x1, double y1,
				    double xmin, double ymin, double xmax, double ymax,
				    double *cx0, double *cy0, double *cx1, double *cy1,
				    double *u0, double *u1)
{
	double dx = x1 - x0;
	double dy = y1 - y0;

	double a = 0.0;
	double b = 1.0;

	double p[4] = {-dx, dx, -dy, dy};
	double q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
	for (int i = 0; i < 4; i++) {
		if (p[i] == 0) {
			if (q[i] < 0)
				return 0;
			continue;
		}
		double t = q[i] / p[i];
		if (p[i] < 0) {
			if (t > b)
				return 0;
			if (t > a)
				a = t;
		} else {
			if (t < a)
				return 0;
			if (t < b)
				b = t;
		}
	}

	if (u0)
		*u0 = a;
	if (u1)
		*u1 = b;

	if (cx0)
		*cx0 = clamp_double(x0 + a * dx, xmin, xmax);
	if (cy0)
		*cy0 = clamp_double(y0 + a * dy, ymin, ymax);
	if (cx1)
		*cx1 = clamp_double(x0 + b * dx, xmin, xmax);
	if (cy1)
		*cy1 = clamp_double(y0 + b * dy, ymin, ymax);
	return 1;
}

static void plot3d_draw_line_depth_block(uint8_t *payload, int panel_w,
					int block_y, int block_h,
					int plot_w, int y_base, int y_limit,
					double x0, double y0, double d0,
					double x1, double y1, double d1,
					struct vec_color c, uint8_t *zsub)
{
	if (!payload || panel_w <= 0 || block_h <= 0 || plot_w <= 0)
		return;
	if (!zsub || y_base < 0 || y_limit <= y_base)
		return;

	double dx = x1 - x0;
	double dy = y1 - y0;
	double steps = fabs(dx);
	double ady = fabs(dy);
	if (ady > steps)
		steps = ady;
	int n = (int)steps;
	if (n <= 0) {
		int ix = (int)lround(x0);
		int iy = (int)lround(y0);
		if (ix < 0 || ix >= plot_w || iy < y_base || iy >= y_limit)
			return;
		size_t idx = (size_t)(iy - y_base) * (size_t)plot_w + (size_t)ix;
		uint8_t z = depth_to_byte(d0);
		if (z > zsub[idx])
			return;
		zsub[idx] = z;

		int px = 1 + ix;
		int py = (1 + iy) - block_y;
		if (py < 0 || py >= block_h)
			return;
		put_px_at(payload, panel_w, px, py, c);
		return;
	}

	for (int i = 0; i <= n; i++) {
		double tp = (double)i / (double)n;
		double x = x0 + dx * tp;
		double y = y0 + dy * tp;
		double d = d0 + (d1 - d0) * tp;

		int ix = (int)lround(x);
		int iy = (int)lround(y);
		if (ix < 0 || ix >= plot_w || iy < y_base || iy >= y_limit)
			continue;

		size_t idx = (size_t)(iy - y_base) * (size_t)plot_w + (size_t)ix;
		uint8_t z = depth_to_byte(d);
		if (z > zsub[idx])
			continue;
		zsub[idx] = z;

		int px = 1 + ix;
		int py = (1 + iy) - block_y;
		if (py < 0 || py >= block_h)
			continue;
		put_px_at(payload, panel_w, px, py, c);
	}
}

static void plot3d_draw_seg_block(uint8_t *payload, int panel_w,
				  int block_y, int block_h,
				  int plot_w, int plot_h,
				  int y_base, int y_limit,
				  double x0, double y0, double d0,
				  double x1, double y1, double d1,
				  struct vec_color c, uint8_t *zsub)
{
	(void)plot_h;
	if (!payload || panel_w <= 0 || block_h <= 0 || plot_w <= 0)
		return;
	if (!zsub || y_base < 0 || y_limit <= y_base)
		return;

	double miny = y0 < y1 ? y0 : y1;
	double maxy = y0 > y1 ? y0 : y1;
	if (maxy < (double)y_base || miny > (double)(y_limit - 1))
		return;

	double cx0, cy0, cx1, cy1, u0, u1;
	if (!clip_line_to_rect_with_t(x0, y0, x1, y1,
				      0.0, (double)y_base, (double)(plot_w - 1), (double)(y_limit - 1),
				      &cx0, &cy0, &cx1, &cy1, &u0, &u1))
		return;

	double cd0 = d0 + u0 * (d1 - d0);
	double cd1 = d0 + u1 * (d1 - d0);
	plot3d_draw_line_depth_block(payload, panel_w, block_y, block_h,
				     plot_w, y_base, y_limit,
				     cx0, cy0, cd0, cx1, cy1, cd1, c, zsub);
}

static int project3d_to_plot(double x, double y, double z,
			     int plot_w, int plot_h,
			     double zoom,
			     double c_yaw, double s_yaw,
			     double c_pitch, double s_pitch,
			     double size,
			     double *px, double *py, double *depth)
{
	if (plot_w <= 0 || plot_h <= 0)
		return 0;

	if (zoom <= 0 || isnan(zoom) || isinf(zoom))
		zoom = 1;

	double x1 = x * c_yaw - y * s_yaw;
	double y1 = x * s_yaw + y * c_yaw;
	double z1 = z;

	double y2 = y1 * c_pitch + z1 * s_pitch;
	double z2 = -y1 * s_pitch + z1 * c_pitch;
	double x2 = x1;

	const double dist = 3.0;
	double denom = dist - z2;
	if (denom <= 0.2)
		return 0;

	double persp = zoom / denom;
	if (size <= 1)
		return 0;

	if (px)
		*px = (double)(plot_w - 1) / 2 + x2 * persp * size;
	if (py)
		*py = (double)(plot_h - 1) / 2 - y2 * persp * size;
	if (depth)
		*depth = denom;
	return 1;
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

int vec_plot_render_3d(struct vec_fb *fb, int x0, int y0, int w, int h,
		       vec_env *env, const vec_node *expr,
		       double x_min, double x_max, double y_min, double y_max,
		       double yaw, double pitch, double zoom,
		       int color_mode, int show_axes,
		       char *err, size_t errsz)
{
	if (!fb || fb->fd < 0 || !env) {
		snprintf(err, errsz, "plot: bad args");
		return -1;
	}
	if (!expr) {
		snprintf(err, errsz, "3D: no expression");
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

	struct vec_color color_bg = {0x00, 0x00, 0x00};
	struct vec_color color_panel_bg = {0x08, 0x08, 0x08};
	struct vec_color color_axis = {0x55, 0x55, 0x55};
	struct vec_color color_plot0 = {0x4A, 0xD1, 0xFF};
	struct vec_color color_axis_x = {0xF0, 0x40, 0x40};
	struct vec_color color_axis_y = {0x40, 0xF0, 0x40};
	struct vec_color color_axis_z = {0x40, 0x80, 0xFF};

	int plot_w = w - 2;
	int plot_h = h - 2;
	if (plot_w <= 2 || plot_h <= 2)
		return 0;

	int grid_x = clamp_int(plot_w / 8, 12, 32);
	int grid_y = clamp_int(plot_h / 8, 12, 32);
	if (grid_x < 2)
		grid_x = 2;
	if (grid_y < 2)
		grid_y = 2;

	double x_c = (x_min + x_max) / 2;
	double y_c = (y_min + y_max) / 2;
	double x_r = (x_max - x_min) / 2;
	double y_r = (y_max - y_min) / 2;
	if (x_r == 0 || isnan(x_r) || isinf(x_r))
		x_r = 1;
	if (y_r == 0 || isnan(y_r) || isinf(y_r))
		y_r = 1;

	size_t grid_n = (size_t)grid_x * (size_t)grid_y;
	if (grid_x <= 0 || grid_y <= 0 || grid_n / (size_t)grid_x != (size_t)grid_y) {
		snprintf(err, errsz, "plot: bad size");
		return -1;
	}

	float *z_grid = malloc(sizeof(z_grid[0]) * grid_n);
	if (!z_grid) {
		snprintf(err, errsz, "plot: out of memory");
		return -1;
	}
	for (size_t i = 0; i < grid_n; i++)
		z_grid[i] = (float)NAN;

	vec_value prev_x;
	vec_value prev_y;
	memset(&prev_x, 0, sizeof(prev_x));
	memset(&prev_y, 0, sizeof(prev_y));
	int had_prev_x = 0;
	int had_prev_y = 0;
	{
		int rc = vec_env_get_var(env, "x", &prev_x);
		if (rc < 0) {
			snprintf(err, errsz, "plot: out of memory");
			free(z_grid);
			return -1;
		}
		had_prev_x = (rc == 0);
		rc = vec_env_get_var(env, "y", &prev_y);
		if (rc < 0) {
			snprintf(err, errsz, "plot: out of memory");
			if (had_prev_x)
				vec_value_destroy(&prev_x);
			free(z_grid);
			return -1;
		}
		had_prev_y = (rc == 0);
	}

	double z_min = DBL_MAX;
	double z_max = -DBL_MAX;
	for (int iy = 0; iy < grid_y; iy++) {
		double y = y_min + ((double)iy / (double)(grid_y - 1)) * (y_max - y_min);
		for (int ix = 0; ix < grid_x; ix++) {
			double x = x_min + ((double)ix / (double)(grid_x - 1)) * (x_max - x_min);

			if (vec_env_set_var(env, "x", vec_value_number(vec_float(x))) != 0 ||
			    vec_env_set_var(env, "y", vec_value_number(vec_float(y))) != 0) {
				snprintf(err, errsz, "plot: out of memory");
				goto cleanup;
			}

			vec_value out;
			memset(&out, 0, sizeof(out));
			char ebuf[64];
			ebuf[0] = 0;
			if (vec_eval_node(env, expr, &out, ebuf, sizeof(ebuf)) != 0) {
				vec_value_destroy(&out);
				continue;
			}
			if (out.kind != VEC_VALUE_NUMBER) {
				vec_value_destroy(&out);
				continue;
			}
			double z = vec_number_float64(out.num);
			vec_value_destroy(&out);
			if (!isfinite(z))
				continue;

			z_grid[(size_t)iy * (size_t)grid_x + (size_t)ix] = (float)z;
			if (z < z_min)
				z_min = z;
			if (z > z_max)
				z_max = z;
		}
	}

cleanup:
	/* Restore env. */
	if (had_prev_x) {
		if (vec_env_set_var(env, "x", prev_x) != 0)
			vec_value_destroy(&prev_x);
		memset(&prev_x, 0, sizeof(prev_x));
	} else {
		(void)vec_env_unset_var(env, "x");
	}
	if (had_prev_y) {
		if (vec_env_set_var(env, "y", prev_y) != 0)
			vec_value_destroy(&prev_y);
		memset(&prev_y, 0, sizeof(prev_y));
	} else {
		(void)vec_env_unset_var(env, "y");
	}

	int have_samples = isfinite(z_min) && isfinite(z_max) && z_min <= z_max;
	if (!have_samples && (!err || !err[0]))
		snprintf(err, errsz, "3D: no valid samples");

	double z_c = have_samples ? (z_min + z_max) / 2 : 0;
	double z_r = have_samples ? (z_max - z_min) / 2 : 1;
	if (z_r == 0 || isnan(z_r) || isinf(z_r))
		z_r = 1;

	double c_yaw = cos(yaw);
	double s_yaw = sin(yaw);
	double c_pitch = cos(pitch);
	double s_pitch = sin(pitch);
	double size = 0.45 * fmin((double)(plot_w - 1), (double)(plot_h - 1));

	float *px_grid = NULL;
	float *py_grid = NULL;
	float *d_grid = NULL;
	uint8_t *ok_grid = NULL;
	uint8_t *z01_grid = NULL;
	float *xN = NULL;
	float *yN = NULL;

	if (have_samples) {
		px_grid = malloc(sizeof(px_grid[0]) * grid_n);
		py_grid = malloc(sizeof(py_grid[0]) * grid_n);
		d_grid = malloc(sizeof(d_grid[0]) * grid_n);
		ok_grid = malloc(sizeof(ok_grid[0]) * grid_n);
		z01_grid = malloc(sizeof(z01_grid[0]) * grid_n);
		xN = malloc(sizeof(xN[0]) * (size_t)grid_x);
		yN = malloc(sizeof(yN[0]) * (size_t)grid_y);
		if (!px_grid || !py_grid || !d_grid || !ok_grid || !z01_grid || !xN || !yN) {
			snprintf(err, errsz, "plot: out of memory");
			free(px_grid);
			free(py_grid);
			free(d_grid);
			free(ok_grid);
			free(z01_grid);
			free(xN);
			free(yN);
			free(z_grid);
			return -1;
		}

		for (int ix = 0; ix < grid_x; ix++) {
			double x = x_min + ((double)ix / (double)(grid_x - 1)) * (x_max - x_min);
			xN[ix] = (float)((x - x_c) / x_r);
		}
		for (int iy = 0; iy < grid_y; iy++) {
			double y = y_min + ((double)iy / (double)(grid_y - 1)) * (y_max - y_min);
			yN[iy] = (float)((y - y_c) / y_r);
		}

		for (int iy = 0; iy < grid_y; iy++) {
			for (int ix = 0; ix < grid_x; ix++) {
				size_t gi = (size_t)iy * (size_t)grid_x + (size_t)ix;
				ok_grid[gi] = 0;
				z01_grid[gi] = 0;
				double z = (double)z_grid[gi];
				if (!isfinite(z))
					continue;

				double zNf = (z - z_c) / z_r;
				double z01 = clamp01(0.5 * (zNf + 1));

				double px, py, d;
				if (!project3d_to_plot(xN[ix], yN[iy], zNf, plot_w, plot_h, zoom,
						       c_yaw, s_yaw, c_pitch, s_pitch, size,
						       &px, &py, &d))
					continue;

				px_grid[gi] = (float)px;
				py_grid[gi] = (float)py;
				d_grid[gi] = (float)d;
				z01_grid[gi] = (uint8_t)lround(clamp01(z01) * 255.0);
				ok_grid[gi] = 1;
			}
		}
	}

	const int block_h = 32;
	uint8_t *zsub = NULL;
	if (plot_w > 0)
		zsub = malloc((size_t)plot_w * (size_t)block_h);
	if (plot_w > 0 && !zsub) {
		snprintf(err, errsz, "plot: out of memory");
		free(px_grid);
		free(py_grid);
		free(d_grid);
		free(ok_grid);
		free(z01_grid);
		free(xN);
		free(yN);
		free(z_grid);
		return -1;
	}

	const int step_x = (grid_x > 24) ? 2 : 1;
	const int step_y = (grid_y > 24) ? 2 : 1;

	static const double cube_edges[12][2][3] = {
		{{-1, -1, -1}, {1, -1, -1}},
		{{-1, 1, -1}, {1, 1, -1}},
		{{-1, -1, 1}, {1, -1, 1}},
		{{-1, 1, 1}, {1, 1, 1}},

		{{-1, -1, -1}, {-1, 1, -1}},
		{{1, -1, -1}, {1, 1, -1}},
		{{-1, -1, 1}, {-1, 1, 1}},
		{{1, -1, 1}, {1, 1, 1}},

		{{-1, -1, -1}, {-1, -1, 1}},
		{{1, -1, -1}, {1, -1, 1}},
		{{-1, 1, -1}, {-1, 1, 1}},
		{{1, 1, -1}, {1, 1, 1}},
	};

	for (int by = 0; by < h; by += block_h) {
		int bh = h - by;
		if (bh > block_h)
			bh = block_h;

		uint8_t *payload = vec_fb_begin_box(fb, (uint16_t)x0, (uint16_t)(y0 + by),
						    (uint16_t)w, (uint16_t)bh);
		if (!payload) {
			snprintf(err, errsz, "plot: out of memory");
			free(zsub);
			free(px_grid);
			free(py_grid);
			free(d_grid);
			free(ok_grid);
			free(z01_grid);
			free(xN);
			free(yN);
			free(z_grid);
			return -1;
		}

		for (int py = 0; py < bh; py++) {
			int panel_y = by + py;
			uint8_t *dst = payload + (size_t)py * (size_t)w * 3;
			for (int px = 0; px < w; px++) {
				struct vec_color c = color_bg;
				if (px >= 1 && px < w - 1 && panel_y >= 1 && panel_y < h - 1)
					c = color_panel_bg;
				put_px(&dst, c);
			}
		}

		int y_base = by - 1;
		if (y_base < 0)
			y_base = 0;
		int y_limit = by + bh - 1;
		if (y_limit > plot_h)
			y_limit = plot_h;
		if (y_limit > y_base)
			memset(zsub, 0xFF, (size_t)plot_w * (size_t)(y_limit - y_base));

		for (size_t ei = 0; ei < (sizeof(cube_edges) / sizeof(cube_edges[0])); ei++) {
			double ax = cube_edges[ei][0][0];
			double ay = cube_edges[ei][0][1];
			double az = cube_edges[ei][0][2];
			double bx = cube_edges[ei][1][0];
			double by3 = cube_edges[ei][1][1];
			double bz = cube_edges[ei][1][2];
			double x0p, y0p, d0p;
			double x1p, y1p, d1p;
			if (!project3d_to_plot(ax, ay, az, plot_w, plot_h, zoom,
					       c_yaw, s_yaw, c_pitch, s_pitch, size,
					       &x0p, &y0p, &d0p) ||
			    !project3d_to_plot(bx, by3, bz, plot_w, plot_h, zoom,
					       c_yaw, s_yaw, c_pitch, s_pitch, size,
					       &x1p, &y1p, &d1p))
				continue;
			if (y_limit > y_base)
				plot3d_draw_seg_block(payload, w, by, bh, plot_w, plot_h, y_base, y_limit,
						      x0p, y0p, d0p, x1p, y1p, d1p, color_axis, zsub);
		}

		if (show_axes && y_limit > y_base) {
			double ox, oy, od;
			if (project3d_to_plot(0, 0, 0, plot_w, plot_h, zoom,
					      c_yaw, s_yaw, c_pitch, s_pitch, size,
					      &ox, &oy, &od)) {
				struct {
					double x;
					double y;
					double z;
					struct vec_color c;
				} axes[] = {
					{1, 0, 0, color_axis_x},
					{0, 1, 0, color_axis_y},
					{0, 0, 1, color_axis_z},
				};
				for (size_t ai = 0; ai < (sizeof(axes) / sizeof(axes[0])); ai++) {
					double ex, ey, ed;
					if (!project3d_to_plot(axes[ai].x, axes[ai].y, axes[ai].z,
							       plot_w, plot_h, zoom,
							       c_yaw, s_yaw, c_pitch, s_pitch, size,
							       &ex, &ey, &ed))
						continue;

					plot3d_draw_seg_block(payload, w, by, bh, plot_w, plot_h, y_base, y_limit,
							      ox, oy, od, ex, ey, ed, axes[ai].c, zsub);

					double dx = ex - ox;
					double dy = ey - oy;
					double n = hypot(dx, dy);
					if (n <= 0 || isnan(n) || isinf(n))
						continue;
					double ux = dx / n;
					double uy = dy / n;
					double px = -uy;
					double py = ux;
					double arrow_len = 7.0;
					double arrow_w = 3.0;
					double bx = ex - ux * arrow_len;
					double by2 = ey - uy * arrow_len;
					double lx = bx + px * arrow_w;
					double ly = by2 + py * arrow_w;
					double rx = bx - px * arrow_w;
					double ry = by2 - py * arrow_w;
					plot3d_draw_seg_block(payload, w, by, bh, plot_w, plot_h, y_base, y_limit,
							      ex, ey, ed, lx, ly, ed, axes[ai].c, zsub);
					plot3d_draw_seg_block(payload, w, by, bh, plot_w, plot_h, y_base, y_limit,
							      ex, ey, ed, rx, ry, ed, axes[ai].c, zsub);
				}
			}
		}

		if (have_samples && y_limit > y_base) {
			for (int iy = 0; iy < grid_y; iy += step_y) {
				int prev_ok = 0;
				int prev_ix = 0;
				size_t prev_gi = 0;
				for (int ix = 0; ix < grid_x; ix += step_x) {
					size_t gi = (size_t)iy * (size_t)grid_x + (size_t)ix;
					if (!ok_grid[gi]) {
						prev_ok = 0;
						continue;
					}
					if (prev_ok) {
						struct vec_color c = color_plot0;
						if (color_mode != 0) {
							double xmid = 0.5 * ((double)xN[prev_ix] + (double)xN[ix]);
							double ymid = (double)yN[iy];
							double zmid = 0.5 * ((double)z01_grid[prev_gi] + (double)z01_grid[gi]) / 255.0;
							c = plot_3d_wire_color(color_mode, xmid, ymid, zmid);
						}
						plot3d_draw_seg_block(payload, w, by, bh, plot_w, plot_h, y_base, y_limit,
								      px_grid[prev_gi], py_grid[prev_gi], d_grid[prev_gi],
								      px_grid[gi], py_grid[gi], d_grid[gi],
								      c, zsub);
					}
					prev_ok = 1;
					prev_ix = ix;
					prev_gi = gi;
				}
			}

			for (int ix = 0; ix < grid_x; ix += step_x) {
				int prev_ok = 0;
				int prev_iy = 0;
				size_t prev_gi = 0;
				for (int iy = 0; iy < grid_y; iy += step_y) {
					size_t gi = (size_t)iy * (size_t)grid_x + (size_t)ix;
					if (!ok_grid[gi]) {
						prev_ok = 0;
						continue;
					}
					if (prev_ok) {
						struct vec_color c = color_plot0;
						if (color_mode != 0) {
							double xmid = (double)xN[ix];
							double ymid = 0.5 * ((double)yN[prev_iy] + (double)yN[iy]);
							double zmid = 0.5 * ((double)z01_grid[prev_gi] + (double)z01_grid[gi]) / 255.0;
							c = plot_3d_wire_color(color_mode, xmid, ymid, zmid);
						}
						plot3d_draw_seg_block(payload, w, by, bh, plot_w, plot_h, y_base, y_limit,
								      px_grid[prev_gi], py_grid[prev_gi], d_grid[prev_gi],
								      px_grid[gi], py_grid[gi], d_grid[gi],
								      c, zsub);
					}
					prev_ok = 1;
					prev_iy = iy;
					prev_gi = gi;
				}
			}
		}

		if (vec_fb_write_box(fb) != 0) {
			snprintf(err, errsz, "plot: fb write failed");
			free(zsub);
			free(px_grid);
			free(py_grid);
			free(d_grid);
			free(ok_grid);
			free(z01_grid);
			free(xN);
			free(yN);
			free(z_grid);
			return -1;
		}
	}

	free(zsub);
	free(px_grid);
	free(py_grid);
	free(d_grid);
	free(ok_grid);
	free(z01_grid);
	free(xN);
	free(yN);
	free(z_grid);

	if (!have_samples)
		return -1;
	return 0;
}
