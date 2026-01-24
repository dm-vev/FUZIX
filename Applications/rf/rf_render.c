#include "rf_render.h"

#include "rf_draw.h"
#include "rf_layout.h"
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
	render_panel(t, l.rf, "RF Control", t->focus == RF_FOCUS_RFCONTROL);
	render_panel(t, l.sniffer, "Packet Sniffer", t->focus == RF_FOCUS_SNIFFER);
	render_panel(t, l.proto, "Protocol View", t->focus == RF_FOCUS_PROTOCOL);
	render_panel(t, l.analysis, "Analysis", t->focus == RF_FOCUS_ANALYSIS);
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

	rf_draw_present(t);
	t->dirty = 0;
}
