#include "rf_task.h"

#include "rf_draw.h"
#include "rf_filters.h"
#include "rf_keys.h"
#include "rf_layout.h"
#include "rf_menu.h"
#include "rf_prompt.h"
#include "rf_recording.h"
#include "rf_replay.h"
#include "rf_render.h"
#include "rf_scan.h"
#include "rf_sniffer.h"
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

	switch (k->kind) {
	case RF_KEY_ESC:
		rf_running = 0;
		return;
	case RF_KEY_ENTER:
		if (t->focus == RF_FOCUS_RFCONTROL) {
			/* Enter toggles/cycles select options like Spark's handleEnter(). */
			switch ((enum rf_setting)t->selected_setting) {
			case RF_SETTING_RATE:
				t->data_rate = (enum rf_data_rate)rf_wrap_enum((int)t->data_rate + 1, 3);
				t->preset_dirty = 1;
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			case RF_SETTING_CRC:
				t->crc_mode = (enum rf_crc_mode)rf_wrap_enum((int)t->crc_mode + 1, 3);
				t->preset_dirty = 1;
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			case RF_SETTING_AUTO_ACK:
				t->auto_ack = !t->auto_ack;
				t->preset_dirty = 1;
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			case RF_SETTING_POWER:
				t->power_level = (enum rf_power_level)rf_wrap_enum((int)t->power_level + 1, 4);
				t->preset_dirty = 1;
				rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
				break;
			default:
				break;
			}
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
		}
		return;
	case RF_KEY_DOWN:
		if (t->focus == RF_FOCUS_RFCONTROL) {
			if (t->selected_setting < (int)RF_SETTING_MAX - 1)
				t->selected_setting++;
			rf_task_invalidate(t, RF_DIRTY_RFCONTROL);
		} else if (t->focus == RF_FOCUS_SNIFFER) {
			rf_sniffer_move_selection(t, +1);
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
		if (t->replay_active)
			rf_replay_tick(t, tick);
		else
			rf_scan_tick(t, tick);
		if (t->replay_active)
			rf_replay_update_packet_cache(t);
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
	free(t->wf_buf);
	t->wf_buf = NULL;
	t->wf_cap = 0;
	free(t->record_buf);
	t->record_buf = NULL;
	t->record_cap = 0;
	t->record_len = 0;
	rf_fb_close(&t->fb);
}
