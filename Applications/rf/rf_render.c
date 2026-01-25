#include "rf_render.h"

#include "rf_draw.h"
#include "rf_analysis_render.h"
#include "rf_automation_render.h"
#include "rf_filters.h"
#include "rf_layout.h"
#include "rf_menu.h"
#include "rf_replay.h"
#include "rf_sniffer.h"
#include "rf_task.h"
#include "rf_waterfall.h"

#include <stdio.h>
#include <string.h>

struct rf_menu_bar_seg {
	enum rf_menu_category cat;
	const char *text;
	int16_t x;
	int16_t w;
};

static int menu_bar_segments(const struct rf_task *t, struct rf_layout l, struct rf_menu_bar_seg out[7])
{
	if (!t || !out)
		return 0;

	static const char *labels[7] = {"View", "RF", "Capture", "Decode", "Display", "Advanced", "Help"};

	int avail_cols = t->cols;
	int pad = 1;
	const int gap_cols = 1;

	int total_cols = 0;
	for (int i = 0; i < 7; i++) {
		int w = (int)strlen(labels[i]);
		if (pad)
			w += 2;
		total_cols += w;
	}
	total_cols += gap_cols * (7 - 1);

	if (total_cols > avail_cols) {
		pad = 0;
		total_cols = 0;
		for (int i = 0; i < 7; i++)
			total_cols += (int)strlen(labels[i]);
		total_cols += gap_cols * (7 - 1);
	}

	int16_t x = (int16_t)(l.menu.x + 2);
	for (int i = 0; i < 7; i++) {
		const char *name = labels[i];
		/* Build text with optional padding (stored as static buffers). */
		static char buf[7][16];
		const char *s = name;
		if (pad) {
			snprintf(buf[i], sizeof(buf[i]), " %s ", name);
			s = buf[i];
		}
		out[i].cat = (enum rf_menu_category)i;
		out[i].text = s;
		out[i].x = x;
		out[i].w = (int16_t)strlen(s) * RF_FONT_W;
		x = (int16_t)(x + out[i].w + (int16_t)gap_cols * RF_FONT_W);
	}
	return 7;
}

static void render_menu_bar(const struct rf_task *t, struct rf_layout l)
{
	if (!t)
		return;

	struct rf_menu_bar_seg segs[7];
	int n = menu_bar_segments(t, l, segs);
	for (int i = 0; i < n; i++) {
		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_header_bg();
		if (t->show_menu && segs[i].cat == t->menu_cat) {
			fg = rf_color_menu_fg();
			bg = rf_color_menu_bg();
		}
		rf_draw_fill_rect(t, segs[i].x, l.menu.y, segs[i].w, RF_FONT_H, bg);
		rf_draw_text(t, segs[i].x, l.menu.y, segs[i].text, fg, bg, (int)(segs[i].w / RF_FONT_W));
	}
}

static void render_header(const struct rf_task *t, struct rf_layout l)
{
	struct rf_color header_bg = rf_color_header_bg();
	struct rf_color dim = rf_color_dim();

	rf_draw_fill_rect(t, l.menu.x, l.menu.y, l.menu.w, l.menu.h, header_bg);
	rf_draw_fill_rect(t, l.toolbar.x, l.toolbar.y, l.toolbar.w, l.toolbar.h, header_bg);

	render_menu_bar(t, l);

	const char *title = "2.4GHz RF Analyzer  nRF24 scan+spectrum+waterfall+sniffer";
	rf_draw_text(t, (int16_t)(l.toolbar.x + 2), l.toolbar.y, title, dim, header_bg, t->cols);
}

static void render_prompt_overlay(const struct rf_task *t)
{
	if (!t)
		return;

	int box_cols = t->cols - 6;
	if (box_cols > 54)
		box_cols = 54;
	const int box_rows = 10;

	int16_t px = (int16_t)(3 * RF_FONT_W);
	int16_t py = (int16_t)((RF_HEADER_ROWS + 3) * RF_FONT_H);
	int16_t pw = (int16_t)(box_cols * RF_FONT_W);
	int16_t ph = (int16_t)(box_rows * RF_FONT_H);

	struct rf_color border = rf_color_border();
	struct rf_color panel = rf_color_panel_bg();
	struct rf_color header = rf_color_header_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color accent = rf_color_accent();
	struct rf_color warn = rf_color_warn();

	rf_draw_fill_rect(t, px, py, pw, ph, border);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(ph - 2), panel);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(RF_FONT_H + 1), header);

	const char *title = t->prompt_title[0] ? t->prompt_title : "Input";
	char title_line[96];
	snprintf(title_line, sizeof(title_line), "%s  (Enter apply, Esc cancel)", title);
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + 1), title_line, fg, header, box_cols);

	int16_t field_y = (int16_t)(py + 2 * RF_FONT_H + 2);
	rf_draw_fill_rect(t, (int16_t)(px + 2), field_y, (int16_t)(pw - 4), (int16_t)(RF_FONT_H + 2), header);

	char text[64];
	unsigned n = 0;
	for (int i = 0; i < t->prompt_len && n + 1 < sizeof(text); i++) {
		uint32_t r = t->prompt_buf[i];
		if (r < 0x20 || r > 0x7e)
			r = '?';
		text[n++] = (char)r;
	}
	text[n] = 0;
	rf_draw_text(t, (int16_t)(px + 4), (int16_t)(field_y + 1), text, fg, header, box_cols - 2);

	int cursor = t->prompt_cursor;
	if (cursor < 0)
		cursor = 0;
	if (cursor > t->prompt_len)
		cursor = t->prompt_len;
	int16_t cx = (int16_t)(px + 4 + cursor * RF_FONT_W);
	rf_draw_fill_rect(t, cx, (int16_t)(field_y + 1), 1, RF_FONT_H, accent);

	if (t->prompt_err[0]) {
		int16_t err_y = (int16_t)(field_y + 2 * RF_FONT_H);
		char err_line[96];
		snprintf(err_line, sizeof(err_line), "ERR: %s", t->prompt_err);
		rf_draw_text(t, (int16_t)(px + 2), err_y, err_line, warn, panel, box_cols);
	}
}

static void render_help_overlay(const struct rf_task *t)
{
	if (!t)
		return;

	int box_cols = t->cols - 6;
	if (box_cols > 54)
		box_cols = 54;
	const int box_rows = 16;

	int16_t px = (int16_t)(3 * RF_FONT_W);
	int16_t py = (int16_t)((RF_HEADER_ROWS + 2) * RF_FONT_H);
	int16_t pw = (int16_t)(box_cols * RF_FONT_W);
	int16_t ph = (int16_t)(box_rows * RF_FONT_H);

	struct rf_color border = rf_color_border();
	struct rf_color panel = rf_color_panel_bg();
	struct rf_color header = rf_color_header_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();

	rf_draw_fill_rect(t, px, py, pw, ph, border);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(ph - 2), panel);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(RF_FONT_H + 1), header);
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + 1), "Help  (h/Esc close)", fg, header, box_cols);

	static const char *const lines[] = {
		"s start/stop scan",
		"w freeze/resume waterfall",
		"p pause/resume packet capture",
		"r reset view",
		"m open menu",
		"t cycle focus",
		"c change channel",
		"f filters",
		"q quit",
		"",
		"Arrows: adjust focused panel",
	};

	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		int16_t y = (int16_t)(py + 2 * RF_FONT_H + 2 + (int16_t)i * RF_FONT_H);
		struct rf_color line_fg = lines[i][0] ? dim : border;
		rf_draw_text(t, (int16_t)(px + 2), y, lines[i], line_fg, panel, box_cols);
	}
}

static void hex_mask_string(char *out, unsigned outsz, const uint8_t *val, const uint8_t *mask, int len)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!val || !mask || len <= 0)
		return;

	unsigned n = 0;
	for (int i = 0; i < len && n + 2 < outsz; i++) {
		char hi = '?';
		char lo = '?';
		if ((mask[i] & 0xF0) != 0) {
			uint8_t v = (val[i] >> 4) & 0x0F;
			hi = (char)((v < 10) ? ('0' + v) : ('A' + (v - 10)));
		}
		if ((mask[i] & 0x0F) != 0) {
			uint8_t v = val[i] & 0x0F;
			lo = (char)((v < 10) ? ('0' + v) : ('A' + (v - 10)));
		}
		out[n++] = hi;
		out[n++] = lo;
	}
	if (n >= outsz)
		n = outsz - 1;
	out[n] = 0;
}

static void render_filters_overlay(const struct rf_task *t, struct rf_layout l)
{
	if (!t)
		return;

	enum { filter_lines = 8 };

	int box_cols = l.right_main_cols;
	if (box_cols < 20)
		box_cols = 20;
	const int box_rows = 3 + filter_lines;

	int16_t px = (int16_t)(l.sniffer.x + 2);
	int16_t py = (int16_t)(l.sniffer.y + RF_FONT_H * 2);
	int16_t pw = (int16_t)(box_cols * RF_FONT_W);
	int16_t ph = (int16_t)(box_rows * RF_FONT_H);

	struct rf_color border = rf_color_border();
	struct rf_color panel = rf_color_panel_bg();
	struct rf_color header = rf_color_header_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();
	struct rf_color sel_bg = rf_color_sel_bg();
	struct rf_color sel_fg = rf_color_sel_fg();

	rf_draw_fill_rect(t, px, py, pw, ph, border);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(ph - 2), panel);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(RF_FONT_H + 1), header);
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + 1), "Filters (Esc/f close)", fg, header, box_cols);
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + RF_FONT_H + 2), "Up/Down sel  Left/Right adj  Enter edit", dim,
		     panel, box_cols);

	char lines[filter_lines][64];
	memset(lines, 0, sizeof(lines));

	snprintf(lines[0], sizeof(lines[0]), "CRC    <%s>", rf_filter_crc_str(t->filter_crc));

	snprintf(lines[1], sizeof(lines[1]), "CH     <%s>", rf_filter_channel_str(t->filter_channel));
	if (t->filter_channel == RF_FILTER_CH_SELECTED) {
		snprintf(lines[1] + strlen(lines[1]), sizeof(lines[1]) - strlen(lines[1]), "  ch=%03d", t->selected_channel);
	} else if (t->filter_channel == RF_FILTER_CH_RANGE) {
		snprintf(lines[1] + strlen(lines[1]), sizeof(lines[1]) - strlen(lines[1]), "  %03d-%03d", t->channel_range_lo,
			 t->channel_range_hi);
	}

	snprintf(lines[2], sizeof(lines[2]), "MINLEN [%02d]", rf_clamp_int(t->filter_min_len, 0, 32));
	if (t->filter_max_len > 0)
		snprintf(lines[3], sizeof(lines[3]), "MAXLEN [%02d]", rf_clamp_int(t->filter_max_len, 0, 32));
	else
		snprintf(lines[3], sizeof(lines[3]), "MAXLEN [--]");

	char addr[16];
	if (t->filter_addr_len > 0) {
		hex_mask_string(addr, sizeof(addr), t->filter_addr, t->filter_addr_mask, t->filter_addr_len);
		snprintf(lines[4], sizeof(lines[4]), "ADDR   <%s>", addr);
	} else {
		snprintf(lines[4], sizeof(lines[4]), "ADDR   <(none)>");
	}

	char pay[32];
	if (t->filter_payload_len > 0) {
		hex_mask_string(pay, sizeof(pay), t->filter_payload, t->filter_payload_mask, t->filter_payload_len);
		snprintf(lines[5], sizeof(lines[5]), "PAY    <%s>", pay);
	} else {
		snprintf(lines[5], sizeof(lines[5]), "PAY    <(none)>");
	}

	if (t->filter_age_ms > 0)
		snprintf(lines[6], sizeof(lines[6]), "AGEms  [%d]", t->filter_age_ms);
	else
		snprintf(lines[6], sizeof(lines[6]), "AGEms  [off]");

	if (t->filter_burst_max_ms > 0)
		snprintf(lines[7], sizeof(lines[7]), "BURSTΔ [%d]", t->filter_burst_max_ms);
	else
		snprintf(lines[7], sizeof(lines[7]), "BURSTΔ [off]");

	int16_t y0 = (int16_t)(py + 2 * RF_FONT_H + 2);
	int sel = t->filter_sel;
	if (sel < 0)
		sel = 0;
	if (sel >= filter_lines)
		sel = filter_lines - 1;
	for (int i = 0; i < filter_lines; i++) {
		int16_t y = (int16_t)(y0 + (int16_t)i * RF_FONT_H);
		struct rf_color line_fg = fg;
		struct rf_color line_bg = panel;
		if (i == sel) {
			line_fg = sel_fg;
			line_bg = sel_bg;
		}
		rf_draw_fill_rect(t, (int16_t)(px + 1), y, (int16_t)(pw - 2), RF_FONT_H, line_bg);
		rf_draw_text(t, (int16_t)(px + 2), y, lines[i], line_fg, line_bg, box_cols);
	}
}

static void render_menu_overlay(const struct rf_task *t, struct rf_layout l)
{
	if (!t)
		return;

	int count = 0;
	const struct rf_menu_item *items = rf_menu_items(t->menu_cat, &count);
	if (!items || count <= 0)
		return;

	struct rf_menu_bar_seg segs[7];
	struct rf_menu_bar_seg anchor = {.cat = t->menu_cat, .x = (int16_t)(l.menu.x + 2), .w = (int16_t)(12 * RF_FONT_W)};
	int seg_n = menu_bar_segments(t, l, segs);
	for (int i = 0; i < seg_n; i++) {
		if (segs[i].cat == t->menu_cat) {
			anchor = segs[i];
			break;
		}
	}

	int max_line_cols = 18;
	for (int i = 0; i < count; i++) {
		char line[96];
		rf_menu_item_line(t, items[i], line, sizeof(line));
		int cols = (int)strlen(line);
		if (cols > max_line_cols)
			max_line_cols = cols;
	}
	if (max_line_cols < 18)
		max_line_cols = 18;
	if (max_line_cols > t->cols - 2)
		max_line_cols = t->cols - 2;

	int16_t pw = (int16_t)(max_line_cols * RF_FONT_W + 4);
	int16_t ph = (int16_t)(count * RF_FONT_H + 2);
	int16_t px = anchor.x;
	int16_t py = (int16_t)(l.menu.y + l.menu.h);

	int16_t screen_w = (int16_t)t->fb.disp.width;
	int16_t screen_h = (int16_t)t->fb.disp.height;
	int16_t status_h = (int16_t)(RF_STATUS_ROWS * RF_FONT_H);

	if (px + pw > screen_w)
		px = (int16_t)(screen_w - pw);
	if (px < 0)
		px = 0;

	int16_t max_h = (int16_t)(screen_h - status_h - py);
	if (ph > max_h)
		ph = max_h;
	if (ph <= 2)
		return;
	int visible_rows = (int)((ph - 2) / RF_FONT_H);
	if (visible_rows <= 0)
		return;

	int sel = t->menu_sel;
	if (sel < 0)
		sel = 0;
	if (sel >= count)
		sel = count - 1;
	int top = sel - visible_rows / 2;
	if (top < 0)
		top = 0;
	if (top > count - visible_rows)
		top = count - visible_rows;
	if (top < 0)
		top = 0;

	struct rf_color border = rf_color_border();
	struct rf_color panel = rf_color_panel_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color sel_bg = rf_color_sel_bg();
	struct rf_color sel_fg = rf_color_sel_fg();

	rf_draw_fill_rect(t, px, py, pw, ph, border);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(ph - 2), panel);

	for (int row = 0; row < visible_rows; row++) {
		int i = top + row;
		if (i < 0 || i >= count)
			continue;
		int16_t y = (int16_t)(py + 1 + (int16_t)row * RF_FONT_H);
		struct rf_color line_fg = fg;
		struct rf_color line_bg = panel;
		if (i == sel) {
			line_fg = sel_fg;
			line_bg = sel_bg;
		}
		rf_draw_fill_rect(t, (int16_t)(px + 1), y, (int16_t)(pw - 2), RF_FONT_H, line_bg);
		char line[96];
		rf_menu_item_line(t, items[i], line, sizeof(line));
		rf_draw_text(t, (int16_t)(px + 2), y, line, line_fg, line_bg, max_line_cols);
	}
}

static void render_overlay(const struct rf_task *t, struct rf_layout l)
{
	if (!t)
		return;

	if (t->show_prompt) {
		render_prompt_overlay(t);
		return;
	}
	if (t->show_menu) {
		render_menu_overlay(t, l);
		return;
	}
	if (t->show_help) {
		render_help_overlay(t);
		return;
	}
	if (t->show_filters) {
		render_filters_overlay(t, l);
		return;
	}
	if (t->show_automation) {
		rf_automation_render_overlay(t);
		return;
	}
}

void rf_render_status_line1(const struct rf_task *t, char *out, unsigned outsz)
{
	if (!out || outsz == 0)
		return;
	if (!t) {
		snprintf(out, outsz, "");
		return;
	}

	const char *mode = "IDLE";
	if (t->replay_active)
		mode = "REPLAY";
	else if (t->scan_active)
		mode = "SCAN";

	const char *wf = t->waterfall_frozen ? "FROZEN" : "RUN";

	const char *cap = "LIVE";
	if (t->replay_active)
		cap = t->replay_playing ? "PLAY" : "PAUSE";
	else if (t->capture_paused)
		cap = "PAUSED";

	const char *rec = t->recording ? "ON" : "OFF";
	(void)wf;
	(void)cap;
	(void)rec;

	snprintf(out, outsz, "MODE:%s  WF:%s  CAP:%s  REC:%s", mode, wf, cap, rec);
	if (t->sweep_count)
		snprintf(out + strlen(out), outsz - strlen(out), "  SWP:%lu", (unsigned long)t->sweep_count);
	if (t->replay_active) {
		char tt[24];
		rf_replay_time_text(t, tt, sizeof(tt));
		snprintf(out + strlen(out), outsz - strlen(out), "  %s x%d", tt, rf_clamp_int(t->replay_speed, 1, 32));
	}
}

void rf_render_status_line2(const struct rf_task *t, char *out, unsigned outsz)
{
	if (!out || outsz == 0)
		return;
	if (!t) {
		snprintf(out, outsz, "");
		return;
	}
	if (t->replay_active) {
		snprintf(out, outsz,
			 "keys: s play  w wf  p play  r reset  m menu  t focus  c chan  f filt  h help  q quit");
		return;
	}
	snprintf(out, outsz, "keys: s scan  w wf  p cap  r reset  m menu  t focus  c chan  f filt  h help  q quit");
}

static void render_status(const struct rf_task *t, struct rf_layout l)
{
	struct rf_color bg = rf_color_status_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();

	rf_draw_fill_rect(t, l.status1.x, l.status1.y, l.status1.w, l.status1.h, bg);
	rf_draw_fill_rect(t, l.status2.x, l.status2.y, l.status2.w, l.status2.h, bg);

	char s1[128];
	char s2[128];
	rf_render_status_line1(t, s1, sizeof(s1));
	rf_render_status_line2(t, s2, sizeof(s2));

	rf_draw_text(t, (int16_t)(l.status1.x + 2), l.status1.y, s1, fg, bg, t->cols);
	rf_draw_text(t, (int16_t)(l.status2.x + 2), l.status2.y, s2, dim, bg, t->cols);
}

static void render_panel(const struct rf_task *t, struct rf_rect r, const char *title, int focused)
{
	struct rf_color border = rf_color_border();
	struct rf_color panel = rf_color_panel_bg();
	struct rf_color header = rf_color_header_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();
	struct rf_color focus_mark = rf_color_focus_mark();

	rf_draw_fill_rect(t, r.x, r.y, r.w, r.h, border);
	struct rf_rect inner = rf_rect_inset(r, 1, 1);
	rf_draw_fill_rect(t, inner.x, inner.y, inner.w, inner.h, panel);
	rf_draw_fill_rect(t, inner.x, inner.y, inner.w, (int16_t)(RF_FONT_H + 1), header);

	const char *mark = " ";
	struct rf_color mark_color = dim;
	if (focused) {
		mark = ">";
		mark_color = focus_mark;
	}
	rf_draw_text(t, (int16_t)(inner.x + 2), inner.y, mark, mark_color, header, 1);

	int max_cols = (int)(inner.w / RF_FONT_W) - 2;
	if (max_cols < 0)
		max_cols = 0;
	rf_draw_text(t, (int16_t)(inner.x + 2 + RF_FONT_W), inner.y, title, fg, header, max_cols);
}

static void slider_ascii(char *out, unsigned outsz, int value, int min, int max, int width)
{
	if (!out || outsz == 0)
		return;
	if (width <= 0) {
		snprintf(out, outsz, "[]");
		return;
	}
	if (max <= min) {
		unsigned n = 0;
		if (n < outsz)
			out[n++] = '[';
		for (int i = 0; i < width && n + 1 < outsz; i++)
			out[n++] = '-';
		if (n < outsz)
			out[n++] = ']';
		if (n >= outsz)
			n = outsz - 1;
		out[n] = 0;
		return;
	}

	value = rf_clamp_int(value, min, max);
	int fill = (value - min) * width / (max - min);
	fill = rf_clamp_int(fill, 0, width);

	unsigned n = 0;
	if (n < outsz)
		out[n++] = '[';
	for (int i = 0; i < width && n + 1 < outsz; i++)
		out[n++] = (i < fill) ? '#' : '-';
	if (n < outsz)
		out[n++] = ']';
	if (n >= outsz)
		n = outsz - 1;
	out[n] = 0;
}

static const char *checkbox_ascii(int on)
{
	return on ? "[x]" : "[ ]";
}

static void render_rf_control(const struct rf_task *t, struct rf_layout l)
{
	render_panel(t, l.rf, "RF Control", t->focus == RF_FOCUS_RFCONTROL);

	struct rf_rect inner = rf_rect_inset(l.rf, 2, 2);
	int max_cols = (int)(inner.w / RF_FONT_W);
	if (max_cols <= 0)
		return;

	int16_t y = (int16_t)(inner.y + RF_FONT_H);

	char preset[64];
	snprintf(preset, sizeof(preset), "PRESET: (none)");
	if (t->active_preset[0]) {
		snprintf(preset, sizeof(preset), "PRESET: %s%s", t->active_preset, t->preset_dirty ? "*" : "");
	}
	rf_draw_text(t, (int16_t)(inner.x + 2), y, preset, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	char rec[96];
	struct rf_color rec_color = rf_color_dim();
	snprintf(rec, sizeof(rec), "REC: OFF");
	if (t->record_err[0]) {
		snprintf(rec, sizeof(rec), "REC: ERROR %s", t->record_err);
		rec_color = rf_color_warn();
	} else if (t->recording) {
		snprintf(rec, sizeof(rec), "REC: ON  %s  %luKB  swp:%lu pkt:%lu", t->record_name,
			 (unsigned long)(t->record_bytes / 1024u), (unsigned long)t->record_sweeps,
			 (unsigned long)t->record_packets);
		rec_color = rf_color_accent();
	}
	rf_draw_text(t, (int16_t)(inner.x + 2), y, rec, rec_color, rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	char dwell_slider[16];
	char step_slider[16];
	slider_ascii(dwell_slider, sizeof(dwell_slider), t->dwell_time_ms, 1, 50, 8);
	slider_ascii(step_slider, sizeof(step_slider), rf_clamp_int(t->scan_speed_scalar, 1, 10), 1, 10, 8);

	char lines[8][64];
	snprintf(lines[0], sizeof(lines[0]), "LO    [%03d]", t->channel_range_lo);
	snprintf(lines[1], sizeof(lines[1]), "HI    [%03d]", t->channel_range_hi);
	snprintf(lines[2], sizeof(lines[2]), "DWELL %s %02dms", dwell_slider, t->dwell_time_ms);
	snprintf(lines[3], sizeof(lines[3]), "STEP  %s %02d", step_slider, rf_clamp_int(t->scan_speed_scalar, 1, 10));
	snprintf(lines[4], sizeof(lines[4]), "RATE  <%s>", rf_data_rate_str(t->data_rate));
	snprintf(lines[5], sizeof(lines[5]), "CRC   <%s>", rf_crc_mode_str(t->crc_mode));
	snprintf(lines[6], sizeof(lines[6]), "ACK   %s", checkbox_ascii(t->auto_ack));
	snprintf(lines[7], sizeof(lines[7]), "PWR   <%s>", rf_power_level_str(t->power_level));

	for (int i = 0; i < 8; i++) {
		int16_t row_y = (int16_t)(y + (int16_t)i * RF_FONT_H);
		if (row_y + RF_FONT_H > inner.y + inner.h)
			return;
		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_panel_bg();
		if (i == t->selected_setting && t->focus == RF_FOCUS_RFCONTROL) {
			fg = rf_color_sel_fg();
			bg = rf_color_sel_bg();
		}
		rf_draw_fill_rect(t, (int16_t)(inner.x + 1), row_y, (int16_t)(inner.w - 2), RF_FONT_H, bg);
		rf_draw_text(t, (int16_t)(inner.x + 2), row_y, lines[i], fg, bg, max_cols);
	}
}

static void render_sniffer(struct rf_task *t, struct rf_layout l)
{
	render_panel(t, l.sniffer, "Packet Sniffer", t->focus == RF_FOCUS_SNIFFER);

	struct rf_rect inner = rf_rect_inset(l.sniffer, 2, 2);
	int max_cols = (int)(inner.w / RF_FONT_W);
	if (max_cols <= 0)
		return;

	int16_t y0 = (int16_t)(inner.y + RF_FONT_H + 1);
	const char *status = "LIVE";
	if (t->replay_active) {
		status = t->replay_playing ? "REPLAY/PLAY" : "REPLAY/PAUSE";
	} else if (t->capture_paused) {
		status = "PAUSED";
	}
	char filt[128];
	rf_sniffer_filter_summary(t, filt, sizeof(filt));

	char hdr[192];
	snprintf(hdr, sizeof(hdr), "%s  pps:%d drop:%lu  %s", status, t->pkts_per_sec, (unsigned long)t->pkt_dropped, filt);
	rf_draw_text(t, (int16_t)(inner.x + 2), y0, hdr, rf_color_fg(), rf_color_panel_bg(), max_cols);

	rf_draw_text(t, (int16_t)(inner.x + 2), (int16_t)(y0 + RF_FONT_H), "t(ms) ch r ln addr   c", rf_color_dim(),
		     rf_color_panel_bg(), max_cols);

	int16_t list_y = (int16_t)(y0 + 2 * RF_FONT_H);
	int list_rows = (int)((inner.h - 3 * RF_FONT_H - 1) / RF_FONT_H);
	if (list_rows <= 0)
		return;

	int total = rf_sniffer_filtered_count(t);
	if (total <= 0) {
		rf_draw_text(t, (int16_t)(inner.x + 2), list_y, "(no packets)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	if (t->sniffer_sel < 0)
		t->sniffer_sel = 0;
	if (t->sniffer_sel >= total) {
		t->sniffer_sel = total - 1;
		if (t->sniffer_sel < 0)
			t->sniffer_sel = 0;
	}

	int max_top = total - list_rows;
	if (max_top < 0)
		max_top = 0;
	if (t->sniffer_top < 0)
		t->sniffer_top = 0;
	if (t->sniffer_top > max_top)
		t->sniffer_top = max_top;
	if (t->sniffer_sel < t->sniffer_top)
		t->sniffer_top = t->sniffer_sel;
	if (t->sniffer_sel >= t->sniffer_top + list_rows) {
		t->sniffer_top = t->sniffer_sel - list_rows + 1;
		if (t->sniffer_top > max_top)
			t->sniffer_top = max_top;
	}

	for (int row = 0; row < list_rows; row++) {
		int idx = t->sniffer_top + row;
		if (idx < 0 || idx >= total)
			continue;

		struct rf_packet_summary p;
		if (!rf_sniffer_filtered_packet_summary_by_index(t, idx, &p))
			continue;

		int ts = (int)(p.tick % 10000u);
		char r = rf_rate_short(p.rate);
		char addr[7];
		char crc[3];
		rf_addr_suffix3(p.addr_len, p.addr, addr);
		rf_crc_text(p.crc_len, p.crc_ok, crc);

		char line[64];
		snprintf(line, sizeof(line), "%04d %03d %c %02d %s %s", ts, p.channel, r, p.length, addr, crc);

		int16_t y = (int16_t)(list_y + (int16_t)row * RF_FONT_H);
		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_panel_bg();
		if (idx == t->sniffer_sel && t->focus == RF_FOCUS_SNIFFER) {
			fg = rf_color_sel_fg();
			bg = rf_color_sel_bg();
		}
		rf_draw_fill_rect(t, (int16_t)(inner.x + 1), y, (int16_t)(inner.w - 2), RF_FONT_H, bg);
		rf_draw_text(t, (int16_t)(inner.x + 2), y, line, fg, bg, max_cols);
	}
}

static void hex_bytes(const uint8_t *b, int n, char *out, unsigned outsz)
{
	static const char digits[] = "0123456789ABCDEF";
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!b || n <= 0)
		return;
	unsigned w = 0;
	for (int i = 0; i < n && w + 2 < outsz; i++) {
		out[w++] = digits[(b[i] >> 4) & 0x0F];
		out[w++] = digits[b[i] & 0x0F];
	}
	if (w >= outsz)
		w = outsz - 1;
	out[w] = 0;
}

static void render_hex_dump(const struct rf_task *t, struct rf_rect box, int16_t y, const uint8_t *data, int len,
			    int max_cols)
{
	if (!t)
		return;
	if (!data || len <= 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(empty)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}
	const int bytes_per_line = 8;
	for (int off = 0; off < len; off += bytes_per_line) {
		if (y + RF_FONT_H > box.y + box.h)
			return;
		int chunk_len = len - off;
		if (chunk_len > bytes_per_line)
			chunk_len = bytes_per_line;
		char hex[32];
		hex_bytes(data + off, chunk_len, hex, sizeof(hex));
		char line[64];
		snprintf(line, sizeof(line), "%02X: %s", off, hex);
		rf_draw_text(t, (int16_t)(box.x + 2), y, line, rf_color_dim(), rf_color_panel_bg(), max_cols);
		y = (int16_t)(y + RF_FONT_H);
	}
}

static void render_protocol(struct rf_task *t, struct rf_layout l)
{
	render_panel(t, l.proto, "Protocol View", t->focus == RF_FOCUS_PROTOCOL);

	struct rf_rect inner = rf_rect_inset(l.proto, 2, 2);
	int max_cols = (int)(inner.w / RF_FONT_W);
	if (max_cols <= 0)
		return;

	int16_t y0 = (int16_t)(inner.y + RF_FONT_H + 1);
	const struct rf_packet *p = NULL;
	if (t->replay_active) {
		if (t->replay_pkt_cache_ok)
			p = &t->replay_pkt_cache;
	} else {
		p = rf_sniffer_filtered_live_packet_by_index(t, t->sniffer_sel);
	}
	if (!p) {
		char mode[32];
		snprintf(mode, sizeof(mode), "mode:%s", rf_protocol_mode_str(t->proto_mode));
		rf_draw_text(t, (int16_t)(inner.x + 2), y0, mode, rf_color_dim(), rf_color_panel_bg(), max_cols);
		const char *msg = "(select a packet)";
		if (t->replay_active && t->replay && t->replay_pkt_limit > 0)
			msg = "(loading packet...)";
		rf_draw_text(t, (int16_t)(inner.x + 2), (int16_t)(y0 + RF_FONT_H), msg, rf_color_dim(), rf_color_panel_bg(),
			     max_cols);
		return;
	}

	char crc[3];
	rf_crc_text(p->crc_len, p->crc_ok, crc);
	char h1[64];
	char h2[64];
	snprintf(h1, sizeof(h1), "mode:%s  #%lu  ch:%03d  t:%04d", rf_protocol_mode_str(t->proto_mode),
		 (unsigned long)p->seq, p->channel, (int)(p->tick % 10000u));
	snprintf(h2, sizeof(h2), "rate:%s len:%02d crc:%s addr:%dB", rf_data_rate_str(p->rate), p->length, crc,
		 p->addr_len);

	rf_draw_text(t, (int16_t)(inner.x + 2), y0, h1, rf_color_fg(), rf_color_panel_bg(), max_cols);
	rf_draw_text(t, (int16_t)(inner.x + 2), (int16_t)(y0 + RF_FONT_H), h2, rf_color_dim(), rf_color_panel_bg(), max_cols);

	int16_t y = (int16_t)(y0 + 2 * RF_FONT_H);
	if (y + RF_FONT_H > inner.y + inner.h)
		return;

	if (t->proto_mode == RF_PROTO_RAW) {
		uint8_t raw[1 + 5 + 32 + 2];
		int n = 0;
		raw[n++] = 0x55;
		for (int i = 0; i < p->addr_len && i < 5; i++)
			raw[n++] = p->addr[i];
		for (int i = 0; i < p->length && i < 32; i++)
			raw[n++] = p->payload[i];
		for (int i = 0; i < p->crc_len && i < 2; i++)
			raw[n++] = p->crc[i];
		render_hex_dump(t, inner, y, raw, n, max_cols);
		return;
	}

	char addr_line[64];
	snprintf(addr_line, sizeof(addr_line), "pre:55 addr:%02X%02X%02X%02X%02X", p->addr[0], p->addr[1], p->addr[2],
		 p->addr[3], p->addr[4]);
	rf_draw_text(t, (int16_t)(inner.x + 2), y, addr_line, rf_color_fg(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	if (p->crc_len > 0) {
		char crc_line[32];
		snprintf(crc_line, sizeof(crc_line), "crc:%s %02X%02X", crc, p->crc[0], p->crc[1]);
		rf_draw_text(t, (int16_t)(inner.x + 2), y, crc_line, rf_color_fg(), rf_color_panel_bg(), max_cols);
		y = (int16_t)(y + RF_FONT_H);
	}

	render_hex_dump(t, inner, y, p->payload, p->length, max_cols);
}

static void render_spectrum(const struct rf_task *t, struct rf_layout l)
{
	render_panel(t, l.spectrum, "Spectrum", t->focus == RF_FOCUS_SPECTRUM);

	struct rf_rect inner = rf_rect_inset(l.spectrum, 2, 2);
	const int16_t header_y = (int16_t)(inner.y + RF_FONT_H + 1);
	const int16_t labels_y = (int16_t)(inner.y + inner.h - RF_FONT_H);
	struct rf_rect plot = {
		.x = inner.x,
		.y = (int16_t)(header_y + RF_FONT_H + 1),
		.w = inner.w,
		.h = (int16_t)(inner.h - 3 * RF_FONT_H - 3),
	};
	if (plot.h <= 0 || plot.w <= 0)
		return;

	rf_draw_fill_rect(t, plot.x, plot.y, plot.w, plot.h, rf_color_bg());

	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		int16_t x0 = (int16_t)(plot.x + (int16_t)ch * plot.w / RF_NUM_CHANNELS);
		int16_t x1 = (int16_t)(plot.x + (int16_t)(ch + 1) * plot.w / RF_NUM_CHANNELS);
		if (x1 <= x0)
			x1 = (int16_t)(x0 + 1);
		if (x0 >= plot.x + plot.w)
			continue;
		if (x1 > plot.x + plot.w)
			x1 = (int16_t)(plot.x + plot.w);

		int16_t h_avg = (int16_t)t->energy_avg[ch] * plot.h / 255;
		if (h_avg > 0) {
			rf_draw_fill_rect(t, x0, (int16_t)(plot.y + plot.h - h_avg), (int16_t)(x1 - x0), h_avg,
					  rf_color_accent());
		}

		int16_t y_peak = (int16_t)(plot.y + plot.h - 1 - (int16_t)t->energy_peak[ch] * plot.h / 255);
		if (y_peak >= plot.y && y_peak < plot.y + plot.h) {
			rf_draw_fill_rect(t, x0, y_peak, (int16_t)(x1 - x0), 1, rf_color_warn());
		}
	}

	int16_t ch_lo_x = (int16_t)(plot.x + (int16_t)t->channel_range_lo * plot.w / RF_NUM_CHANNELS);
	int16_t ch_hi_x = (int16_t)(plot.x + (int16_t)(t->channel_range_hi + 1) * plot.w / RF_NUM_CHANNELS);
	rf_draw_fill_rect(t, ch_lo_x, plot.y, 1, plot.h, rf_color_border());
	rf_draw_fill_rect(t, ch_hi_x, plot.y, 1, plot.h, rf_color_border());

	int16_t marker_x = (int16_t)(plot.x + (int16_t)t->selected_channel * plot.w / RF_NUM_CHANNELS);
	rf_draw_fill_rect(t, marker_x, plot.y, 1, plot.h, rf_color_fg());

	for (int i = 0; i < 6; i++) {
		int ch = i * 25;
		int16_t x = (int16_t)(plot.x + (int16_t)ch * plot.w / RF_NUM_CHANNELS);
		rf_draw_fill_rect(t, x, (int16_t)(plot.y + plot.h - 2), 1, 2, rf_color_dim());

		char label[8];
		snprintf(label, sizeof(label), "%d", ch);
		int remain_px = (int)(inner.x + inner.w - x);
		if (remain_px <= 0)
			continue;
		int remain_cols = remain_px / RF_FONT_W;
		if (remain_cols <= 0)
			continue;
		rf_draw_text(t, x, labels_y, label, rf_color_dim(), rf_color_panel_bg(), remain_cols);
	}

	char info[96];
	snprintf(info, sizeof(info), "ch %03d  avg+peak  rng %d-%d  dwell %dms  step %d", t->selected_channel,
		 t->channel_range_lo, t->channel_range_hi, t->dwell_time_ms, rf_clamp_int(t->scan_speed_scalar, 1, 10));
	rf_draw_text(t, (int16_t)(inner.x + 2), header_y, info, rf_color_fg(), rf_color_panel_bg(), l.left_cols);
}

static void render_waterfall(struct rf_task *t, struct rf_layout l)
{
	render_panel(t, l.waterfall, "Waterfall", t->focus == RF_FOCUS_WATERFALL);

	struct rf_rect inner = rf_rect_inset(l.waterfall, 2, 2);
	struct rf_rect plot;
	int16_t header_y;
	if (!rf_waterfall_plot_rect(t, l, &plot, &header_y))
		return;

	rf_draw_fill_rect(t, plot.x, plot.y, plot.w, plot.h, rf_color_bg());
	if (t->wf_buf && t->wf_w == plot.w && t->wf_h == plot.h)
		rf_waterfall_blit(t, plot);

	int16_t marker_x = (int16_t)(plot.x + (int16_t)t->selected_channel * plot.w / RF_NUM_CHANNELS);
	rf_draw_fill_rect(t, marker_x, plot.y, 1, plot.h, rf_color_fg());
	int16_t ch_lo_x = (int16_t)(plot.x + (int16_t)t->channel_range_lo * plot.w / RF_NUM_CHANNELS);
	int16_t ch_hi_x = (int16_t)(plot.x + (int16_t)(t->channel_range_hi + 1) * plot.w / RF_NUM_CHANNELS);
	rf_draw_fill_rect(t, ch_lo_x, plot.y, 1, plot.h, rf_color_border());
	rf_draw_fill_rect(t, ch_hi_x, plot.y, 1, plot.h, rf_color_border());

	const char *state = t->waterfall_frozen ? "FROZEN" : "RUN";
	char info[96];
	snprintf(info, sizeof(info), "%s  pal:%s  step:%d  sync:spectrum", state, rf_wf_palette_str(t->wf_palette),
		 rf_clamp_int(t->scan_speed_scalar, 1, 10));
	rf_draw_text(t, (int16_t)(inner.x + 2), header_y, info, rf_color_fg(), rf_color_panel_bg(), l.left_cols);
}

static void render_placeholders(const struct rf_task *t, struct rf_layout l)
{
	/* Left column is fully rendered. */
	render_rf_control(t, l);
	render_sniffer((struct rf_task *)t, l);
	render_protocol((struct rf_task *)t, l);
	char title[32];
	snprintf(title, sizeof(title), "Analysis %s", rf_analysis_view_str(t->analysis_view));
	render_panel(t, l.analysis, title, t->focus == RF_FOCUS_ANALYSIS);
	rf_analysis_render_contents((struct rf_task *)t, l);
}

void rf_render_dirty(struct rf_task *t)
{
	if (!t || !t->active)
		return;
	if (t->dirty == 0)
		return;

	struct rf_layout l = rf_compute_layout(t);

	/* For now, always full repaint when anything is dirty. */
	rf_draw_fill_rect(t, 0, 0, (int16_t)t->fb.disp.width, (int16_t)t->fb.disp.height, rf_color_bg());
	render_header(t, l);
	render_spectrum(t, l);
	render_waterfall(t, l);
	render_placeholders(t, l);
	render_status(t, l);
	render_overlay(t, l);

	rf_draw_present(t);
	t->dirty = 0;
}
