#include "vec_draw.h"
#include "vec_env.h"
#include "vec_eval.h"
#include "vec_fb.h"
#include "vec_keys.h"
#include "vec_parser.h"
#include "vec_plot.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

enum {
	MAX_OUTPUT_LINES = 200,
	MAX_HISTORY = 200
};

enum vec_tab {
	TAB_TERMINAL = 0,
	TAB_PLOT = 1,
	TAB_STACK = 2,
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

	double x_min;
	double x_max;
	double y_min;
	double y_max;

	vec_env env;
};

static volatile sig_atomic_t running = 1;

static void on_sig(int sig)
{
	(void)sig;
	running = 0;
}

static void ui_set_message(struct ui_state *u, const char *s)
{
	if (!u)
		return;
	if (!s)
		s = "";
	snprintf(u->message, sizeof(u->message), "%s", s);
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
	if (v && v->kind == VEC_VALUE_NUMBER) {
		char buf[64];
		ui_set_input(u, vec_number_string(v->num, u->env.prec, buf, sizeof(buf)));
	} else {
		ui_set_input(u, "");
	}
	ui_set_message(u, "Enter apply | Esc cancel");
}

static void ui_cancel_edit(struct ui_state *u)
{
	if (!u)
		return;
	u->edit_var[0] = 0;
	ui_set_input(u, "");
}

static void ui_handle_command(struct ui_state *u, const char *cmdline)
{
	if (!u || !cmdline)
		return;

	if (!strncmp(cmdline, "plot", 4) && (cmdline[4] == 0 || isspace((unsigned char)cmdline[4]))) {
		const char *expr = cmdline + 4;
		while (*expr == ' ')
			expr++;
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
			if (u->graph)
				vec_node_destroy(u->graph);
			u->graph = acts.items[0].expr;
			acts.items[0].expr = NULL;
			vec_actions_destroy(&acts);

			snprintf(u->graph_src, sizeof(u->graph_src), "%s", expr);
			ui_set_message(u, "plot updated");
		}
		if (!u->graph && !*expr)
			ui_set_message(u, "plot: no expression set");
		ui_switch_tab(u, TAB_PLOT);
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
		ui_set_message(u, "view updated");
		return;
	}
	if (!strcmp(cmdline, "autoscale")) {
		if (!u->graph) {
			ui_set_message(u, "autoscale: no plot");
			return;
		}
		double miny = 1e300;
		double maxy = -1e300;
		for (int i = 0; i < 200; i++) {
			double x = u->x_min + ((double)i / 199.0) * (u->x_max - u->x_min);
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
		if (miny <= maxy && isfinite(miny) && isfinite(maxy) && miny != maxy) {
			double pad = (maxy - miny) * 0.1;
			u->y_min = miny - pad;
			u->y_max = maxy + pad;
			ui_set_message(u, "autoscaled");
		} else {
			ui_set_message(u, "autoscale: no finite samples");
		}
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
		u->env.mode = VEC_MODE_EXACT;
		ui_set_message(u, "mode: exact");
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
				ui_append_line(u, err[0] ? err : "eval error");
				continue;
			}
			if (vec_env_set_var(&u->env, a->var_name, v) != 0) {
				ui_append_line(u, "eval: out of memory");
				vec_value_destroy(&v);
				continue;
			}
			{
				char buf[64];
				char out[128];
				snprintf(out, sizeof(out), "%s = %s", a->var_name,
					 vec_number_string(v.num, u->env.prec, buf, sizeof(buf)));
				ui_append_line(u, out);
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
			if (a->expr && vec_node_has_ident(a->expr, "x")) {
				if (u->graph)
					vec_node_destroy(u->graph);
				u->graph = a->expr;
				a->expr = NULL;
				snprintf(u->graph_src, sizeof(u->graph_src), "%s", line);
				ui_append_line(u, "= <plot expr>");
				ui_set_message(u, "plot expr captured (F2)");
			} else {
				ui_append_line(u, err[0] ? err : "eval error");
			}
			continue;
		}
		if (v.kind == VEC_VALUE_NUMBER) {
			char buf[64];
			ui_append_line(u, vec_number_string(v.num, u->env.prec, buf, sizeof(buf)));
		} else {
			ui_append_line(u, "<value>");
		}
		if (a->expr && vec_node_has_ident(a->expr, "x")) {
			if (u->graph)
				vec_node_destroy(u->graph);
			u->graph = a->expr;
			a->expr = NULL;
			snprintf(u->graph_src, sizeof(u->graph_src), "%s", line);
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
	int fd = open(path, O_RDWR | O_NOCTTY);
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

static void restore_kbd(struct ui_state *u)
{
	if (!u)
		return;
	if (u->kbdfd >= 0)
		tcsetattr(u->kbdfd, TCSANOW, &u->kbd_saved);
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
		(void)vec_plot_render(&u->fb, 0, plot_y, u->fb.disp.width, plot_h,
				      &u->env, u->graph,
				      u->x_min, u->x_max, u->y_min, u->y_max,
				      perr, sizeof(perr));
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
		char valbuf[64];
		const char *val = "<value>";
		if (v->value.kind == VEC_VALUE_NUMBER)
			val = vec_number_string(v->value.num, u->env.prec, valbuf, sizeof(valbuf));

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
		"F1  terminal",
		"F2  plot",
		"F3  vars/stack",
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
		"  :plot EXPR   set plot expr",
		"  arrows pan   +/- zoom",
		"  a autoscale",
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
		ui_set_message(u, "arrows pan | +/- zoom | PgUp/PgDn zoom | a autoscale | c term");
		break;
	case TAB_STACK:
		u->stack_sel = 0;
		u->stack_top = 0;
		break;
	case TAB_TERMINAL:
	default:
		ui_set_message(u, "Enter eval | Ctrl+G plot | F2 plot | F3 stack | :clear");
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
		break;
	case VEC_KEY_CTRL:
		if (k.ctrl == 0x07) {
			ui_switch_tab(u, TAB_PLOT);
			break;
		}
		break;
	case VEC_KEY_BACKSPACE:
		if (u->tab == TAB_TERMINAL)
			ui_backspace(u);
		break;
	case VEC_KEY_DELETE:
		if (u->tab == TAB_TERMINAL)
			ui_delete(u);
		break;
	case VEC_KEY_LEFT:
		if (u->tab == TAB_TERMINAL) {
			if (u->cursor)
				u->cursor--;
		} else if (u->tab == TAB_PLOT) {
			double dx = (u->x_max - u->x_min) * 0.1;
			u->x_min -= dx;
			u->x_max -= dx;
		}
		break;
	case VEC_KEY_RIGHT:
		if (u->tab == TAB_TERMINAL) {
			if (u->cursor < u->input_len)
				u->cursor++;
		} else if (u->tab == TAB_PLOT) {
			double dx = (u->x_max - u->x_min) * 0.1;
			u->x_min += dx;
			u->x_max += dx;
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
			double dy = (u->y_max - u->y_min) * 0.1;
			u->y_min += dy;
			u->y_max += dy;
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
			double dy = (u->y_max - u->y_min) * 0.1;
			u->y_min -= dy;
			u->y_max -= dy;
		}
		break;
	case VEC_KEY_RUNE:
		if (u->tab != TAB_TERMINAL && k.r == 'q') {
			running = 0;
			break;
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
			} else if (k.r == '+' || k.r == '=') {
				double cx = (u->x_min + u->x_max) * 0.5;
				double cy = (u->y_min + u->y_max) * 0.5;
				double rx = (u->x_max - u->x_min) * 0.5 * 0.8;
				double ry = (u->y_max - u->y_min) * 0.5 * 0.8;
				u->x_min = cx - rx;
				u->x_max = cx + rx;
				u->y_min = cy - ry;
				u->y_max = cy + ry;
			} else if (k.r == '-') {
				double cx = (u->x_min + u->x_max) * 0.5;
				double cy = (u->y_min + u->y_max) * 0.5;
				double rx = (u->x_max - u->x_min) * 0.5 * 1.25;
				double ry = (u->y_max - u->y_min) * 0.5 * 1.25;
				u->x_min = cx - rx;
				u->x_max = cx + rx;
				u->y_min = cy - ry;
				u->y_max = cy + ry;
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
	fprintf(out, "usage: vector [-m|-d] [-t /dev/tty1]\n");
}

int main(int argc, char **argv)
{
	int fb_mode = FB_MODE_MEMORY;
	const char *kbd_path = "/dev/tty1";

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m")) {
			fb_mode = FB_MODE_MEMORY;
		} else if (!strcmp(argv[i], "-d")) {
			fb_mode = FB_MODE_DIRECT;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			kbd_path = argv[++i];
		} else {
			usage(stderr);
			return 1;
		}
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

	char fb_err[128];
	if (vec_fb_open(&u.fb, fb_mode, fb_err, sizeof(fb_err)) != 0) {
		fprintf(stderr, "%s\n", fb_err);
		return 1;
	}

	u.kbdfd = open_kbd(&u, kbd_path);
	if (u.kbdfd < 0) {
		fprintf(stderr, "kbd: open %s: %s\n", kbd_path, strerror(errno));
		vec_fb_close(&u.fb);
		return 1;
	}

	u.cols = u.fb.disp.width / VEC_FONT_W;
	u.rows = u.fb.disp.height / VEC_FONT_H;
	if (u.cols <= 0 || u.rows <= 3) {
		fprintf(stderr, "fb: unsupported size %ux%u\n",
			(unsigned)u.fb.disp.width, (unsigned)u.fb.disp.height);
		restore_kbd(&u);
		close(u.kbdfd);
		vec_fb_close(&u.fb);
		return 1;
	}

	vec_env_init(&u.env);
	ui_set_message(&u, "ready");

	ui_render(&u);

	uint8_t inbuf[128];
	size_t inlen = 0;

	while (running) {
		uint8_t tmp[32];
		ssize_t n = read(u.kbdfd, tmp, sizeof(tmp));
		if (n > 0) {
			if (inlen + (size_t)n > sizeof(inbuf))
				inlen = 0;
			memcpy(inbuf + inlen, tmp, (size_t)n);
			inlen += (size_t)n;
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
	}

	restore_kbd(&u);
	close(u.kbdfd);
	vec_env_destroy(&u.env);
	vec_node_destroy(u.graph);
	for (int i = 0; i < u.line_count; i++)
		free(u.lines[i]);
	for (int i = 0; i < u.hist_count; i++)
		free(u.history[i]);
	vec_fb_close(&u.fb);
	return 0;
}
