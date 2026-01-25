#include "rf_analysis_render.h"

#include "rf_analytics.h"
#include "rf_draw.h"
#include "rf_replay.h"
#include "rf_selection.h"
#include "rf_session.h"
#include "rf_sniffer.h"
#include "rf_task.h"
#include "rf_types.h"

#include <stdio.h>
#include <string.h>

static void tabs_line(const struct rf_task *t, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t)
		return;

	static const enum rf_analysis_view labels[] = {
		RF_ANALYSIS_CHANNELS,	   RF_ANALYSIS_DEVICES,      RF_ANALYSIS_TIMING,      RF_ANALYSIS_COLLISIONS,
		RF_ANALYSIS_CORRELATION,   RF_ANALYSIS_COMPARISON,   RF_ANALYSIS_MONITORING,  RF_ANALYSIS_ANNOTATIONS,
		RF_ANALYSIS_DIAGNOSTICS,   RF_ANALYSIS_STRESS,
	};

	size_t w = 0;
	w += (size_t)snprintf(out + w, outsz - w, "tabs:");
	for (unsigned i = 0; i < (unsigned)(sizeof(labels) / sizeof(labels[0])); i++) {
		enum rf_analysis_view v = labels[i];
		const char *s = rf_analysis_view_str(v);
		if (v == t->analysis_view)
			w += (size_t)snprintf(out + w, outsz - w, " [%s]", s);
		else
			w += (size_t)snprintf(out + w, outsz - w, " %s", s);
		if (w >= outsz)
			break;
	}
	snprintf(out + w, outsz - w, "  (</> switch, ^/v select)");
}

static void fill_line(struct rf_task *t, struct rf_rect box, int16_t y, struct rf_color bg)
{
	rf_draw_fill_rect(t, (int16_t)(box.x + 1), y, (int16_t)(box.w - 2), RF_FONT_H, bg);
}

static void draw_line(struct rf_task *t, struct rf_rect box, int16_t y, const char *text, struct rf_color fg, struct rf_color bg,
		      int max_cols)
{
	fill_line(t, box, y, bg);
	rf_draw_text(t, (int16_t)(box.x + 2), y, text, fg, bg, max_cols);
}

static void render_channels(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;
	if (t->ana_sweep_count == 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no sweep data yet)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	struct rf_chan_score top[8];
	int top_n = rf_analytics_top_channels(t, 8, top, (int)(sizeof(top) / sizeof(top[0])));
	if (top_n <= 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no channels)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	if (t->analysis_sel < 0)
		t->analysis_sel = 0;
	if (t->analysis_sel >= top_n)
		t->analysis_sel = top_n - 1;

	for (int i = 0; i < top_n; i++) {
		int ch = top[i].ch;
		int score = top[i].score;
		int occ_pct = (int)(t->ana_occ_count[ch] * 100u / t->ana_sweep_count);
		int bad_pct = 0;
		if (t->ana_chan_pkt[ch] > 0)
			bad_pct = (int)(t->ana_chan_bad[ch] * 100u / t->ana_chan_pkt[ch]);

		char per[16];
		rf_analytics_periodic_text(t, ch, per, sizeof(per));

		char line[96];
		snprintf(line, sizeof(line), "ch:%03d  score:%3d  occ:%2d%%  bad:%2d%%  %s", ch, score, occ_pct, bad_pct, per);

		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_panel_bg();
		if (i == t->analysis_sel && t->focus == RF_FOCUS_ANALYSIS) {
			fg = rf_color_sel_fg();
			bg = rf_color_sel_bg();
		}
		draw_line(t, box, (int16_t)(y + (int16_t)i * RF_FONT_H), line, fg, bg, max_cols);
	}

	char best[96];
	rf_analytics_best_history_line(t, 4, best, sizeof(best));
	int16_t yy = (int16_t)(y + (int16_t)top_n * RF_FONT_H);
	if (yy + RF_FONT_H <= box.y + box.h) {
		rf_draw_text(t, (int16_t)(box.x + 2), yy, best, rf_color_dim(), rf_color_panel_bg(), max_cols);
	}
}

static void render_devices(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;

	uint64_t now = t->now_tick;
	if (t->replay_active)
		now = t->replay_now_tick;

	int active = 0;
	for (int i = 0; i < RF_MAX_DEVICES; i++) {
		struct rf_device_stat *d = &t->devices[i];
		if (!d->used)
			continue;
		if (d->last_tick != 0 && now > d->last_tick && (now - d->last_tick) <= 5000)
			active++;
	}

	char hdr[96];
	snprintf(hdr, sizeof(hdr), "devices: active:%d total:%d  (by addr)", active, t->device_count);
	rf_draw_text(t, (int16_t)(box.x + 2), y, hdr, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	int top_idx[8];
	int top_n = rf_analytics_top_device_indices(t, 8, top_idx, (int)(sizeof(top_idx) / sizeof(top_idx[0])));
	if (top_n <= 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no devices)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	if (t->analysis_sel < 0)
		t->analysis_sel = 0;
	if (t->analysis_sel >= top_n)
		t->analysis_sel = top_n - 1;

	for (int i = 0; i < top_n; i++) {
		struct rf_device_stat *d = &t->devices[top_idx[i]];

		int bad_pct = 0;
		uint32_t crc_total = d->crc_ok + d->crc_bad;
		if (crc_total > 0)
			bad_pct = (int)(d->crc_bad * 100u / crc_total);

		char age[12];
		snprintf(age, sizeof(age), "-");
		if (d->last_tick != 0 && now > d->last_tick)
			snprintf(age, sizeof(age), "%ds", (int)((now - d->last_tick) / 1000u));

		char suf[7];
		rf_addr_suffix3(d->addr_len, d->addr, suf);

		char line[96];
		snprintf(line, sizeof(line), "%s  pk:%4lu bad:%2d%% hop:%3lu rty:%3lu last:%s", suf, (unsigned long)d->pkt_count,
			 bad_pct, (unsigned long)d->hop_count, (unsigned long)d->retries, age);

		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_panel_bg();
		if (i == t->analysis_sel && t->focus == RF_FOCUS_ANALYSIS) {
			fg = rf_color_sel_fg();
			bg = rf_color_sel_bg();
		}
		draw_line(t, box, (int16_t)(y + (int16_t)i * RF_FONT_H), line, fg, bg, max_cols);
	}
}

static void render_timing(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;

	uint8_t addr_len = 0;
	uint8_t addr[5];
	if (!rf_selection_selected_packet_addr(t, &addr_len, addr)) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(select a packet for device timing)", rf_color_dim(), rf_color_panel_bg(),
			     max_cols);
		return;
	}

	const struct rf_device_stat *d = rf_analytics_find_device_const(t, addr_len, addr);
	if (!d) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(device not tracked yet)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	char suf[7];
	rf_addr_suffix3(d->addr_len, d->addr, suf);
	char h1[96];
	snprintf(h1, sizeof(h1), "dev:%s  pk:%lu bad:%lu rty:%lu", suf, (unsigned long)d->pkt_count, (unsigned long)d->crc_bad,
		 (unsigned long)d->retries);
	rf_draw_text(t, (int16_t)(box.x + 2), y, h1, rf_color_fg(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	if (d->int_count == 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(need more packets)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	char h2[96];
	char h3[96];
	snprintf(h2, sizeof(h2), "int(avg:%lums jit:%lums min:%lu max:%lu)", (unsigned long)d->int_avg, (unsigned long)d->int_jitter,
		 (unsigned long)d->int_min, (unsigned long)d->int_max);
	snprintf(h3, sizeof(h3), "bursts:%lu maxlen:%u hop:%lu", (unsigned long)d->burst_count, (unsigned)d->burst_max,
		 (unsigned long)d->hop_count);
	rf_draw_text(t, (int16_t)(box.x + 2), y, h2, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);
	rf_draw_text(t, (int16_t)(box.x + 2), y, h3, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	char slot[64];
	slot[0] = 0;
	if (d->int_count >= 6 && d->int_avg > 0 && d->int_jitter <= d->int_avg / 10u)
		snprintf(slot, sizeof(slot), "slot: ~%lums (low jitter)", (unsigned long)d->int_avg);
	if (slot[0] && y + RF_FONT_H <= box.y + box.h)
		rf_draw_text(t, (int16_t)(box.x + 2), y, slot, rf_color_dim(), rf_color_panel_bg(), max_cols);
}

static int has_packet_stats(const struct rf_task *t)
{
	if (!t)
		return 0;
	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		if (t->ana_chan_pkt[ch] != 0)
			return 1;
	}
	return 0;
}

static void render_collisions(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;

	if (!has_packet_stats(t)) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no packet stats yet)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	struct rf_conflict_row rows[8];
	int n = rf_analytics_top_conflict_channels(t, 8, rows, (int)(sizeof(rows) / sizeof(rows[0])));
	if (n <= 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no conflicts)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}
	for (int i = 0; i < n; i++) {
		char line[64];
		snprintf(line, sizeof(line), "ch:%03d  pkt:%4lu  bad:%2d%%  rty:%2d%%", rows[i].ch, (unsigned long)rows[i].pkt,
			 rows[i].bad_pct, rows[i].retry_pct);
		rf_draw_text(t, (int16_t)(box.x + 2), (int16_t)(y + (int16_t)i * RF_FONT_H), line, rf_color_fg(), rf_color_panel_bg(),
			     max_cols);
	}
}

static void render_correlation(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;
	if (t->occ_hist_count == 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(need sweep history)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	int ref = rf_clamp_int(t->selected_channel, 0, RF_MAX_CHANNEL);
	char hdr[64];
	snprintf(hdr, sizeof(hdr), "ref ch:%03d  win:%d sweeps", ref, t->occ_hist_count);
	rf_draw_text(t, (int16_t)(box.x + 2), y, hdr, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	struct rf_corr_entry top[8];
	int top_n = rf_analytics_top_correlated_channels(t, ref, 8, top, (int)(sizeof(top) / sizeof(top[0])));
	if (top_n <= 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no correlations)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}
	for (int i = 0; i < top_n; i++) {
		char line[64];
		snprintf(line, sizeof(line), "ch:%03d  jacc:%2d%%  both:%d", top[i].ch, top[i].jacc_pct, top[i].both);
		rf_draw_text(t, (int16_t)(box.x + 2), (int16_t)(y + (int16_t)i * RF_FONT_H), line, rf_color_fg(), rf_color_panel_bg(),
			     max_cols);
	}

	int16_t yy = (int16_t)(y + (int16_t)top_n * RF_FONT_H + RF_FONT_H);
	if (yy + RF_FONT_H <= box.y + box.h) {
		uint8_t addr_len = 0;
		uint8_t addr[5];
		if (rf_selection_selected_packet_addr(t, &addr_len, addr)) {
			const struct rf_device_stat *d = rf_analytics_find_device_const(t, addr_len, addr);
			if (d) {
				char seq[64];
				rf_analytics_device_hop_seq_text(d, 12, seq, sizeof(seq));
				char line[96];
				snprintf(line, sizeof(line), "hop seq: %s", seq);
				rf_draw_text(t, (int16_t)(box.x + 2), yy, line, rf_color_dim(), rf_color_panel_bg(), max_cols);
			}
		}
	}
}

static int abs_int(int v)
{
	return (v < 0) ? -v : v;
}

static void render_comparison(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;
	if (!t->compare) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(load compare session in Capture menu)", rf_color_dim(), rf_color_panel_bg(),
			     max_cols);
		return;
	}
	if (t->compare_err[0]) {
		char line[96];
		snprintf(line, sizeof(line), "ERR: %s", t->compare_err);
		rf_draw_text(t, (int16_t)(box.x + 2), y, line, rf_color_warn(), rf_color_panel_bg(), max_cols);
		return;
	}

	const char *cur_name = "LIVE";
	if (t->replay_active && t->replay)
		cur_name = t->replay->name;

	char hdr[96];
	snprintf(hdr, sizeof(hdr), "cur:%s  cmp:%s", cur_name, t->compare->name);
	rf_draw_text(t, (int16_t)(box.x + 2), y, hdr, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	struct diff_row {
		int ch;
		int diff;
		int cur_occ;
		int cmp_occ;
		int cur_bad;
		int cmp_bad;
	} top[7];
	int top_n = 0;

	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		int cur_occ = 0;
		if (t->ana_sweep_count > 0)
			cur_occ = (int)(t->ana_occ_count[ch] * 100u / t->ana_sweep_count);
		int cmp_occ = 0;
		if (t->compare->sweep_count_total > 0)
			cmp_occ = (int)(t->compare->occ_count[ch] * 100u / t->compare->sweep_count_total);

		int cur_bad = 0;
		if (t->ana_chan_pkt[ch] > 0)
			cur_bad = (int)(t->ana_chan_bad[ch] * 100u / t->ana_chan_pkt[ch]);
		int cmp_bad = 0;
		if (t->compare->pkt_count[ch] > 0)
			cmp_bad = (int)(t->compare->pkt_bad[ch] * 100u / t->compare->pkt_count[ch]);

		int diff = abs_int(cur_occ - cmp_occ) + abs_int(cur_bad - cmp_bad);
		if (diff == 0)
			continue;

		struct diff_row r = {.ch = ch, .diff = diff, .cur_occ = cur_occ, .cmp_occ = cmp_occ, .cur_bad = cur_bad, .cmp_bad = cmp_bad};
		if (top_n < (int)(sizeof(top) / sizeof(top[0]))) {
			top[top_n++] = r;
		} else {
			int worst = 0;
			for (int i = 1; i < top_n; i++) {
				if (top[i].diff < top[worst].diff)
					worst = i;
			}
			if (r.diff <= top[worst].diff)
				continue;
			top[worst] = r;
		}
	}

	for (int i = 0; i < top_n; i++) {
		for (int j = i + 1; j < top_n; j++) {
			if (top[j].diff > top[i].diff) {
				struct diff_row tmp = top[i];
				top[i] = top[j];
				top[j] = tmp;
			}
		}
	}

	if (top_n == 0) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no diff)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	for (int i = 0; i < top_n; i++) {
		char line[64];
		snprintf(line, sizeof(line), "ch:%03d  occ:%2d->%2d  bad:%2d->%2d", top[i].ch, top[i].cur_occ, top[i].cmp_occ,
			 top[i].cur_bad, top[i].cmp_bad);
		rf_draw_text(t, (int16_t)(box.x + 2), (int16_t)(y + (int16_t)i * RF_FONT_H), line, rf_color_fg(), rf_color_panel_bg(),
			     max_cols);
	}
}

static void render_monitoring(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;
	if (!t->replay_active || !t->replay) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(monitoring graph: load session in replay)", rf_color_dim(),
			     rf_color_panel_bg(), max_cols);
		return;
	}
	if (t->replay->bucket_ms == 0 || t->replay->band_occ_pct_len == 0 || !t->replay->band_occ_pct) {
		rf_draw_text(t, (int16_t)(box.x + 2), y, "(no long-term sweep stats)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}

	uint64_t bucket_ms = t->replay->bucket_ms;
	uint64_t rel_now = 0;
	if (t->replay_now_tick >= t->replay->start_tick)
		rel_now = t->replay_now_tick - t->replay->start_tick;

	int cur = (int)(rel_now / bucket_ms);
	if (cur < 0)
		cur = 0;
	if (cur >= (int)t->replay->band_occ_pct_len)
		cur = (int)t->replay->band_occ_pct_len - 1;

	char hdr[96];
	snprintf(hdr, sizeof(hdr), "band occ: %lu b @%lus  cur:%d", (unsigned long)t->replay->band_occ_pct_len,
		 (unsigned long)(bucket_ms / 1000u), cur);
	rf_draw_text(t, (int16_t)(box.x + 2), y, hdr, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	const int win = 8;
	int start = cur - win / 2;
	if (start < 0)
		start = 0;
	int end = start + win - 1;
	if (end >= (int)t->replay->band_occ_pct_len) {
		end = (int)t->replay->band_occ_pct_len - 1;
		start = end - (win - 1);
		if (start < 0)
			start = 0;
	}
	int rows = end - start + 1;
	if (rows <= 0)
		return;

	if (t->analysis_sel < 0)
		t->analysis_sel = 0;
	if (t->analysis_sel >= rows)
		t->analysis_sel = rows - 1;

	int bar_w = max_cols - 16;
	if (bar_w < 6)
		bar_w = 6;
	if (bar_w > 64)
		bar_w = 64;

	for (int i = 0; i < rows; i++) {
		int idx = start + i;
		int pct = (int)t->replay->band_occ_pct[idx];
		if (pct < 0)
			pct = 0;
		if (pct > 100)
			pct = 100;
		int filled = pct * bar_w / 100;
		if (filled > bar_w)
			filled = bar_w;

		char bar[80];
		int bw = 0;
		for (int j = 0; j < bar_w && bw + 1 < (int)sizeof(bar); j++)
			bar[bw++] = (j < filled) ? '#' : '.';
		bar[bw] = 0;

		char line[128];
		snprintf(line, sizeof(line), "m:%04d %3d%% %s", idx, pct, bar);

		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_panel_bg();
		if (idx == cur)
			fg = rf_color_accent();
		if (i == t->analysis_sel && t->focus == RF_FOCUS_ANALYSIS) {
			fg = rf_color_sel_fg();
			bg = rf_color_sel_bg();
		}

		draw_line(t, box, (int16_t)(y + (int16_t)i * RF_FONT_H), line, fg, bg, max_cols);
	}
}

static void render_annotations(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	(void)t;
	rf_draw_text(t, (int16_t)(box.x + 2), y, "(annotations: not implemented)", rf_color_dim(), rf_color_panel_bg(), max_cols);
}

static void render_diagnostics(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;
	char age[24];
	snprintf(age, sizeof(age), "never");
	if (t->diag_last_run_tick != 0 && t->now_tick > t->diag_last_run_tick)
		snprintf(age, sizeof(age), "%ds ago", (int)((t->now_tick - t->diag_last_run_tick) / 1000u));

	char h0[64];
	snprintf(h0, sizeof(h0), "Enter: run diagnostics  last: %s", age);
	rf_draw_text(t, (int16_t)(box.x + 2), y, h0, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	char lines[4][96];
	snprintf(lines[0], sizeof(lines[0]), "RF      : %s", t->diag_rf_ok ? "OK" : "FAIL");
	snprintf(lines[1], sizeof(lines[1]), "SPI     : %s", t->diag_spi_ok ? "OK" : "FAIL");
	snprintf(lines[2], sizeof(lines[2]), "TICK    : %s  avg:%lu min:%lu max:%lu", t->diag_timing_ok ? "OK" : "FAIL",
		 (unsigned long)t->tick_stats_avg_ms, (unsigned long)t->tick_stats_min_ms, (unsigned long)t->tick_stats_max_ms);
	snprintf(lines[3], sizeof(lines[3]), "STAB    : %d/100  drop:%lu recerr:%c", t->diag_stability_score, (unsigned long)t->pkt_dropped,
		 t->record_err[0] ? 'Y' : 'N');

	for (int i = 0; i < 4; i++) {
		int16_t yy = (int16_t)(y + (int16_t)i * RF_FONT_H);
		if (yy + RF_FONT_H > box.y + box.h)
			return;
		rf_draw_text(t, (int16_t)(box.x + 2), yy, lines[i], rf_color_fg(), rf_color_panel_bg(), max_cols);
	}
}

static void render_stress(struct rf_task *t, struct rf_rect box, int16_t y, int max_cols)
{
	if (!t)
		return;

	int ch = rf_clamp_int(t->selected_channel, 0, RF_MAX_CHANNEL);
	char hdr[64];
	snprintf(hdr, sizeof(hdr), "stress: ch:%03d rate:%s", ch, rf_data_rate_str(t->data_rate));
	rf_draw_text(t, (int16_t)(box.x + 2), y, hdr, rf_color_dim(), rf_color_panel_bg(), max_cols);
	y = (int16_t)(y + RF_FONT_H);

	const char *run = t->stress_running ? "ON" : "OFF";
	char dur[24];
	snprintf(dur, sizeof(dur), "manual");
	if (t->stress_duration_ms > 0)
		snprintf(dur, sizeof(dur), "%dms", t->stress_duration_ms);

	int loss_pct = 0;
	if (t->stress_sent > 0)
		loss_pct = (int)(t->stress_lost * 100u / t->stress_sent);

	char stats[96];
	snprintf(stats, sizeof(stats), "sent:%lu rx:%lu lost:%lu (%d%%) lat:%lu/%lums", (unsigned long)t->stress_sent,
		 (unsigned long)t->stress_recv, (unsigned long)t->stress_lost, loss_pct, (unsigned long)t->stress_lat_avg_ms,
		 (unsigned long)t->stress_lat_max_ms);

	char lines[4][96];
	snprintf(lines[0], sizeof(lines[0]), "RUN     <%s>", run);
	snprintf(lines[1], sizeof(lines[1]), "PPS     [%d]", rf_clamp_int(t->stress_pps, 1, 1000));
	snprintf(lines[2], sizeof(lines[2]), "DURms   [%s]", dur);
	snprintf(lines[3], sizeof(lines[3]), "%s", stats);

	if (t->analysis_sel < 0)
		t->analysis_sel = 0;
	if (t->analysis_sel >= 4)
		t->analysis_sel = 3;

	for (int i = 0; i < 4; i++) {
		int16_t yy = (int16_t)(y + (int16_t)i * RF_FONT_H);
		struct rf_color fg = rf_color_fg();
		struct rf_color bg = rf_color_panel_bg();
		if (i == t->analysis_sel && t->focus == RF_FOCUS_ANALYSIS) {
			fg = rf_color_sel_fg();
			bg = rf_color_sel_bg();
		}
		draw_line(t, box, yy, lines[i], fg, bg, max_cols);
	}
}

void rf_analysis_render_contents(struct rf_task *t, struct rf_layout l)
{
	if (!t)
		return;
	if (l.analysis.w <= 0 || l.analysis.h <= 0 || l.analysis_cols <= 0)
		return;

	struct rf_rect inner = rf_rect_inset(l.analysis, 2, 2);
	int max_cols = (int)(inner.w / RF_FONT_W);
	if (max_cols <= 0)
		return;

	char tabs[192];
	tabs_line(t, tabs, sizeof(tabs));
	int16_t y0 = (int16_t)(inner.y + RF_FONT_H + 1);
	rf_draw_text(t, (int16_t)(inner.x + 2), y0, tabs, rf_color_dim(), rf_color_panel_bg(), max_cols);

	int16_t y = (int16_t)(y0 + RF_FONT_H);
	if (y + RF_FONT_H > inner.y + inner.h)
		return;

	switch (t->analysis_view) {
	case RF_ANALYSIS_CHANNELS:
		render_channels(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_DEVICES:
		render_devices(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_TIMING:
		render_timing(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_COLLISIONS:
		render_collisions(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_CORRELATION:
		render_correlation(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_COMPARISON:
		render_comparison(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_MONITORING:
		render_monitoring(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_ANNOTATIONS:
		render_annotations(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_DIAGNOSTICS:
		render_diagnostics(t, inner, y, max_cols);
		return;
	case RF_ANALYSIS_STRESS:
		render_stress(t, inner, y, max_cols);
		return;
	default:
		rf_draw_text(t, (int16_t)(inner.x + 2), y, "(unknown view)", rf_color_dim(), rf_color_panel_bg(), max_cols);
		return;
	}
}
