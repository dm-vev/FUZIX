#include "rf_task.h"

#include "rf_draw.h"
#include "rf_analytics.h"
#include "rf_automation.h"
#include "rf_diagnostics.h"
#include "rf_filters.h"
#include "rf_keys.h"
#include "rf_layout.h"
#include "rf_menu.h"
#include "rf_prompt.h"
#include "rf_recording.h"
#include "rf_replay.h"
#include "rf_render.h"
#include "rf_scan.h"
#include "rf_session.h"
#include "rf_sniffer.h"
#include "rf_stress.h"
#include "rf_term.h"
#include "rf_view.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t rf_running = 1;

static void on_sig(int sig)
{
	(void)sig;
	rf_running = 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	memset(&ts, 0, sizeof(ts));
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void cycle_focus(struct rf_task *t)
{
	if (!t)
		return;
	t->focus = (enum rf_focus_panel)((int)t->focus + 1);
	if (t->focus > RF_FOCUS_ANALYSIS)
		t->focus = RF_FOCUS_SPECTRUM;
	rf_task_invalidate(t, RF_DIRTY_HEADER);
}

static void prev_analysis_view(struct rf_task *t)
{
	if (!t)
		return;
	t->analysis_view = (enum rf_analysis_view)rf_wrap_enum((int)t->analysis_view - 1, 10);
	t->analysis_sel = 0;
	t->analysis_top = 0;
	rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
}

static void next_analysis_view(struct rf_task *t)
{
	if (!t)
		return;
	t->analysis_view = (enum rf_analysis_view)rf_wrap_enum((int)t->analysis_view + 1, 10);
	t->analysis_sel = 0;
	t->analysis_top = 0;
	rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
}

static void adjust_setting(struct rf_task *t, int delta)
{
	if (!t || delta == 0)
		return;

	switch ((enum rf_setting)t->selected_setting) {
	case RF_SETTING_CHAN_LO:
		t->channel_range_lo = rf_clamp_int(t->channel_range_lo + delta, 0, RF_MAX_CHANNEL);
		if (t->channel_range_lo > t->channel_range_hi)
			t->channel_range_hi = t->channel_range_lo;
		break;
	case RF_SETTING_CHAN_HI:
		t->channel_range_hi = rf_clamp_int(t->channel_range_hi + delta, 0, RF_MAX_CHANNEL);
		if (t->channel_range_hi < t->channel_range_lo)
			t->channel_range_lo = t->channel_range_hi;
		break;
	case RF_SETTING_DWELL:
		t->dwell_time_ms = rf_clamp_int(t->dwell_time_ms + delta, 1, 50);
		break;
	case RF_SETTING_SPEED:
		t->scan_speed_scalar = rf_clamp_int(t->scan_speed_scalar + delta, 1, 10);
		break;
	case RF_SETTING_RATE:
		t->data_rate = (enum rf_data_rate)rf_wrap_enum((int)t->data_rate + delta, 3);
		break;
	case RF_SETTING_CRC:
		t->crc_mode = (enum rf_crc_mode)rf_wrap_enum((int)t->crc_mode + delta, 3);
		break;
	case RF_SETTING_AUTO_ACK:
		t->auto_ack = !t->auto_ack;
		break;
	case RF_SETTING_POWER:
		t->power_level = (enum rf_power_level)rf_wrap_enum((int)t->power_level + delta, 4);
		break;
	default:
		break;
	}
	t->preset_dirty = 1;
	rf_recording_record_config(t, t->now_tick);
	rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS | RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL);
}

static void handle_analysis_enter(struct rf_task *t)
{
	if (!t)
		return;

	switch (t->analysis_view) {
	case RF_ANALYSIS_CHANNELS: {
		struct rf_chan_score top[8];
		int top_n = rf_analytics_top_channels(t, 8, top, (int)(sizeof(top) / sizeof(top[0])));
		if (t->analysis_sel >= 0 && t->analysis_sel < top_n) {
			t->selected_channel = rf_clamp_int(top[t->analysis_sel].ch, 0, RF_MAX_CHANNEL);
			rf_recording_record_config(t, t->now_tick);
			rf_task_invalidate(t, RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS | RF_DIRTY_ANALYSIS);
		}
		return;
	}
	case RF_ANALYSIS_MONITORING: {
		if (!t->replay_active || !t->replay || t->replay->bucket_ms == 0 || t->replay->band_occ_pct_len == 0 ||
		    !t->replay->band_occ_pct)
			return;
		uint64_t bucket_ms = t->replay->bucket_ms;
		uint64_t rel_now = 0;
		if (t->replay_now_tick >= t->replay->start_tick)
			rel_now = t->replay_now_tick - t->replay->start_tick;
		int cur = (int)(rel_now / bucket_ms);
		if (cur < 0)
			cur = 0;
		if (cur >= (int)t->replay->band_occ_pct_len)
			cur = (int)t->replay->band_occ_pct_len - 1;

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
		if (t->analysis_sel < 0 || t->analysis_sel >= rows)
			return;
		int idx = start + t->analysis_sel;
		uint64_t off_ms = (uint64_t)idx * bucket_ms;
		rf_replay_seek_ms(t, off_ms);
		rf_task_invalidate(t, RF_DIRTY_ALL);
		return;
	}
	case RF_ANALYSIS_ANNOTATIONS: {
		if (!t->replay_active || !t->replay)
			return;
		struct rf_annotation notes[8];
		int n = rf_annotations_visible(t, t->replay_now_tick, notes, (int)(sizeof(notes) / sizeof(notes[0])));
		if (t->analysis_sel < 0 || t->analysis_sel >= n)
			return;
		struct rf_annotation a = notes[t->analysis_sel];
		if (a.start_tick == 0)
			return;
		uint64_t off_ms = 0;
		if (a.start_tick > t->replay->start_tick)
			off_ms = a.start_tick - t->replay->start_tick;
		rf_replay_seek_ms(t, off_ms);
		rf_task_invalidate(t, RF_DIRTY_ALL);
		return;
	}
	case RF_ANALYSIS_DIAGNOSTICS:
		rf_diagnostics_run(t, t->now_tick);
		return;
	case RF_ANALYSIS_STRESS:
		switch (t->analysis_sel) {
		case 0:
			if (t->stress_running)
				rf_stress_stop(t);
			else
				rf_stress_start(t, t->now_tick);
			return;
		case 1: {
			char initial[16];
			snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->stress_pps, 1, 1000));
			rf_prompt_open(t, RF_PROMPT_STRESS_PPS, "Stress packets/sec (1..1000)", initial);
			return;
		}
		case 2: {
			char initial[16];
			snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->stress_duration_ms, 0, 1000000));
			rf_prompt_open(t, RF_PROMPT_STRESS_DURATION, "Stress duration (ms, 0=manual)", initial);
			return;
		}
		default:
			return;
		}
	default:
		return;
	}
}

static void handle_key(struct rf_task *t, const struct rf_key *k)
{
	if (!t || !k)
		return;

	if (t->show_prompt) {
		rf_prompt_handle_key(t, k);
		return;
	}
	if (t->show_help) {
		if (k->kind == RF_KEY_ESC || (k->kind == RF_KEY_RUNE && (k->r == 'h' || k->r == 'H'))) {
			t->show_help = 0;
			rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
		}
		return;
	}
	if (t->show_menu) {
		rf_menu_handle_key(t, k);
		return;
	}
	if (t->show_filters) {
		rf_filters_handle_key(t, k);
		return;
	}
	if (t->show_automation) {
		rf_automation_handle_key(t, k);
		return;
	}

	switch (k->kind) {
	case RF_KEY_ESC:
		rf_running = 0;
		return;
	case RF_KEY_ENTER:
		switch (t->focus) {
		case RF_FOCUS_RFCONTROL:
			switch ((enum rf_setting)t->selected_setting) {
			case RF_SETTING_CHAN_LO: {
				char initial[16];
				snprintf(initial, sizeof(initial), "%d", t->channel_range_lo);
				rf_prompt_open(t, RF_PROMPT_SET_RANGE_LO, "Set range LO", initial);
				break;
			}
			case RF_SETTING_CHAN_HI: {
				char initial[16];
				snprintf(initial, sizeof(initial), "%d", t->channel_range_hi);
				rf_prompt_open(t, RF_PROMPT_SET_RANGE_HI, "Set range HI", initial);
				break;
			}
			case RF_SETTING_DWELL: {
				char initial[16];
				snprintf(initial, sizeof(initial), "%d", t->dwell_time_ms);
				rf_prompt_open(t, RF_PROMPT_SET_DWELL, "Set dwell time (ms)", initial);
				break;
			}
			case RF_SETTING_SPEED: {
				char initial[16];
				snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->scan_speed_scalar, 1, 10));
				rf_prompt_open(t, RF_PROMPT_SET_SCAN_STEP, "Set scan step (1..10)", initial);
				break;
			}
			case RF_SETTING_RATE:
				t->data_rate = (enum rf_data_rate)rf_wrap_enum((int)t->data_rate + 1, 3);
				t->preset_dirty = 1;
				rf_recording_record_config(t, t->now_tick);
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			case RF_SETTING_CRC:
				t->crc_mode = (enum rf_crc_mode)rf_wrap_enum((int)t->crc_mode + 1, 3);
				t->preset_dirty = 1;
				rf_recording_record_config(t, t->now_tick);
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			case RF_SETTING_AUTO_ACK:
				t->auto_ack = !t->auto_ack;
				t->preset_dirty = 1;
				rf_recording_record_config(t, t->now_tick);
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			case RF_SETTING_POWER:
				t->power_level = (enum rf_power_level)rf_wrap_enum((int)t->power_level + 1, 4);
				t->preset_dirty = 1;
				rf_recording_record_config(t, t->now_tick);
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			default:
				break;
			}
			return;
		case RF_FOCUS_SPECTRUM:
		case RF_FOCUS_WATERFALL: {
			char initial[16];
			snprintf(initial, sizeof(initial), "%d", t->selected_channel);
			rf_prompt_open(t, RF_PROMPT_SET_CHANNEL, "Set selected channel", initial);
			return;
		}
		case RF_FOCUS_ANALYSIS:
			handle_analysis_enter(t);
			return;
		default:
			return;
		}
		return;
	case RF_KEY_LEFT:
		switch (t->focus) {
		case RF_FOCUS_SPECTRUM:
		case RF_FOCUS_WATERFALL:
			t->selected_channel--;
			if (t->selected_channel < 0)
				t->selected_channel = RF_MAX_CHANNEL;
			rf_task_invalidate(t, RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
			return;
		case RF_FOCUS_RFCONTROL:
			adjust_setting(t, -1);
			return;
		case RF_FOCUS_ANALYSIS:
			prev_analysis_view(t);
			return;
		default:
			return;
		}
	case RF_KEY_RIGHT:
		switch (t->focus) {
		case RF_FOCUS_SPECTRUM:
		case RF_FOCUS_WATERFALL:
			t->selected_channel++;
			if (t->selected_channel > RF_MAX_CHANNEL)
				t->selected_channel = 0;
			rf_task_invalidate(t, RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
			return;
		case RF_FOCUS_RFCONTROL:
			adjust_setting(t, +1);
			return;
		case RF_FOCUS_ANALYSIS:
			next_analysis_view(t);
			return;
		default:
			return;
		}
	case RF_KEY_UP:
		if (t->focus == RF_FOCUS_RFCONTROL) {
			if (t->selected_setting > 0)
				t->selected_setting--;
			rf_task_invalidate(t, RF_DIRTY_RFCONTROL);
		} else if (t->focus == RF_FOCUS_SNIFFER) {
			rf_sniffer_move_selection(t, -1);
		} else if (t->focus == RF_FOCUS_PROTOCOL) {
			rf_task_invalidate(t, RF_DIRTY_PROTOCOL);
		} else if (t->focus == RF_FOCUS_ANALYSIS) {
			if (t->analysis_sel > 0) {
				t->analysis_sel--;
				rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
			}
		}
		return;
	case RF_KEY_DOWN:
		if (t->focus == RF_FOCUS_RFCONTROL) {
			if (t->selected_setting < (int)RF_SETTING_MAX - 1)
				t->selected_setting++;
			rf_task_invalidate(t, RF_DIRTY_RFCONTROL);
		} else if (t->focus == RF_FOCUS_SNIFFER) {
			rf_sniffer_move_selection(t, +1);
		} else if (t->focus == RF_FOCUS_PROTOCOL) {
			rf_task_invalidate(t, RF_DIRTY_PROTOCOL);
		} else if (t->focus == RF_FOCUS_ANALYSIS) {
			t->analysis_sel++;
			rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
		}
		return;
	case RF_KEY_RUNE:
		break;
	default:
		return;
	}

	switch (k->r) {
	case 'q':
	case 'Q':
		rf_running = 0;
		return;
	case 's':
	case 'S':
		if (t->replay_active) {
			t->replay_playing = !t->replay_playing;
			rf_task_invalidate(t, RF_DIRTY_STATUS);
			return;
		}
		t->scan_active = !t->scan_active;
		t->scan_next_tick = 0;
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL);
		return;
	case 'w':
	case 'W':
		t->waterfall_frozen = !t->waterfall_frozen;
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_WATERFALL);
		return;
	case 'p':
	case 'P':
		if (t->replay_active) {
			t->replay_playing = !t->replay_playing;
			rf_task_invalidate(t, RF_DIRTY_STATUS);
			return;
		}
		t->capture_paused = !t->capture_paused;
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_SNIFFER);
		return;
	case 'r':
	case 'R':
		if (t->replay_active)
			rf_replay_reset_view(t);
		else
			rf_view_reset(t);
		return;
	case 'm':
	case 'M':
		if (t->show_menu)
			rf_menu_close(t);
		else
			rf_menu_open(t);
		return;
	case 't':
	case 'T':
		cycle_focus(t);
		return;
	case 'c':
	case 'C': {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->selected_channel);
		rf_prompt_open(t, RF_PROMPT_SET_CHANNEL, "Set selected channel", initial);
		return;
	}
	case 'f':
	case 'F':
		rf_filters_toggle(t);
		return;
	case 'h':
	case 'H':
		t->show_help = 1;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
		return;
	default:
		return;
	}
}

int rf_task_init(struct rf_task *t, int fb_mode, char *err, size_t errsz)
{
	if (!t) {
		snprintf(err, errsz, "rf: bad args");
		return -1;
	}
	memset(t, 0, sizeof(*t));

	t->record_fd = -1;

	t->active = 1;
	t->focus = RF_FOCUS_SPECTRUM;
	t->dirty = RF_DIRTY_ALL;

	/* Spark defaults (task.go New()). */
	t->selected_channel = 37;
	t->channel_range_lo = 0;
	t->channel_range_hi = RF_MAX_CHANNEL;
	t->dwell_time_ms = 5;
	t->scan_speed_scalar = 1;
	t->data_rate = RF_RATE_2M;
	t->crc_mode = RF_CRC_2B;
	t->auto_ack = 0;
	t->power_level = RF_PWR_MAX;
	t->wf_palette = RF_WF_PAL_CYAN;
	t->rng = 0xA341316Cu;
	t->stress_pps = 200;
	t->stress_duration_ms = 10000;
	t->replay_speed = 1;
	t->menu_cat = RF_MENU_RF;
	t->proto_mode = RF_PROTO_DECODED;
	t->analysis_view = RF_ANALYSIS_CHANNELS;
	t->filter_crc = RF_FILTER_CRC_ANY;
	t->filter_channel = RF_FILTER_CH_ALL;
	t->auto_ack = 0;
	t->auto_record = 1;
	snprintf(t->auto_session_base, sizeof(t->auto_session_base), "auto");

	if (rf_fb_open(&t->fb, fb_mode, err, errsz) != 0)
		return -1;

	t->cols = (int)t->fb.disp.width / RF_FONT_W;
	t->rows = (int)t->fb.disp.height / RF_FONT_H;
	t->main_rows = t->rows - RF_HEADER_ROWS - RF_STATUS_ROWS;
	if (t->cols <= 0 || t->rows <= 0 || t->main_rows <= 0) {
		snprintf(err, errsz, "rf: unsupported fb geometry %ux%u",
			 (unsigned)t->fb.disp.width, (unsigned)t->fb.disp.height);
		rf_fb_close(&t->fb);
		return -1;
	}

	char fb_err[128];
	if (rf_fb_activate(&t->fb, fb_err, sizeof(fb_err)) != 0) {
		snprintf(err, errsz, "%s", fb_err);
		rf_fb_close(&t->fb);
		return -1;
	}

	if (rf_term_setup_stdin(err, errsz) != 0) {
		rf_fb_close(&t->fb);
		return -1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	return 0;
}

int rf_task_run(struct rf_task *t)
{
	if (!t)
		return 1;

	t->now_tick = now_ms();
	t->next_render_tick = t->now_tick;

	/* Initial frame. */
	t->dirty = RF_DIRTY_ALL;
	rf_render_dirty(t);

	while (rf_running) {
		uint8_t tmp[32];
		ssize_t n = read(0, tmp, sizeof(tmp));
		if (n > 0) {
			if (t->inlen + (size_t)n > sizeof(t->inbuf))
				t->inlen = 0;
			memcpy(t->inbuf + t->inlen, tmp, (size_t)n);
			t->inlen += (size_t)n;
		}

		for (;;) {
			struct rf_key k;
			size_t consumed = 0;
			if (!rf_next_key(t->inbuf, t->inlen, &consumed, &k))
				break;
			if (consumed == 0 || consumed > t->inlen)
				break;
			memmove(t->inbuf, t->inbuf + consumed, t->inlen - consumed);
			t->inlen -= consumed;

			handle_key(t, &k);
			if (!rf_running)
				break;
		}

		uint64_t tick = now_ms();
		t->now_tick = tick;
		rf_tick_stats_update(t, tick);
		rf_automation_tick(t, tick);
		if (t->replay_active)
			rf_replay_tick(t, tick);
		else
			rf_scan_tick(t, tick);
		if (t->replay_active)
			rf_replay_update_packet_cache(t);
		rf_stress_tick(t, tick);
		rf_sniffer_tick_pps(t, tick);
		rf_recording_flush(t, tick, 0);

		if (t->dirty && tick >= t->next_render_tick) {
			rf_render_dirty(t);
			t->next_render_tick = tick + RF_RENDER_INTERVAL_TICKS;
		}

		if (t->inlen == 0)
			usleep(10000);
	}

	return 0;
}

void rf_task_destroy(struct rf_task *t)
{
	if (!t)
		return;
	rf_term_restore_stdin();
	t->now_tick = now_ms();
	rf_recording_stop(t, NULL, 0);
	rf_replay_exit(t);
	if (t->compare) {
		rf_session_free(t->compare);
		t->compare = NULL;
	}
	free(t->wf_buf);
	t->wf_buf = NULL;
	t->wf_cap = 0;
	free(t->record_buf);
	t->record_buf = NULL;
	t->record_cap = 0;
	t->record_len = 0;
	rf_fb_close(&t->fb);
}
