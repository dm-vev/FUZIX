#include "vec_cas.h"
#include "vec_draw.h"
#include "vec_env.h"
#include "vec_eval.h"
#include "vec_fb.h"
#include "vec_keys.h"
#include "vec_parser.h"
#include "vec_plot.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

enum {
	MAX_OUTPUT_LINES = 200,
	MAX_HISTORY = 200,
	MAX_PLOTS = 8,
	MAX_PLOT_POINTS = 1024
};

enum {
	UI_DEFAULT_PLOT_DIM = 2,
	/* Spark defaults. */
	UI_DEFAULT_3D_PLOT_COLOR_MODE = 1,
};

enum vec_tab {
	TAB_TERMINAL = 0,
	TAB_PLOT = 1,
	TAB_STACK = 2,
};

enum ui_plot_kind {
	UI_PLOT_FUNC = 0,
	UI_PLOT_SERIES = 1,
};

struct ui_plot {
	enum ui_plot_kind kind;
	char src[128];
	vec_node *expr;
	float *xs;
	float *ys;
	size_t len;
};

struct ui_state {
	struct vec_fb fb;
	int kbdfd;
	struct termios kbd_saved;

	int cols;
	int rows;

	enum vec_tab tab;
	int show_help;
	int help_top;

	char message[64];
	char edit_var[32];

	char input[256];
	size_t input_len;
	size_t cursor;

	char *lines[MAX_OUTPUT_LINES];
	int line_count;

	char *history[MAX_HISTORY];
	int hist_count;
	int hist_pos;

	int stack_top;
	int stack_sel;

	char graph_src[128];
	vec_node *graph;

	struct ui_plot plots[MAX_PLOTS];
	int plot_count;

	int plot_dim;
	double plot_yaw;
	double plot_pitch;
	double plot_zoom;
	uint8_t plot_color_mode;
	int show_axes_3d;

	double x_min;
	double x_max;
	double y_min;
	double y_max;
	double zoom_in_factor;
	double zoom_out_factor;

	vec_env env;
};

static volatile sig_atomic_t running = 1;

static void on_sig(int sig)
{
	(void)sig;
	running = 0;
}

struct kbd_source {
	int fd;
	struct termios saved;
	int have_saved;
	int need_close;
};

static int ui_terminal_can_quit(const struct ui_state *u)
{
	return u && u->tab == TAB_TERMINAL && !u->edit_var[0] && !u->show_help && u->input_len == 0;
}

static void ui_set_message(struct ui_state *u, const char *s)
{
	if (!u)
		return;
	if (!s)
		s = "";
	snprintf(u->message, sizeof(u->message), "%s", s);
}

static void ui_plot_destroy(struct ui_plot *p)
{
	if (!p)
		return;
	vec_node_destroy(p->expr);
	free(p->xs);
	free(p->ys);
	memset(p, 0, sizeof(*p));
}

static void ui_plots_clear(struct ui_state *u)
{
	if (!u)
		return;
	for (int i = 0; i < u->plot_count; i++)
		ui_plot_destroy(&u->plots[i]);
	u->plot_count = 0;
}

static int ui_add_plot_func(struct ui_state *u, const char *src, const vec_node *expr)
{
	if (!u || !expr)
		return -1;
	if (!src)
		src = "<expr>";

	if (u->plot_count > 0 && !strcmp(u->plots[u->plot_count - 1].src, src))
		return 0;

	if (u->plot_count >= MAX_PLOTS) {
		ui_plot_destroy(&u->plots[0]);
		memmove(&u->plots[0], &u->plots[1], sizeof(u->plots[0]) * (MAX_PLOTS - 1));
		u->plot_count = MAX_PLOTS - 1;
	}

	vec_node *ex = vec_node_clone(expr);
	if (!ex)
		return -1;

	struct ui_plot *p = &u->plots[u->plot_count++];
	memset(p, 0, sizeof(*p));
	p->kind = UI_PLOT_FUNC;
	snprintf(p->src, sizeof(p->src), "%s", src);
	p->expr = ex;
	return 0;
}

static int series_copy_downsample(const double *xs, const double *ys, size_t n,
				  float **out_xs, float **out_ys, size_t *out_n)
{
	if (!out_xs || !out_ys || !out_n)
		return -1;
	*out_xs = NULL;
	*out_ys = NULL;
	*out_n = 0;
	if (!xs || !ys || n == 0)
		return -1;

	size_t step = 1;
	size_t cap = n;
	if (MAX_PLOT_POINTS > 1 && n > (size_t)MAX_PLOT_POINTS) {
		step = n / (size_t)MAX_PLOT_POINTS;
		if (step < 1)
			step = 1;
		cap = (size_t)MAX_PLOT_POINTS;
	}

	float *dx = malloc(sizeof(dx[0]) * cap);
	float *dy = malloc(sizeof(dy[0]) * cap);
	if (!dx || !dy) {
		free(dx);
		free(dy);
		return -1;
	}

	size_t m = 0;
	for (size_t i = 0; i < n && m < cap; i += step) {
		dx[m] = (float)xs[i];
		dy[m] = (float)ys[i];
		m++;
	}
	if (m == 0) {
		free(dx);
		free(dy);
		return -1;
	}
	dx[m - 1] = (float)xs[n - 1];
	dy[m - 1] = (float)ys[n - 1];

	*out_xs = dx;
	*out_ys = dy;
	*out_n = m;
	return 0;
}

static int matrix_copy_downsample(const double *mat, size_t rows,
				  float **out_xs, float **out_ys, size_t *out_n)
{
	if (!out_xs || !out_ys || !out_n)
		return -1;
	*out_xs = NULL;
	*out_ys = NULL;
	*out_n = 0;
	if (!mat || rows == 0)
		return -1;

	size_t step = 1;
	size_t cap = rows;
	if (MAX_PLOT_POINTS > 1 && rows > (size_t)MAX_PLOT_POINTS) {
		step = rows / (size_t)MAX_PLOT_POINTS;
		if (step < 1)
			step = 1;
		cap = (size_t)MAX_PLOT_POINTS;
	}

	float *dx = malloc(sizeof(dx[0]) * cap);
	float *dy = malloc(sizeof(dy[0]) * cap);
	if (!dx || !dy) {
		free(dx);
		free(dy);
		return -1;
	}

	size_t m = 0;
	for (size_t i = 0; i < rows && m < cap; i += step) {
		dx[m] = (float)mat[i * 2 + 0];
		dy[m] = (float)mat[i * 2 + 1];
		m++;
	}
	if (m == 0) {
		free(dx);
		free(dy);
		return -1;
	}
	dx[m - 1] = (float)mat[(rows - 1) * 2 + 0];
	dy[m - 1] = (float)mat[(rows - 1) * 2 + 1];

	*out_xs = dx;
	*out_ys = dy;
	*out_n = m;
	return 0;
}

static int ui_add_plot_series_owned(struct ui_state *u, const char *src,
				    float *xs, float *ys, size_t n)
{
	if (!u || !xs || !ys || n == 0) {
		free(xs);
		free(ys);
		return -1;
	}
	if (!src)
		src = "series";

	if (u->plot_count >= MAX_PLOTS) {
		ui_plot_destroy(&u->plots[0]);
		memmove(&u->plots[0], &u->plots[1], sizeof(u->plots[0]) * (MAX_PLOTS - 1));
		u->plot_count = MAX_PLOTS - 1;
	}

	struct ui_plot *p = &u->plots[u->plot_count++];
	memset(p, 0, sizeof(*p));
	p->kind = UI_PLOT_SERIES;
	snprintf(p->src, sizeof(p->src), "%s", src);
	p->xs = xs;
	p->ys = ys;
	p->len = n;
	return 0;
}

static int ui_add_plot_series(struct ui_state *u, const char *src,
			      const double *xs, const double *ys, size_t n)
{
	if (!u || !xs || !ys || n == 0)
		return -1;

	float *dx = NULL;
	float *dy = NULL;
	size_t m = 0;
	if (series_copy_downsample(xs, ys, n, &dx, &dy, &m) != 0)
		return -1;
	return ui_add_plot_series_owned(u, src, dx, dy, m);
}

static int ui_add_plot_matrix_xy(struct ui_state *u, const char *src, const vec_value *v)
{
	if (!u || !v || v->kind != VEC_VALUE_MATRIX || v->cols != 2 || v->rows <= 0 || !v->mat)
		return -1;

	float *dx = NULL;
	float *dy = NULL;
	size_t m = 0;
	if (matrix_copy_downsample(v->mat, (size_t)v->rows, &dx, &dy, &m) != 0)
		return -1;
	return ui_add_plot_series_owned(u, src, dx, dy, m);
}

static void ui_clip_cols(const struct ui_state *u, char *s, size_t ssz)
{
	if (!u || !s || ssz == 0)
		return;
	if (u->cols > 0 && (size_t)u->cols < ssz)
		s[u->cols] = 0;
}

static void ui_fmt_axis(double v, char *buf, size_t bufsz)
{
	if (!buf || bufsz == 0)
		return;
	if (!isfinite(v)) {
		buf[0] = 0;
		return;
	}
	if (fabs(v) < 1e-12) {
		snprintf(buf, bufsz, "0");
		return;
	}

	double av = fabs(v);
	if (av >= 1000 || av < 0.01)
		snprintf(buf, bufsz, "%.2g", v);
	else if (av >= 10)
		snprintf(buf, bufsz, "%.0f", v);
	else if (av >= 1)
		snprintf(buf, bufsz, "%.2f", v);
	else
		snprintf(buf, bufsz, "%.3f", v);
}

static void ui_header_text(struct ui_state *u, char *out, size_t outsz)
{
	if (!u || !out || outsz == 0)
		return;

	out[0] = 0;
	switch (u->tab) {
	case TAB_PLOT:
		if (!u->graph) {
			snprintf(out, outsz, "VECTOR plot | F1 term F2 plot* F3 stack");
			break;
		}
		{
			const char *expr = u->graph_src[0] ? u->graph_src : "<expr>";
			char head[128];
			snprintf(head, sizeof(head), "VECTOR plot | F1 term F2 plot* F3 stack | ");
			size_t hlen = strlen(head);
			if (hlen >= outsz) {
				snprintf(out, outsz, "%s", head);
				break;
			}
			snprintf(out, outsz, "%s%s", head, expr);
		}
		break;
	case TAB_STACK:
		snprintf(out, outsz, "VECTOR stack | F1 term F2 plot F3 stack*");
		break;
	case TAB_TERMINAL:
	default:
		snprintf(out, outsz, "VECTOR | F1 term* F2 plot F3 stack");
		break;
	}
	ui_clip_cols(u, out, outsz);
}

static void ui_status_text(struct ui_state *u, char *out, size_t outsz, int *cursor_col, int *cursor_on)
{
	if (!u || !out || outsz == 0)
		return;

	out[0] = 0;
	if (cursor_col)
		*cursor_col = -1;
	if (cursor_on)
		*cursor_on = 0;

	if (u->edit_var[0]) {
		char prefix[64];
		snprintf(prefix, sizeof(prefix), "edit %s = ", u->edit_var);
		size_t prefix_len = strlen(prefix);

		size_t visible = 0;
		if (u->cols > 0 && (size_t)u->cols > prefix_len)
			visible = (size_t)u->cols - prefix_len;

		size_t start = 0;
		if (visible > 0 && u->cursor > visible - 1) {
			start = u->cursor - (visible - 1);
		}
		if (start > u->input_len)
			start = u->input_len;

		snprintf(out, outsz, "%s%.*s", prefix, (int)visible, u->input + start);

		if (cursor_col && cursor_on && u->cols > 0) {
			int col = (int)prefix_len + (int)(u->cursor - start);
			if (col < (int)prefix_len)
				col = (int)prefix_len;
			if (col > u->cols - 1)
				col = u->cols - 1;
			*cursor_col = col;
			*cursor_on = 1;
		}

		if (u->message[0]) {
			size_t n = strlen(out);
			if (n + 3 < outsz)
				snprintf(out + n, outsz - n, " | %s", u->message);
		}
		ui_clip_cols(u, out, outsz);
		return;
	}

	switch (u->tab) {
		case TAB_PLOT: {
			char xmin[16], xmax[16], ymin[16], ymax[16];
			ui_fmt_axis(u->x_min, xmin, sizeof(xmin));
			ui_fmt_axis(u->x_max, xmax, sizeof(xmax));
			ui_fmt_axis(u->y_min, ymin, sizeof(ymin));
			ui_fmt_axis(u->y_max, ymax, sizeof(ymax));
			if (u->plot_dim == 3)
				snprintf(out, outsz, "3D x:[%s..%s] y:[%s..%s] zoom:%0.2f", xmin, xmax, ymin, ymax, u->plot_zoom);
			else
				snprintf(out, outsz, "x:[%s..%s] y:[%s..%s]", xmin, xmax, ymin, ymax);
			break;
		}
	case TAB_STACK:
		snprintf(out, outsz, "stack: Up/Down select | Enter edit | F1 term | F2 plot");
		break;
	case TAB_TERMINAL:
	default:
		snprintf(out, outsz, "%s", u->message);
		ui_clip_cols(u, out, outsz);
		return;
	}

	if (u->message[0]) {
		size_t n = strlen(out);
		if (n + 3 < outsz) {
			snprintf(out + n, outsz - n, " | %s", u->message);
		}
	}
	ui_clip_cols(u, out, outsz);
}

static void ui_append_line(struct ui_state *u, const char *s)
{
	if (!u || !s)
		return;
	char *dup = strdup(s);
	if (!dup)
		return;
	if (u->line_count >= MAX_OUTPUT_LINES) {
		free(u->lines[0]);
		memmove(&u->lines[0], &u->lines[1], sizeof(u->lines[0]) * (MAX_OUTPUT_LINES - 1));
		u->line_count = MAX_OUTPUT_LINES - 1;
	}
	u->lines[u->line_count++] = dup;
}

static const char *ui_format_value(struct ui_state *u, const vec_value *v, char *buf, size_t bufsz)
{
	if (!u || !v || !buf || bufsz == 0)
		return "";
	buf[0] = 0;

	switch (v->kind) {
	case VEC_VALUE_EXPR:
		if (v->expr) {
			(void)vec_node_to_string(v->expr, buf, bufsz);
			return buf;
		}
		return "<expr>";
	case VEC_VALUE_COMPLEX: {
		double re = v->c.re;
		double im = v->c.im;
		if (im == 0) {
			(void)vec_number_string(vec_float(re), u->env.prec, buf, bufsz);
			return buf;
		}
		if (re == 0) {
			char imbuf[64];
			(void)vec_number_string(vec_float(im), u->env.prec, imbuf, sizeof(imbuf));
			snprintf(buf, bufsz, "%si", imbuf);
			return buf;
		}

		char rebuf[64], imbuf[64];
		(void)vec_number_string(vec_float(re), u->env.prec, rebuf, sizeof(rebuf));
		(void)vec_number_string(vec_float(im), u->env.prec, imbuf, sizeof(imbuf));
		if (im > 0 && imbuf[0] != '+') {
			char tmp[64];
			snprintf(tmp, sizeof(tmp), "+%s", imbuf);
			snprintf(imbuf, sizeof(imbuf), "%s", tmp);
		}
		snprintf(buf, bufsz, "%s%si", rebuf, imbuf);
		return buf;
	}
	case VEC_VALUE_ARRAY:
		if (!v->len)
			return "[]";
		{
			double min = v->arr[0];
			double max = v->arr[0];
			for (size_t i = 1; i < v->len; i++) {
				double x = v->arr[i];
				if (x < min)
					min = x;
				if (x > max)
					max = x;
			}
			char minbuf[24], maxbuf[24];
			(void)vec_number_string(vec_float(min), 6, minbuf, sizeof(minbuf));
			(void)vec_number_string(vec_float(max), 6, maxbuf, sizeof(maxbuf));
			snprintf(buf, bufsz, "[%lu] %s..%s", (unsigned long)v->len, minbuf, maxbuf);
			return buf;
		}
	case VEC_VALUE_MATRIX:
		if (!v->mat || v->rows <= 0 || v->cols <= 0)
			return "[?]";
		{
			size_t n = (size_t)v->rows * (size_t)v->cols;
			if (!n)
				return "[?]";
			double min = v->mat[0];
			double max = v->mat[0];
			for (size_t i = 1; i < n; i++) {
				double x = v->mat[i];
				if (x < min)
					min = x;
				if (x > max)
					max = x;
			}
			char minbuf[24], maxbuf[24];
			(void)vec_number_string(vec_float(min), 6, minbuf, sizeof(minbuf));
			(void)vec_number_string(vec_float(max), 6, maxbuf, sizeof(maxbuf));
			snprintf(buf, bufsz, "[%dx%d] %s..%s", v->rows, v->cols, minbuf, maxbuf);
			return buf;
		}
	case VEC_VALUE_NUMBER:
	default:
		return vec_number_string(v->num, u->env.prec, buf, bufsz);
	}
}

static const char *ui_value_edit_string(struct ui_state *u, const vec_value *v, char *buf, size_t bufsz)
{
	if (!u || !v || !buf || bufsz == 0)
		return "";
	buf[0] = 0;

	switch (v->kind) {
	case VEC_VALUE_EXPR:
		if (v->expr) {
			(void)vec_node_to_string(v->expr, buf, bufsz);
			return buf;
		}
		return "";
	case VEC_VALUE_ARRAY:
		return "";
	case VEC_VALUE_COMPLEX: {
		double re = v->c.re;
		double im = v->c.im;
		if (im == 0)
			return vec_number_string(vec_float(re), u->env.prec, buf, bufsz);
		if (re == 0) {
			char imbuf[64];
			(void)vec_number_string(vec_float(im), u->env.prec, imbuf, sizeof(imbuf));
			snprintf(buf, bufsz, "%s*i", imbuf);
			return buf;
		}
		char rebuf[64], imbuf[64];
		(void)vec_number_string(vec_float(re), u->env.prec, rebuf, sizeof(rebuf));
		if (im < 0) {
			(void)vec_number_string(vec_float(-im), u->env.prec, imbuf, sizeof(imbuf));
			snprintf(buf, bufsz, "%s-%s*i", rebuf, imbuf);
		} else {
			(void)vec_number_string(vec_float(im), u->env.prec, imbuf, sizeof(imbuf));
			snprintf(buf, bufsz, "%s+%s*i", rebuf, imbuf);
		}
		return buf;
	}
	case VEC_VALUE_NUMBER:
	default:
		return vec_number_string(v->num, u->env.prec, buf, bufsz);
	}
}

static void ui_history_push(struct ui_state *u, const char *s)
{
	if (!u || !s || !*s)
		return;
	char *dup = strdup(s);
	if (!dup)
		return;
	if (u->hist_count >= MAX_HISTORY) {
		free(u->history[0]);
		memmove(&u->history[0], &u->history[1], sizeof(u->history[0]) * (MAX_HISTORY - 1));
		u->hist_count = MAX_HISTORY - 1;
	}
	u->history[u->hist_count++] = dup;
	u->hist_pos = u->hist_count;
}

static void ui_set_input(struct ui_state *u, const char *s)
{
	if (!u)
		return;
	if (!s)
		s = "";
	snprintf(u->input, sizeof(u->input), "%s", s);
	u->input_len = strlen(u->input);
	u->cursor = u->input_len;
}

static void ui_hist_up(struct ui_state *u)
{
	if (!u || u->hist_count == 0)
		return;
	if (u->hist_pos > 0)
		u->hist_pos--;
	ui_set_input(u, u->history[u->hist_pos]);
}

static void ui_hist_down(struct ui_state *u)
{
	if (!u || u->hist_count == 0)
		return;
	if (u->hist_pos < u->hist_count - 1) {
		u->hist_pos++;
		ui_set_input(u, u->history[u->hist_pos]);
		return;
	}
	u->hist_pos = u->hist_count;
	ui_set_input(u, "");
}

static void ui_insert_char(struct ui_state *u, int ch)
{
	if (!u)
		return;
	if (u->input_len + 1 >= sizeof(u->input))
		return;
	if (u->cursor > u->input_len)
		u->cursor = u->input_len;
	memmove(&u->input[u->cursor + 1], &u->input[u->cursor], u->input_len - u->cursor + 1);
	u->input[u->cursor] = (char)ch;
	u->input_len++;
	u->cursor++;
}

static void ui_backspace(struct ui_state *u)
{
	if (!u || u->cursor == 0 || u->input_len == 0)
		return;
	memmove(&u->input[u->cursor - 1], &u->input[u->cursor], u->input_len - u->cursor + 1);
	u->input_len--;
	u->cursor--;
}

static void ui_delete(struct ui_state *u)
{
	if (!u || u->cursor >= u->input_len)
		return;
	memmove(&u->input[u->cursor], &u->input[u->cursor + 1], u->input_len - u->cursor);
	u->input_len--;
}

static void ui_switch_tab(struct ui_state *u, enum vec_tab tab);

static vec_var *ui_stack_var_at(struct ui_state *u, int idx)
{
	if (!u)
		return NULL;
	int i = 0;
	for (vec_var *v = u->env.vars; v; v = v->next, i++) {
		if (i == idx)
			return v;
	}
	return NULL;
}

static int ui_stack_var_count(struct ui_state *u)
{
	if (!u)
		return 0;
	int total = 0;
	for (vec_var *v = u->env.vars; v; v = v->next)
		total++;
	return total;
}

static void ui_start_edit_var(struct ui_state *u, const char *name, const vec_value *v)
{
	if (!u || !name || !*name)
		return;

	snprintf(u->edit_var, sizeof(u->edit_var), "%s", name);
	char buf[160];
	ui_set_input(u, v ? ui_value_edit_string(u, v, buf, sizeof(buf)) : "");
	ui_set_message(u, "Enter apply | Esc cancel");
}

static void ui_cancel_edit(struct ui_state *u)
{
	if (!u)
		return;
	u->edit_var[0] = 0;
	ui_set_input(u, "");
}

static void ui_normalize_view(struct ui_state *u)
{
	if (!u)
		return;
	if (!isfinite(u->x_min) || !isfinite(u->x_max) || u->x_min >= u->x_max) {
		u->x_min = -10;
		u->x_max = 10;
	}
	if (!isfinite(u->y_min) || !isfinite(u->y_max) || u->y_min >= u->y_max) {
		u->y_min = -10;
		u->y_max = 10;
	}
}

static void ui_cycle_plot_zoom(struct ui_state *u)
{
	if (!u)
		return;
	if (u->zoom_in_factor >= 0.89) {
		u->zoom_in_factor = 0.8;
		u->zoom_out_factor = 1.25;
	} else if (u->zoom_in_factor >= 0.79) {
		u->zoom_in_factor = 0.5;
		u->zoom_out_factor = 2.0;
	} else {
		u->zoom_in_factor = 0.9;
		u->zoom_out_factor = 1.0 / 0.9;
	}
	char msg[64];
	snprintf(msg, sizeof(msg), "zoom: in x%.2f out x%.2f", u->zoom_in_factor, u->zoom_out_factor);
	ui_set_message(u, msg);
}

static void ui_plot_pan(struct ui_state *u, double dx_frac, double dy_frac)
{
	if (!u)
		return;
	double dx = (u->x_max - u->x_min) * dx_frac;
	double dy = (u->y_max - u->y_min) * dy_frac;
	u->x_min += dx;
	u->x_max += dx;
	u->y_min += dy;
	u->y_max += dy;
	ui_normalize_view(u);
}

static void ui_plot_zoom(struct ui_state *u, double factor)
{
	if (!u)
		return;
	double cx = (u->x_min + u->x_max) * 0.5;
	double cy = (u->y_min + u->y_max) * 0.5;
	double hx = (u->x_max - u->x_min) * 0.5 * factor;
	double hy = (u->y_max - u->y_min) * 0.5 * factor;
	if (hx <= 0 || hy <= 0)
		return;
	u->x_min = cx - hx;
	u->x_max = cx + hx;
	u->y_min = cy - hy;
	u->y_max = cy + hy;
	ui_normalize_view(u);
}

static double clamp_double(double v, double lo, double hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void ui_reset_3d_view(struct ui_state *u)
{
	if (!u)
		return;
	u->plot_yaw = 0.8;
	u->plot_pitch = 0.85;
	u->plot_zoom = 1.1;
	if (u->plot_color_mode > 2)
		u->plot_color_mode = UI_DEFAULT_3D_PLOT_COLOR_MODE;
}

static void ui_plot_zoom_3d(struct ui_state *u, double factor)
{
	if (!u)
		return;
	if (factor <= 0 || isnan(factor) || isinf(factor))
		return;
	double z = u->plot_zoom / factor;
	z = clamp_double(z, 0.2, 20.0);
	u->plot_zoom = z;
}

static void ui_set_domain_from_array(struct ui_state *u, const vec_value *v)
{
	if (!u || !v || v->kind != VEC_VALUE_ARRAY || !v->arr || v->len < 2)
		return;

	double min = v->arr[0];
	double max = v->arr[0];
	for (size_t i = 1; i < v->len; i++) {
		double x = v->arr[i];
		if (x < min)
			min = x;
		if (x > max)
			max = x;
	}
	if (min < max) {
		u->x_min = min;
		u->x_max = max;
		ui_normalize_view(u);
	}
}

static void ui_set_range_from_array(struct ui_state *u, const vec_value *v)
{
	if (!u || !v || v->kind != VEC_VALUE_ARRAY || !v->arr || v->len < 2)
		return;

	double min = v->arr[0];
	double max = v->arr[0];
	for (size_t i = 1; i < v->len; i++) {
		double y = v->arr[i];
		if (y < min)
			min = y;
		if (y > max)
			max = y;
	}
	if (min < max) {
		u->y_min = min;
		u->y_max = max;
		ui_normalize_view(u);
	}
}

static void ui_autoscale_from_series(struct ui_state *u)
{
	if (!u || u->plot_count <= 0)
		return;

	double minx = DBL_MAX;
	double maxx = -DBL_MAX;
	double miny = DBL_MAX;
	double maxy = -DBL_MAX;

	for (int i = 0; i < u->plot_count; i++) {
		const struct ui_plot *p = &u->plots[i];
		if (!p || p->kind != UI_PLOT_SERIES || !p->xs || !p->ys || p->len == 0)
			continue;
		for (size_t j = 0; j < p->len; j++) {
			double x = (double)p->xs[j];
			double y = (double)p->ys[j];
			if (!isfinite(x) || !isfinite(y))
				continue;
			if (x < minx)
				minx = x;
			if (x > maxx)
				maxx = x;
			if (y < miny)
				miny = y;
			if (y > maxy)
				maxy = y;
		}
	}

	if (minx < DBL_MAX && maxx > -DBL_MAX && minx < maxx) {
		u->x_min = minx;
		u->x_max = maxx;
	}
	if (miny < DBL_MAX && maxy > -DBL_MAX && miny < maxy) {
		double pad = (maxy - miny) * 0.1;
		if (pad == 0)
			pad = 1;
		u->y_min = miny - pad;
		u->y_max = maxy + pad;
	}
	ui_normalize_view(u);
}

static void ui_autoscale_func(struct ui_state *u)
{
	if (!u || !u->graph || u->x_min >= u->x_max)
		return;

	double miny = DBL_MAX;
	double maxy = -DBL_MAX;
	for (int i = 0; i < 240; i++) {
		double x = u->x_min + ((double)i / 239.0) * (u->x_max - u->x_min);
		vec_value v;
		char ebuf[64];
		memset(&v, 0, sizeof(v));
		ebuf[0] = 0;
		if (vec_eval_node_override(&u->env, u->graph, "x", vec_value_number(vec_float(x)),
					   &v, ebuf, sizeof(ebuf)) != 0)
			continue;
		if (v.kind != VEC_VALUE_NUMBER)
			continue;
		double y = vec_number_float64(v.num);
		if (!isfinite(y))
			continue;
		if (y < miny)
			miny = y;
		if (y > maxy)
			maxy = y;
	}
	if (miny < DBL_MAX && maxy > -DBL_MAX && miny < maxy) {
		double pad = (maxy - miny) * 0.1;
		if (pad == 0)
			pad = 1;
		u->y_min = miny - pad;
		u->y_max = maxy + pad;
		ui_normalize_view(u);
	}
}

static void ui_autoscale_plots(struct ui_state *u)
{
	if (!u)
		return;
	if (u->plot_count > 0) {
		ui_autoscale_from_series(u);
		return;
	}

	if (u->graph) {
		vec_value gv;
		memset(&gv, 0, sizeof(gv));
		char gerr[96];
		gerr[0] = 0;
		if (vec_eval_node(&u->env, u->graph, &gv, gerr, sizeof(gerr)) == 0 &&
		    gv.kind == VEC_VALUE_MATRIX && gv.cols == 2 && gv.rows > 0 && gv.mat) {
			double minx = DBL_MAX;
			double maxx = -DBL_MAX;
			double miny = DBL_MAX;
			double maxy = -DBL_MAX;

			for (int i = 0; i < gv.rows; i++) {
				double x = gv.mat[i * 2 + 0];
				double y = gv.mat[i * 2 + 1];
				if (!isfinite(x) || !isfinite(y))
					continue;
				if (x < minx)
					minx = x;
				if (x > maxx)
					maxx = x;
				if (y < miny)
					miny = y;
				if (y > maxy)
					maxy = y;
			}

			if (minx < DBL_MAX && maxx > -DBL_MAX && minx < maxx) {
				u->x_min = minx;
				u->x_max = maxx;
			}
			if (miny < DBL_MAX && maxy > -DBL_MAX && miny < maxy) {
				double pad = (maxy - miny) * 0.1;
				if (pad == 0)
					pad = 1;
				u->y_min = miny - pad;
				u->y_max = maxy + pad;
			}
			ui_normalize_view(u);
			vec_value_destroy(&gv);
			return;
		}
		vec_value_destroy(&gv);
	}
	ui_autoscale_func(u);
}

static void ui_set_graph_take(struct ui_state *u, const char *src, vec_node *expr)
{
	if (!u) {
		vec_node_destroy(expr);
		return;
	}
	if (u->graph)
		vec_node_destroy(u->graph);
	u->graph = expr;
	snprintf(u->graph_src, sizeof(u->graph_src), "%s", src ? src : "");

	if (u->plot_dim != 3 && u->graph && vec_node_has_ident(u->graph, "x"))
		ui_autoscale_plots(u);
}

static void ui_set_graph_clone(struct ui_state *u, const char *src, const vec_node *expr)
{
	if (!u)
		return;
	vec_node *cpy = expr ? vec_node_clone(expr) : NULL;
	if (expr && !cpy) {
		ui_set_message(u, "eval: out of memory");
		return;
	}
	ui_set_graph_take(u, src, cpy);
}

static void ui_try_plot_series(struct ui_state *u, const char *label, const vec_value *v)
{
	if (!u || !label || !v)
		return;
	if (!strcmp(label, "x"))
		return;
	if (v->kind != VEC_VALUE_ARRAY || !v->arr || v->len == 0)
		return;

	vec_value xv;
	memset(&xv, 0, sizeof(xv));
	if (vec_env_get_var(&u->env, "x", &xv) != 0)
		return;
	if (xv.kind != VEC_VALUE_ARRAY || !xv.arr || xv.len != v->len) {
		vec_value_destroy(&xv);
		return;
	}

	if (ui_add_plot_series(u, label, xv.arr, v->arr, v->len) == 0)
		ui_autoscale_plots(u);
	vec_value_destroy(&xv);
}

static void ui_try_plot_matrix_xy(struct ui_state *u, const char *label, const vec_value *v)
{
	if (!u || !label || !v)
		return;
	if (v->kind != VEC_VALUE_MATRIX || v->cols != 2 || v->rows <= 0 || !v->mat)
		return;
	if (ui_add_plot_matrix_xy(u, label, v) == 0)
		ui_autoscale_plots(u);
}

static void ui_force_plot(struct ui_state *u)
{
	if (!u)
		return;

	/* 1) Prefer existing graph expression. */
	if (u->graph && vec_node_has_ident(u->graph, "x")) {
		const char *src = u->graph_src[0] ? u->graph_src : "<expr>";
		(void)ui_add_plot_func(u, src, u->graph);
		return;
	}

	/* 2) If y is defined as an expression, plot it as y(x). */
	vec_value yv;
	memset(&yv, 0, sizeof(yv));
	if (vec_env_get_var(&u->env, "y", &yv) == 0) {
		if (yv.kind == VEC_VALUE_EXPR && yv.expr && vec_node_has_ident(yv.expr, "x")) {
			char exbuf[128];
			exbuf[0] = 0;
			(void)vec_node_to_string(yv.expr, exbuf, sizeof(exbuf));
			char src[160];
			snprintf(src, sizeof(src), "y = %s", exbuf[0] ? exbuf : "<expr>");
			ui_set_graph_clone(u, src, yv.expr);
			(void)ui_add_plot_func(u, src, yv.expr);
			vec_value_destroy(&yv);
			return;
		}
		vec_value_destroy(&yv);
	}

	/* 3) If y is an array and x is an array, plot a series. */
	memset(&yv, 0, sizeof(yv));
	if (vec_env_get_var(&u->env, "y", &yv) == 0) {
		if (yv.kind == VEC_VALUE_ARRAY)
			ui_try_plot_series(u, "y", &yv);
		vec_value_destroy(&yv);
	}
}

static void ui_set_plot_tab_message(struct ui_state *u)
{
	if (!u)
		return;
	if (u->plot_dim == 3) {
		ui_set_message(u, "3D: arrows rotate | +/- zoom | PgUp/PgDn zoom | Tab axes | z zoom step | a autoscale | c term | $plotdim 2");
	} else {
		ui_set_message(u, "arrows pan | +/- zoom | PgUp/PgDn zoom | z zoom step | a autoscale | c term");
	}
}

static void ui_handle_service_command(struct ui_state *u, const char *cmdline)
{
	if (!u || !cmdline)
		return;

	char buf[128];
	snprintf(buf, sizeof(buf), "%s", cmdline);

	char *fields[3];
	int n = 0;
	char *p = buf;
	while (*p && n < (int)(sizeof(fields) / sizeof(fields[0]))) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		fields[n++] = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (*p)
			*p++ = 0;
	}
	if (n == 0)
		return;

	const char *cmd = fields[0];
	if (!strcmp(cmd, "plotdim")) {
		if (n != 2) {
			ui_set_message(u, "usage: $plotdim 2|3");
			return;
		}
		int dim = atoi(fields[1]);
		if (dim != 2 && dim != 3) {
			ui_set_message(u, "plotdim: expected 2 or 3");
			return;
		}
		u->plot_dim = dim;
		if (dim == 3)
			ui_reset_3d_view(u);
		if (u->tab == TAB_PLOT)
			ui_set_plot_tab_message(u);
		else {
			char msg[32];
			snprintf(msg, sizeof(msg), "plotdim: %d", dim);
			ui_set_message(u, msg);
		}
		return;
	}

	if (!strcmp(cmd, "plotcolor")) {
		if (n == 1) {
			u->plot_color_mode = (uint8_t)((u->plot_color_mode + 1) % 3);
		} else if (n == 2) {
			if (!strcmp(fields[1], "0") || !strcmp(fields[1], "mono")) {
				u->plot_color_mode = 0;
			} else if (!strcmp(fields[1], "1") || !strcmp(fields[1], "height")) {
				u->plot_color_mode = 1;
			} else if (!strcmp(fields[1], "2") || !strcmp(fields[1], "pos") || !strcmp(fields[1], "position")) {
				u->plot_color_mode = 2;
			} else {
				ui_set_message(u, "plotcolor: expected 0|1|2 or mono|height|pos");
				return;
			}
		} else {
			ui_set_message(u, "usage: $plotcolor [0|1|2]");
			return;
		}
		const char *name = "mono";
		if (u->plot_color_mode == 1)
			name = "height";
		else if (u->plot_color_mode == 2)
			name = "pos";
		{
			char msg[48];
			snprintf(msg, sizeof(msg), "plotcolor: %s", name);
			ui_set_message(u, msg);
		}
		return;
	}

	if (!strcmp(cmd, "resetview")) {
		u->x_min = -10;
		u->x_max = 10;
		u->y_min = -10;
		u->y_max = 10;
		ui_normalize_view(u);
		ui_reset_3d_view(u);
		ui_set_message(u, "view reset");
		return;
	}

	if (!strcmp(cmd, "autoscale")) {
		if (u->plot_count == 0 && !u->graph) {
			ui_set_message(u, "autoscale: no plot");
			return;
		}
		ui_autoscale_plots(u);
		ui_set_message(u, "autoscale");
		return;
	}

	ui_set_message(u, "unknown service command");
}

static void ui_handle_command(struct ui_state *u, const char *cmdline)
{
	if (!u || !cmdline)
		return;

	if (!strncmp(cmdline, "plot", 4) && (cmdline[4] == 0 || isspace((unsigned char)cmdline[4]))) {
		const char *expr = cmdline + 4;
		while (*expr == ' ')
			expr++;
		if (!*expr) {
			ui_force_plot(u);
			ui_switch_tab(u, TAB_PLOT);
			return;
		}
		if (*expr) {
			char err[128];
			memset(err, 0, sizeof(err));
			vec_actions acts;
			if (vec_parse_input(expr, &acts, err, sizeof(err)) != 0) {
				ui_set_message(u, err[0] ? err : "plot: parse error");
				return;
			}
			if (acts.count != 1 || acts.items[0].kind != VEC_ACT_EVAL || !acts.items[0].expr) {
				vec_actions_destroy(&acts);
				ui_set_message(u, "plot: expected single expression");
				return;
			}
			ui_set_graph_take(u, expr, acts.items[0].expr);
			acts.items[0].expr = NULL;
			vec_actions_destroy(&acts);
			ui_set_message(u, "plot updated");
		}
		if (!u->graph && !*expr)
			ui_set_message(u, "plot: no expression set");
		ui_switch_tab(u, TAB_PLOT);
		return;
	}
	if (!strcmp(cmdline, "plotclear")) {
		ui_plots_clear(u);
		ui_set_message(u, "plots cleared");
		return;
	}
	if (!strcmp(cmdline, "plots")) {
		if (u->plot_count == 0) {
			ui_append_line(u, "plots: (none)");
			return;
		}
		char line[196];
		for (int i = 0; i < u->plot_count; i++) {
			const char *src = u->plots[i].src[0] ? u->plots[i].src : "<expr>";
			snprintf(line, sizeof(line), "plot[%d]: %s", i, src);
			ui_append_line(u, line);
		}
		return;
	}
	if (!strncmp(cmdline, "plotdel", 6) && (cmdline[6] == 0 || isspace((unsigned char)cmdline[6]))) {
		const char *p = cmdline + 6;
		while (*p == ' ')
			p++;
		if (!*p) {
			ui_set_message(u, "usage: :plotdel N");
			return;
		}
		int idx = atoi(p);
		if (idx < 0 || idx >= u->plot_count) {
			int alt = idx - 1;
			if (alt >= 0 && alt < u->plot_count)
				idx = alt;
		}
		if (idx < 0 || idx >= u->plot_count) {
			ui_set_message(u, "plot index out of range");
			return;
		}
		ui_plot_destroy(&u->plots[idx]);
		if (idx + 1 < u->plot_count)
			memmove(&u->plots[idx], &u->plots[idx + 1], sizeof(u->plots[idx]) * (size_t)(u->plot_count - idx - 1));
		u->plot_count--;
		ui_set_message(u, "plot deleted");
		return;
	}
	if (!strcmp(cmdline, "help")) {
		u->show_help = !u->show_help;
		u->help_top = 0;
		ui_set_message(u, u->show_help ? "help: on" : "help: off");
		return;
	}
	if (!strcmp(cmdline, "term")) {
		ui_switch_tab(u, TAB_TERMINAL);
		return;
	}
	if (!strcmp(cmdline, "stack")) {
		ui_switch_tab(u, TAB_STACK);
		return;
	}
	if (!strcmp(cmdline, "resetview")) {
		u->x_min = -10;
		u->x_max = 10;
		u->y_min = -10;
		u->y_max = 10;
		u->zoom_in_factor = 0.8;
		u->zoom_out_factor = 1.25;
		ui_set_message(u, "view reset");
		return;
	}
	if (!strncmp(cmdline, "x ", 2)) {
		char *endp = NULL;
		double a = strtod(cmdline + 2, &endp);
		double b = endp ? strtod(endp, &endp) : 0;
		if (!isfinite(a) || !isfinite(b) || a >= b) {
			ui_set_message(u, "usage: :x A B (A<B)");
			return;
		}
		u->x_min = a;
		u->x_max = b;
		ui_normalize_view(u);
		ui_set_message(u, "x updated");
		return;
	}
	if (!strncmp(cmdline, "y ", 2)) {
		char *endp = NULL;
		double a = strtod(cmdline + 2, &endp);
		double b = endp ? strtod(endp, &endp) : 0;
		if (!isfinite(a) || !isfinite(b) || a >= b) {
			ui_set_message(u, "usage: :y A B (A<B)");
			return;
		}
		u->y_min = a;
		u->y_max = b;
		ui_normalize_view(u);
		ui_set_message(u, "y updated");
		return;
	}
	if (!strncmp(cmdline, "view ", 5)) {
		char *endp = NULL;
		double xmin = strtod(cmdline + 5, &endp);
		double xmax = endp ? strtod(endp, &endp) : 0;
		double ymin = endp ? strtod(endp, &endp) : 0;
		double ymax = endp ? strtod(endp, &endp) : 0;
		if (!isfinite(xmin) || !isfinite(xmax) || !isfinite(ymin) || !isfinite(ymax) ||
		    xmin >= xmax || ymin >= ymax) {
			ui_set_message(u, "usage: :view xmin xmax ymin ymax");
			return;
		}
		u->x_min = xmin;
		u->x_max = xmax;
		u->y_min = ymin;
		u->y_max = ymax;
		ui_normalize_view(u);
		ui_set_message(u, "view updated");
		return;
	}
	if (!strcmp(cmdline, "autoscale")) {
		if (u->plot_count == 0 && !u->graph) {
			ui_set_message(u, "autoscale: no plot");
			return;
		}
		ui_autoscale_plots(u);
		ui_set_message(u, "autoscale");
		return;
	}
	if (!strcmp(cmdline, "clear")) {
		for (int i = 0; i < u->line_count; i++)
			free(u->lines[i]);
		u->line_count = 0;
		ui_set_message(u, "cleared");
		return;
	}
	if (!strcmp(cmdline, "exact")) {
#ifdef VEC_LITE
		ui_set_message(u, "mode: exact disabled");
#else
		u->env.mode = VEC_MODE_EXACT;
		ui_set_message(u, "mode: exact");
#endif
		return;
	}
	if (!strcmp(cmdline, "float")) {
		u->env.mode = VEC_MODE_FLOAT;
		ui_set_message(u, "mode: float");
		return;
	}
	if (!strncmp(cmdline, "prec", 4)) {
		const char *p = cmdline + 4;
		while (*p == ' ')
			p++;
		int v = atoi(p);
		if (v < 1 || v > 32) {
			ui_set_message(u, "prec: 1..32");
			return;
		}
		u->env.prec = v;
		ui_set_message(u, "prec updated");
		return;
	}
	ui_set_message(u, "unknown command");
}

static void ui_eval_line(struct ui_state *u, const char *line)
{
	char err[128];
	if (!u || !line)
		return;

	while (*line && isspace((unsigned char)*line))
		line++;
	if (!*line)
		return;

	ui_history_push(u, line);
	{
		char tmp[280];
		snprintf(tmp, sizeof(tmp), "V> %s", line);
		ui_append_line(u, tmp);
	}

	if (line[0] == ':') {
		ui_handle_command(u, line + 1);
		return;
	}
	if (line[0] == '$') {
		ui_handle_service_command(u, line + 1);
		return;
	}

	memset(err, 0, sizeof(err));
	vec_actions acts;
	if (vec_parse_input(line, &acts, err, sizeof(err)) != 0) {
		ui_append_line(u, err[0] ? err : "parse error");
		return;
	}

	for (size_t i = 0; i < acts.count; i++) {
		vec_action *a = &acts.items[i];
		if (a->kind == VEC_ACT_ASSIGN_VAR) {
			vec_value v;
			memset(err, 0, sizeof(err));
				if (vec_eval_node(&u->env, a->expr, &v, err, sizeof(err)) != 0) {
					if (!strncmp(err, "eval: unknown variable", 22) &&
					    a->var_name && (!strcmp(a->var_name, "y") || !strcmp(a->var_name, "z")) &&
					    a->expr && vec_node_has_ident(a->expr, "x")) {
						vec_node *expr = vec_node_clone(a->expr);
						if (!expr) {
							ui_append_line(u, "eval: out of memory");
							continue;
						}
						vec_value ev = vec_value_expr(expr);
						if (vec_env_set_var(&u->env, a->var_name, ev) != 0) {
							ui_append_line(u, "eval: out of memory");
							vec_value_destroy(&ev);
							continue;
						}
					{
						char buf[160];
						char out[160];
						snprintf(out, sizeof(out), "%s = %s", a->var_name,
							 ui_format_value(u, &ev, buf, sizeof(buf)));
						ui_append_line(u, out);
					}
						{
							char exbuf[128];
							exbuf[0] = 0;
							(void)vec_node_to_string(expr, exbuf, sizeof(exbuf));
							char src[160];
							snprintf(src, sizeof(src), "%s = %s", a->var_name, exbuf[0] ? exbuf : "<expr>");
							ui_set_graph_clone(u, src, expr);
							if (vec_node_has_ident(expr, "x"))
								(void)ui_add_plot_func(u, src, expr);
						}
						continue;
					}
					ui_append_line(u, err[0] ? err : "eval error");
				continue;
			}
			if (vec_env_set_var(&u->env, a->var_name, v) != 0) {
				ui_append_line(u, "eval: out of memory");
				vec_value_destroy(&v);
				continue;
			}
			{
				char buf[160];
				char out[128];
				snprintf(out, sizeof(out), "%s = %s", a->var_name, ui_format_value(u, &v, buf, sizeof(buf)));
				ui_append_line(u, out);
			}
			if (v.kind == VEC_VALUE_ARRAY && a->var_name) {
				if (!strcmp(a->var_name, "x")) {
					ui_set_domain_from_array(u, &v);
				} else if (!strcmp(a->var_name, "y")) {
					ui_set_range_from_array(u, &v);
					ui_try_plot_series(u, "y", &v);
				} else {
					ui_try_plot_series(u, a->var_name, &v);
				}
			} else if (v.kind == VEC_VALUE_MATRIX && v.cols == 2 && a->var_name) {
				ui_try_plot_matrix_xy(u, a->var_name, &v);
			} else if (v.kind == VEC_VALUE_EXPR && v.expr && a->var_name) {
				ui_set_graph_clone(u, a->var_name, v.expr);
			} else if (v.kind == VEC_VALUE_NUMBER && a->var_name) {
				vec_node *nn = vec_node_number_new(v.num);
				if (nn)
					ui_set_graph_take(u, a->var_name, nn);
			}
			continue;
		}
		if (a->kind == VEC_ACT_ASSIGN_FUNC) {
			if (vec_env_set_func(&u->env, a->func_name, a->func_param, a->expr) != 0) {
				ui_append_line(u, "eval: out of memory");
				continue;
			}
			a->expr = NULL; /* ownership transferred */
			{
				char out[128];
				snprintf(out, sizeof(out), "%s(%s) = <expr>", a->func_name, a->func_param);
				ui_append_line(u, out);
			}
			continue;
		}

		vec_value v;
		memset(err, 0, sizeof(err));
		if (vec_eval_node(&u->env, a->expr, &v, err, sizeof(err)) != 0) {
			if (!strncmp(err, "eval: unknown variable", 22) &&
			    a->expr && (vec_node_has_ident(a->expr, "x") || vec_node_has_ident(a->expr, "y"))) {
				char src[160];
				src[0] = 0;
				(void)vec_node_to_string(a->expr, src, sizeof(src));
				ui_set_graph_take(u, src[0] ? src : "<expr>", a->expr);
				a->expr = NULL;

				char out[192];
				snprintf(out, sizeof(out), "= %s", src[0] ? src : "<expr>");
				ui_append_line(u, out);
				continue;
			}
			ui_append_line(u, err[0] ? err : "eval error");
			continue;
		}
		{
			char buf[160];
			char out[192];
			snprintf(out, sizeof(out), "= %s", ui_format_value(u, &v, buf, sizeof(buf)));
			ui_append_line(u, out);
		}
		if (v.kind == VEC_VALUE_ARRAY) {
			ui_try_plot_series(u, "result", &v);
		} else if (v.kind == VEC_VALUE_MATRIX && v.cols == 2) {
			ui_try_plot_matrix_xy(u, "result", &v);
		} else if (a->expr) {
			char src[160];
			src[0] = 0;
			(void)vec_node_to_string(a->expr, src, sizeof(src));

			if (v.kind == VEC_VALUE_EXPR && v.expr) {
				ui_set_graph_take(u, src[0] ? src : "<expr>", v.expr);
				v.expr = NULL;
			} else if (v.kind == VEC_VALUE_NUMBER) {
				vec_node *nn = vec_node_number_new(v.num);
				if (nn)
					ui_set_graph_take(u, src[0] ? src : "<expr>", nn);
			} else {
				ui_set_graph_clone(u, src[0] ? src : "<expr>", a->expr);
			}

			if (vec_node_has_ident(a->expr, "x"))
				(void)ui_add_plot_func(u, src[0] ? src : "<expr>", a->expr);
		}
		vec_value_destroy(&v);
	}

	vec_actions_destroy(&acts);
}

static void ui_submit(struct ui_state *u)
{
	if (!u)
		return;
	char line[256];
	snprintf(line, sizeof(line), "%s", u->input);
	ui_set_input(u, "");
	ui_eval_line(u, line);
}

static int open_kbd(struct ui_state *u, const char *path)
{
	if (!u || !path)
		return -1;

	int fd = -1;
	if (!strcmp(path, "-")) {
		fd = 0;
	} else {
		fd = open(path, O_RDWR | O_NOCTTY);
	}
	if (fd < 0)
		return -1;
	if (tcgetattr(fd, &u->kbd_saved) == 0) {
		struct termios t = u->kbd_saved;
		cfmakeraw(&t);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 1;
		tcsetattr(fd, TCSANOW, &t);
	}
	return fd;
}

static int kbd_setup_fd(struct kbd_source *ks, int fd, int need_close)
{
	if (!ks || fd < 0)
		return -1;
	memset(ks, 0, sizeof(*ks));
	ks->fd = fd;
	ks->need_close = need_close;
	{
		int flags = fcntl(fd, F_GETFL);
		if (flags >= 0)
			(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	if (tcgetattr(fd, &ks->saved) == 0) {
		struct termios t = ks->saved;
		cfmakeraw(&t);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 1;
		(void)tcsetattr(fd, TCSANOW, &t);
		ks->have_saved = 1;
	}
	return 0;
}

static int kbd_open_auto(struct kbd_source *out, size_t out_cap, const char *preferred)
{
	if (!out || out_cap == 0)
		return -1;
	size_t n = 0;

	/* 1) Preferred path (usually ttyname(0)) */
	if (preferred && *preferred) {
		int fd = open(preferred, O_RDONLY | O_NOCTTY | O_NONBLOCK);
		if (fd >= 0 && n < out_cap) {
			if (kbd_setup_fd(&out[n], fd, 1) == 0)
				n++;
			else
				close(fd);
		}
	}

	/* 2) Common console ttys */
	for (int i = 1; i <= 8 && n < out_cap; i++) {
		char path[16];
		snprintf(path, sizeof(path), "/dev/tty%d", i);
		int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (kbd_setup_fd(&out[n], fd, 1) == 0)
			n++;
		else
			close(fd);
	}

	/* 3) Fallback controlling tty (may be a mux) */
	if (n < out_cap) {
		int fd = open("/dev/tty", O_RDONLY | O_NOCTTY | O_NONBLOCK);
		if (fd >= 0) {
			if (kbd_setup_fd(&out[n], fd, 1) == 0)
				n++;
			else
				close(fd);
		}
	}

	/* 4) Always include stdin as a last resort */
	if (n < out_cap) {
		(void)kbd_setup_fd(&out[n], 0, 0);
		n++;
	}

	return (int)n;
}

static void restore_kbd(struct ui_state *u)
{
	if (!u)
		return;
	if (u->kbdfd >= 0)
		tcsetattr(u->kbdfd, TCSANOW, &u->kbd_saved);
}

static void kbd_restore_all(struct kbd_source *ks, size_t n)
{
	if (!ks)
		return;
	for (size_t i = 0; i < n; i++) {
		if (ks[i].fd < 0)
			continue;
		if (ks[i].have_saved)
			(void)tcsetattr(ks[i].fd, TCSANOW, &ks[i].saved);
		if (ks[i].need_close)
			close(ks[i].fd);
		ks[i].fd = -1;
	}
}

static void ui_render_help(struct ui_state *u);
static void ui_render_terminal(struct ui_state *u);
static void ui_render_plot(struct ui_state *u);
static void ui_render_stack(struct ui_state *u);

static void ui_render(struct ui_state *u)
{
	struct vec_color fg = {0xEE, 0xEE, 0xEE};
	struct vec_color header_bg = {0x22, 0x22, 0x22};
	struct vec_color status_bg = {0x22, 0x22, 0x22};

	char line[256];
	int status_cursor_col = -1;
	int status_cursor_on = 0;

	ui_header_text(u, line, sizeof(line));
	(void)vec_draw_text_row(&u->fb, 0, line, fg, header_bg, -1, 0);

	if (u->show_help) {
		ui_render_help(u);
	} else {
		switch (u->tab) {
		case TAB_PLOT:
			ui_render_plot(u);
			break;
		case TAB_STACK:
			ui_render_stack(u);
			break;
		case TAB_TERMINAL:
		default:
			ui_render_terminal(u);
			break;
		}
	}

	ui_status_text(u, line, sizeof(line), &status_cursor_col, &status_cursor_on);
	(void)vec_draw_text_row(&u->fb, u->rows - 1, line, fg, status_bg, status_cursor_col, status_cursor_on);

	if (u->fb.mode == FB_MODE_MEMORY)
		(void)vec_fb_flush(&u->fb, NULL);
}

static void ui_render_terminal(struct ui_state *u)
{
	struct vec_color fg = {0xEE, 0xEE, 0xEE};
	struct vec_color panel_bg = {0x08, 0x08, 0x08};
	struct vec_color input_bg = {0x00, 0x00, 0x00};

	int panel_rows = u->rows - 2;
	int history_rows = panel_rows - 1;
	if (history_rows < 1)
		history_rows = 1;

	int start = 0;
	if (u->line_count > history_rows)
		start = u->line_count - history_rows;

	char line[256];
	for (int i = 0; i < history_rows; i++) {
		const char *s = "";
		int idx = start + i;
		if (idx >= 0 && idx < u->line_count)
			s = u->lines[idx];
		snprintf(line, sizeof(line), "%s", s ? s : "");
		if (u->cols > 0 && u->cols < (int)sizeof(line))
			line[u->cols] = 0;
		(void)vec_draw_text_row(&u->fb, 1 + i, line, fg, panel_bg, -1, 0);
	}

	/* Input row. */
	const char *prompt = "> ";
	size_t prompt_len = strlen(prompt);
	size_t visible = (u->cols > (int)prompt_len) ? (size_t)u->cols - prompt_len : 0;
	size_t off = 0;
	if (visible && u->cursor > visible)
		off = u->cursor - visible;
	if (off > u->input_len)
		off = u->input_len;

	char in[256];
	snprintf(in, sizeof(in), "%s%.*s", prompt, (int)visible, u->input + off);
	if (u->cols > 0 && u->cols < (int)sizeof(in))
		in[u->cols] = 0;
	int cursor_col = (int)prompt_len + (int)(u->cursor - off);
	if (cursor_col < 0 || cursor_col >= u->cols)
		cursor_col = -1;
	(void)vec_draw_text_row(&u->fb, u->rows - 2, in, fg, input_bg, cursor_col, 1);
}

static void ui_render_plot(struct ui_state *u)
{
	/* Pixel plot area: rows 1..rows-2 (inclusive). */
	int plot_y = VEC_FONT_H;
	int plot_h = (u->rows - 2) * VEC_FONT_H;
	if (plot_h > 0) {
		char perr[96];
		perr[0] = 0;
		if (u->plot_dim == 3) {
			const vec_node *expr = u->graph;
			if (!expr) {
				for (int i = 0; i < u->plot_count; i++) {
					const struct ui_plot *p = &u->plots[i];
					if (p->kind == UI_PLOT_FUNC && p->expr) {
						expr = p->expr;
						break;
					}
				}
			}
			int rc = vec_plot_render_3d(&u->fb, 0, plot_y, u->fb.disp.width, plot_h,
						    &u->env, expr,
						    u->x_min, u->x_max, u->y_min, u->y_max,
						    u->plot_yaw, u->plot_pitch, u->plot_zoom,
						    (int)u->plot_color_mode, u->show_axes_3d,
						    perr, sizeof(perr));
			if (rc != 0 && perr[0])
				ui_set_message(u, perr);
		} else {
			vec_plot plots[MAX_PLOTS];
			size_t nplots = 0;
			for (int i = 0; i < u->plot_count && nplots < (sizeof(plots) / sizeof(plots[0])); i++) {
				const struct ui_plot *p = &u->plots[i];
				vec_plot vp;
				memset(&vp, 0, sizeof(vp));
				if (p->kind == UI_PLOT_FUNC) {
					vp.kind = VEC_PLOT_FUNC;
					vp.expr = p->expr;
				} else if (p->kind == UI_PLOT_SERIES) {
					vp.kind = VEC_PLOT_SERIES;
					vp.xs = p->xs;
					vp.ys = p->ys;
					vp.len = p->len;
				} else {
					continue;
				}
				plots[nplots++] = vp;
			}
			const vec_node *fallback = (nplots == 0) ? u->graph : NULL;
			(void)vec_plot_render_multi(&u->fb, 0, plot_y, u->fb.disp.width, plot_h,
						    &u->env, plots, nplots, fallback,
						    u->x_min, u->x_max, u->y_min, u->y_max,
						    perr, sizeof(perr));
		}
	}
}

static void ui_render_stack(struct ui_state *u)
{
	struct vec_color fg = {0xEE, 0xEE, 0xEE};
	struct vec_color panel_bg = {0x08, 0x08, 0x08};
	char line[256];

	int panel_rows = u->rows - 2;
	int list_rows = panel_rows;

	int total = 0;
	for (vec_var *v = u->env.vars; v; v = v->next)
		total++;
	if (u->stack_sel < 0)
		u->stack_sel = 0;
	if (u->stack_sel >= total)
		u->stack_sel = total ? total - 1 : 0;
	if (u->stack_top > u->stack_sel)
		u->stack_top = u->stack_sel;
	if (u->stack_sel >= u->stack_top + list_rows)
		u->stack_top = u->stack_sel - list_rows + 1;
	if (u->stack_top < 0)
		u->stack_top = 0;

	int idx = 0;
	int row = 1;
	for (vec_var *v = u->env.vars; v && row < u->rows - 1; v = v->next, idx++) {
		if (idx < u->stack_top)
			continue;
		char valbuf[160];
		const char *val = ui_format_value(u, &v->value, valbuf, sizeof(valbuf));

		snprintf(line, sizeof(line), "%c %-10s %s", (idx == u->stack_sel) ? '>' : ' ',
			 v->name ? v->name : "?", val);
		if (u->cols > 0 && u->cols < (int)sizeof(line))
			line[u->cols] = 0;
		(void)vec_draw_text_row(&u->fb, row++, line, fg, panel_bg, -1, 0);
	}
	for (; row < u->rows - 1; row++)
		(void)vec_draw_text_row(&u->fb, row, "", fg, panel_bg, -1, 0);
}

static void ui_render_help(struct ui_state *u)
{
	struct vec_color fg = {0xEE, 0xEE, 0xEE};
	struct vec_color panel_bg = {0x08, 0x08, 0x08};

		static const char *help[] = {
			"Vector (Spark) port - keys",
			"",
			"F1 / Ctrl+T  terminal",
			"F2 / Ctrl+G  plot",
			"F3 / Ctrl+S  vars/stack",
			"Esc quit",
			"",
		"terminal:",
		"  Enter   evaluate",
		"  Ctrl+G  plot tab",
		"  Up/Down history",
		"  Left/Right edit",
		"  Tab     (todo) autocomplete",
		"",
		"plot:",
		"  :plot        force plot",
		"  :plot EXPR   set plot expr",
		"  :plots       list plots",
		"  :plotdel N   delete plot",
		"  :plotclear   clear plots",
		"  arrows pan   +/- zoom   PgUp/PgDn zoom",
		"  z zoom step  a autoscale  c term",
		"  $plotdim 2|3",
		"  $plotcolor [0|1|2]",
		"  3D: arrows rotate  Tab axes",
		"",
		"stack:",
		"  Up/Down select",
		"  Enter   edit variable",
		"  e       edit variable",
		"",
		"commands:",
		"  :help   toggle this help",
		"  :exact  exact rationals",
		"  :float  float mode",
		"  :prec N print precision",
		"  :x A B  set x-range",
		"  :y A B  set y-range",
		"  :view xmin xmax ymin ymax",
		"  :resetview",
		"  :autoscale",
	};
	int help_lines = (int)(sizeof(help) / sizeof(help[0]));

	if (u->help_top < 0)
		u->help_top = 0;
	if (u->help_top > help_lines)
		u->help_top = help_lines;

	char line[256];
	int row = 1;
	for (int i = u->help_top; i < help_lines && row < u->rows - 1; i++, row++) {
		snprintf(line, sizeof(line), "%s", help[i]);
		if (u->cols > 0 && u->cols < (int)sizeof(line))
			line[u->cols] = 0;
		(void)vec_draw_text_row(&u->fb, row, line, fg, panel_bg, -1, 0);
	}
	for (; row < u->rows - 1; row++)
		(void)vec_draw_text_row(&u->fb, row, "", fg, panel_bg, -1, 0);
}

static void ui_switch_tab(struct ui_state *u, enum vec_tab tab)
{
	if (!u)
		return;
	if (tab == u->tab && !u->edit_var[0])
		return;
	if (u->edit_var[0])
		ui_cancel_edit(u);
	u->tab = tab;
	u->show_help = 0;

	switch (u->tab) {
	case TAB_PLOT:
		if (!u->graph && u->plot_count == 0) {
			ui_set_message(u, "no plot yet (enter sin(x) then Ctrl+G/F2)");
			break;
		}
		if (u->x_min >= u->x_max) {
			u->x_min = -10;
			u->x_max = 10;
		}
		if (u->y_min >= u->y_max) {
			u->y_min = -10;
			u->y_max = 10;
		}
		if (u->plot_count == 0 && u->graph && vec_node_has_ident(u->graph, "x")) {
			const char *src = u->graph_src[0] ? u->graph_src : "<expr>";
			(void)ui_add_plot_func(u, src, u->graph);
		}
		if (u->plot_dim != 3)
			ui_autoscale_plots(u);
		ui_set_plot_tab_message(u);
		break;
	case TAB_STACK:
		u->stack_sel = 0;
		u->stack_top = 0;
		break;
	case TAB_TERMINAL:
	default:
		ui_set_message(u, "Enter eval | q/backspace/del quits (empty) | Ctrl+G/F2 plot | F3 stack | :clear");
		break;
	}
}

static void ui_handle_help_key(struct ui_state *u, vec_key k)
{
	if (!u)
		return;
	switch (k.kind) {
	case VEC_KEY_ESC:
	case VEC_KEY_ENTER:
		u->show_help = 0;
		break;
	case VEC_KEY_UP:
		if (u->help_top > 0)
			u->help_top--;
		break;
	case VEC_KEY_DOWN:
		u->help_top++;
		break;
	case VEC_KEY_HOME:
		u->help_top = 0;
		break;
	case VEC_KEY_END:
		u->help_top = 1 << 30;
		break;
	default:
		break;
	}
}

static void ui_handle_edit_key(struct ui_state *u, vec_key k)
{
	if (!u)
		return;
	switch (k.kind) {
	case VEC_KEY_ESC:
		ui_cancel_edit(u);
		ui_set_message(u, "edit canceled");
		break;
	case VEC_KEY_ENTER: {
		char name[sizeof(u->edit_var)];
		char expr[sizeof(u->input)];
		snprintf(name, sizeof(name), "%s", u->edit_var);
		snprintf(expr, sizeof(expr), "%s", u->input);
		ui_cancel_edit(u);
		ui_set_message(u, "");
		char line[320];
		snprintf(line, sizeof(line), "%s=%s", name, expr);
		ui_eval_line(u, line);
		break;
	}
	case VEC_KEY_BACKSPACE:
		ui_backspace(u);
		break;
	case VEC_KEY_DELETE:
		ui_delete(u);
		break;
	case VEC_KEY_LEFT:
		if (u->cursor)
			u->cursor--;
		break;
	case VEC_KEY_RIGHT:
		if (u->cursor < u->input_len)
			u->cursor++;
		break;
	case VEC_KEY_HOME:
		u->cursor = 0;
		break;
	case VEC_KEY_END:
		u->cursor = u->input_len;
		break;
	case VEC_KEY_RUNE:
		if (k.r >= 0x20 && k.r != 0x7f)
			ui_insert_char(u, (int)k.r);
		break;
	default:
		break;
	}
}

static void ui_handle_key(struct ui_state *u, vec_key k)
{
	if (!u)
		return;

	switch (k.kind) {
	case VEC_KEY_F1:
		ui_switch_tab(u, TAB_TERMINAL);
		return;
	case VEC_KEY_F2:
		ui_switch_tab(u, TAB_PLOT);
		return;
	case VEC_KEY_F3:
		ui_switch_tab(u, TAB_STACK);
		return;
	default:
		break;
	}

	if (u->tab != TAB_TERMINAL && k.kind == VEC_KEY_RUNE && (k.r == 'h' || k.r == 'H')) {
		u->show_help = !u->show_help;
		u->help_top = 0;
		return;
	}
	if (u->show_help) {
		ui_handle_help_key(u, k);
		return;
	}
	if (u->edit_var[0]) {
		ui_handle_edit_key(u, k);
		return;
	}

	switch (k.kind) {
	case VEC_KEY_ESC:
		running = 0;
		break;
	case VEC_KEY_ENTER:
		if (u->tab == TAB_TERMINAL)
			ui_submit(u);
		else if (u->tab == TAB_STACK) {
			vec_var *v = ui_stack_var_at(u, u->stack_sel);
			if (v && v->name)
				ui_start_edit_var(u, v->name, &v->value);
		}
		break;
	case VEC_KEY_TAB:
		if (u->tab == TAB_PLOT && u->plot_dim == 3) {
			u->show_axes_3d = !u->show_axes_3d;
			ui_set_message(u, u->show_axes_3d ? "3D axes: on" : "3D axes: off");
		}
		break;
		case VEC_KEY_CTRL:
			if (k.ctrl == 0x07) {
				ui_switch_tab(u, TAB_PLOT);
				break;
			}
			if (k.ctrl == 0x14) { /* Ctrl+T */
				ui_switch_tab(u, TAB_TERMINAL);
				break;
			}
			if (k.ctrl == 0x13) { /* Ctrl+S */
				ui_switch_tab(u, TAB_STACK);
				break;
			}
			break;
	case VEC_KEY_BACKSPACE:
		if (u->tab == TAB_TERMINAL) {
			if (ui_terminal_can_quit(u)) {
				running = 0;
				break;
			}
			ui_backspace(u);
		}
		break;
	case VEC_KEY_DELETE:
		if (u->tab == TAB_TERMINAL) {
			if (ui_terminal_can_quit(u)) {
				running = 0;
				break;
			}
			ui_delete(u);
		}
		break;
	case VEC_KEY_LEFT:
		if (u->tab == TAB_TERMINAL) {
			if (u->cursor)
				u->cursor--;
		} else if (u->tab == TAB_PLOT) {
			if (u->plot_dim == 3) {
				u->plot_yaw -= 0.1;
			} else {
				ui_plot_pan(u, -0.1, 0);
			}
		}
		break;
	case VEC_KEY_RIGHT:
		if (u->tab == TAB_TERMINAL) {
			if (u->cursor < u->input_len)
				u->cursor++;
		} else if (u->tab == TAB_PLOT) {
			if (u->plot_dim == 3) {
				u->plot_yaw += 0.1;
			} else {
				ui_plot_pan(u, 0.1, 0);
			}
		}
		break;
	case VEC_KEY_HOME:
		if (u->tab == TAB_TERMINAL)
			u->cursor = 0;
		else if (u->tab == TAB_STACK)
			u->stack_sel = 0;
		break;
	case VEC_KEY_END:
		if (u->tab == TAB_TERMINAL)
			u->cursor = u->input_len;
		else if (u->tab == TAB_STACK) {
			int total = ui_stack_var_count(u);
			if (total > 0)
				u->stack_sel = total - 1;
		}
		break;
	case VEC_KEY_UP:
		if (u->tab == TAB_TERMINAL)
			ui_hist_up(u);
		else if (u->tab == TAB_STACK && u->stack_sel > 0)
			u->stack_sel--;
		else if (u->tab == TAB_PLOT) {
			if (u->plot_dim == 3) {
				u->plot_pitch = clamp_double(u->plot_pitch - 0.08, -1.2, 1.2);
			} else {
				ui_plot_pan(u, 0, 0.1);
			}
		}
		break;
	case VEC_KEY_DOWN:
		if (u->tab == TAB_TERMINAL)
			ui_hist_down(u);
		else if (u->tab == TAB_STACK) {
			int total = ui_stack_var_count(u);
			if (u->stack_sel + 1 < total)
				u->stack_sel++;
		}
		else if (u->tab == TAB_PLOT) {
			if (u->plot_dim == 3) {
				u->plot_pitch = clamp_double(u->plot_pitch + 0.08, -1.2, 1.2);
			} else {
				ui_plot_pan(u, 0, -0.1);
			}
		}
		break;
	case VEC_KEY_PGUP:
		if (u->tab == TAB_PLOT) {
			if (u->plot_dim == 3)
				ui_plot_zoom_3d(u, u->zoom_in_factor);
			else
				ui_plot_zoom(u, u->zoom_in_factor);
		}
		break;
	case VEC_KEY_PGDN:
		if (u->tab == TAB_PLOT) {
			if (u->plot_dim == 3)
				ui_plot_zoom_3d(u, u->zoom_out_factor);
			else
				ui_plot_zoom(u, u->zoom_out_factor);
		}
		break;
	case VEC_KEY_RUNE:
		if (k.r == 'q' || k.r == 'Q') {
			if (u->tab != TAB_TERMINAL || ui_terminal_can_quit(u)) {
				running = 0;
				break;
			}
		}
		if (u->tab == TAB_STACK && (k.r == 'e' || k.r == 'E')) {
			vec_var *v = ui_stack_var_at(u, u->stack_sel);
			if (v && v->name)
				ui_start_edit_var(u, v->name, &v->value);
			break;
		}
		if (u->tab == TAB_TERMINAL && k.r >= 0x20 && k.r != 0x7f)
			ui_insert_char(u, (int)k.r);
		if (u->tab == TAB_PLOT) {
			if (k.r == 'a' || k.r == 'A') {
				ui_handle_command(u, "autoscale");
			} else if (k.r == 'c' || k.r == 'C') {
				ui_switch_tab(u, TAB_TERMINAL);
			} else if (k.r == 'z' || k.r == 'Z') {
				ui_cycle_plot_zoom(u);
			} else if (k.r == '+' || k.r == '=') {
				if (u->plot_dim == 3)
					ui_plot_zoom_3d(u, u->zoom_in_factor);
				else
					ui_plot_zoom(u, u->zoom_in_factor);
			} else if (k.r == '-') {
				if (u->plot_dim == 3)
					ui_plot_zoom_3d(u, u->zoom_out_factor);
				else
					ui_plot_zoom(u, u->zoom_out_factor);
			}
		}
		break;
	default:
		break;
	}
}

static void usage(FILE *out)
{
	fprintf(out, "vector (FUZIX) - Spark Vector port (WIP)\n");
	fprintf(out, "usage: vector [-m|-d] [-t /dev/ttyX|-] [-v] [-l]\n");
	fprintf(out, "       -v prints build info and exits\n");
	fprintf(out, "       -l prints verbose startup logs in text mode\n");
	fprintf(out, "       default is -d (direct framebuffer)\n");
}

int main(int argc, char **argv)
{
	int fb_mode = FB_MODE_DIRECT;
	const char *kbd_path = NULL;
	int show_version = 0;
	int verbose_log = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m")) {
			fb_mode = FB_MODE_MEMORY;
		} else if (!strcmp(argv[i], "-d")) {
			fb_mode = FB_MODE_DIRECT;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			kbd_path = argv[++i];
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			show_version = 1;
		} else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--log")) {
			verbose_log = 1;
		} else {
			usage(stderr);
			return 1;
		}
	}

	if (show_version) {
#ifndef VEC_BUILD_STR
#define VEC_BUILD_STR "unknown"
#endif
#ifndef VEC_BUILDTIME_STR
#define VEC_BUILDTIME_STR "unknown"
#endif
		printf("vector build %s %s\n", VEC_BUILD_STR, VEC_BUILDTIME_STR);
		return 0;
	}

	if (!kbd_path) {
		const char *t = ttyname(0);
		if (t && *t)
			kbd_path = t;
		else
			kbd_path = "-";
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);

	struct ui_state u;
	memset(&u, 0, sizeof(u));
	u.kbdfd = -1;
	u.tab = TAB_TERMINAL;
	u.hist_pos = 0;
	u.graph = NULL;
	u.graph_src[0] = 0;
	u.x_min = -10;
	u.x_max = 10;
	u.y_min = -10;
	u.y_max = 10;
	u.zoom_in_factor = 0.8;
	u.zoom_out_factor = 1.25;
	u.plot_dim = UI_DEFAULT_PLOT_DIM;
	u.plot_color_mode = UI_DEFAULT_3D_PLOT_COLOR_MODE;
	u.show_axes_3d = 0;
	ui_reset_3d_view(&u);

	if (verbose_log) {
		fprintf(stderr, "vector: starting (mode=%s)\n",
			fb_mode == FB_MODE_MEMORY ? "memory" : "direct");
		fflush(stderr);
	}

	char fb_err[128];
	if (vec_fb_open(&u.fb, fb_mode, fb_err, sizeof(fb_err)) != 0) {
		fprintf(stderr, "%s\n", fb_err);
		return 1;
	}

	if (verbose_log) {
		fprintf(stderr, "vector: fb opened+locked (text console still active)\n");
		fprintf(stderr, "vector: fb mode info: %ux%u fmt=%u\n",
			(unsigned)u.fb.disp.width, (unsigned)u.fb.disp.height, (unsigned)u.fb.disp.format);
		fflush(stderr);
	}

	/* Compute geometry early so we can prebuild the first frame. */
	u.cols = u.fb.disp.width / VEC_FONT_W;
	u.rows = u.fb.disp.height / VEC_FONT_H;
	if (u.cols <= 0 || u.rows <= 3) {
		fprintf(stderr, "fb: unsupported size %ux%u\n",
			(unsigned)u.fb.disp.width, (unsigned)u.fb.disp.height);
		vec_fb_close(&u.fb);
		return 1;
	}
	if (verbose_log) {
		fprintf(stderr, "vector: geometry cols=%d rows=%d\n", u.cols, u.rows);
		fflush(stderr);
	}

	struct kbd_source kbds[12];
	memset(kbds, 0, sizeof(kbds));
	for (size_t i = 0; i < (sizeof(kbds) / sizeof(kbds[0])); i++)
		kbds[i].fd = -1;
	int kbd_count = 0;

	if (kbd_path) {
		if (!strcmp(kbd_path, "-")) {
			(void)kbd_setup_fd(&kbds[0], 0, 0);
			kbd_count = 1;
		} else {
			int fd = open(kbd_path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
			if (fd < 0) {
				fprintf(stderr, "kbd: open %s: %s\n", kbd_path, strerror(errno));
				vec_fb_close(&u.fb);
				return 1;
			}
			(void)kbd_setup_fd(&kbds[0], fd, 1);
			kbd_count = 1;
		}
	} else {
		const char *t = ttyname(0);
		kbd_count = kbd_open_auto(kbds, sizeof(kbds) / sizeof(kbds[0]), t);
		if (kbd_count <= 0)
			kbd_count = 0;
	}

	if (verbose_log) {
		fprintf(stderr, "vector: kbd sources=%d\n", kbd_count);
		fflush(stderr);
	}

	vec_env_init(&u.env);
	ui_set_message(&u, "ready");

	if (verbose_log) {
		fprintf(stderr, "vector: env init ok\n");
		fprintf(stderr, "vector: preparing initial frame\n");
		fflush(stderr);
	}

		if (verbose_log) {
			fprintf(stderr, "vector: switching to graphics mode\n");
			fflush(stderr);
		}
	if (vec_fb_activate(&u.fb, fb_err, sizeof(fb_err)) != 0) {
		fprintf(stderr, "%s\n", fb_err);
		kbd_restore_all(kbds, (size_t)kbd_count);
		vec_env_destroy(&u.env);
		vec_fb_close(&u.fb);
		return 1;
	}

		/* Draw a minimal initial frame immediately after switching modes. */
		{
			char row[256];
			struct vec_color fg = {0xEE, 0xEE, 0xEE};
			struct vec_color header_bg = {0x22, 0x22, 0x22};
			struct vec_color panel_bg = {0x08, 0x08, 0x08};
			struct vec_color input_bg = {0x00, 0x00, 0x00};
			struct vec_color status_bg = {0x22, 0x22, 0x22};

			ui_header_text(&u, row, sizeof(row));
			(void)vec_draw_text_row(&u.fb, 0, row, fg, header_bg, -1, 0);

			row[0] = 0;
			for (int r = 1; r < u.rows - 2; r++)
				(void)vec_draw_text_row(&u.fb, r, row, fg, panel_bg, -1, 0);

			snprintf(row, sizeof(row), "> ");
			(void)vec_draw_text_row(&u.fb, u.rows - 2, row, fg, input_bg, 2, 1);

			{
				int status_cursor_col = -1;
				int status_cursor_on = 0;
				ui_status_text(&u, row, sizeof(row), &status_cursor_col, &status_cursor_on);
			}
			(void)vec_draw_text_row(&u.fb, u.rows - 1, row, fg, status_bg, -1, 0);
			if (u.fb.mode == FB_MODE_MEMORY)
				(void)vec_fb_flush(&u.fb, NULL);
		}

	if (verbose_log) {
		fprintf(stderr, "vector: first frame drawn\n");
		fflush(stderr);
	}

	ui_render(&u);

	uint8_t inbuf[128];
	size_t inlen = 0;

	while (running) {
		uint8_t tmp[32];
		for (int i = 0; i < kbd_count; i++) {
			if (kbds[i].fd < 0)
				continue;
			ssize_t n = read(kbds[i].fd, tmp, sizeof(tmp));
			if (n > 0) {
				if (inlen + (size_t)n > sizeof(inbuf))
					inlen = 0;
				memcpy(inbuf + inlen, tmp, (size_t)n);
				inlen += (size_t)n;
			}
		}

		for (;;) {
			size_t consumed = 0;
			vec_key k;
			if (!vec_next_key(inbuf, inlen, &consumed, &k))
				break;
			if (consumed == 0 || consumed > inlen)
				break;
			memmove(inbuf, inbuf + consumed, inlen - consumed);
			inlen -= consumed;
			ui_handle_key(&u, k);
			ui_render(&u);
			if (!running)
				break;
		}

		if (inlen == 0)
			usleep(10000);
	}

	kbd_restore_all(kbds, (size_t)kbd_count);
	ui_plots_clear(&u);
	vec_env_destroy(&u.env);
	vec_node_destroy(u.graph);
	for (int i = 0; i < u.line_count; i++)
		free(u.lines[i]);
	for (int i = 0; i < u.hist_count; i++)
		free(u.history[i]);
	vec_fb_close(&u.fb);
	return 0;
}
