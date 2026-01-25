#include "rf_menu.h"

#include "rf_annotations.h"
#include "rf_automation.h"
#include "rf_keys.h"
#include "rf_preset_profiles.h"
#include "rf_prompt.h"
#include "rf_recording.h"
#include "rf_replay.h"
#include "rf_selection.h"
#include "rf_session.h"
#include "rf_task.h"
#include "rf_view.h"
#include "rf_waterfall.h"

#include <stdio.h>
#include <string.h>

static void activate_menu_item(struct rf_task *t, enum rf_menu_item_id id)
{
	if (!t)
		return;

	switch (id) {
	case RF_MENU_ITEM_FOCUS_SPECTRUM:
		t->focus = RF_FOCUS_SPECTRUM;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_HEADER);
		return;
	case RF_MENU_ITEM_FOCUS_WATERFALL:
		t->focus = RF_FOCUS_WATERFALL;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_HEADER);
		return;
	case RF_MENU_ITEM_FOCUS_RFCONTROL:
		t->focus = RF_FOCUS_RFCONTROL;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_HEADER);
		return;
	case RF_MENU_ITEM_FOCUS_SNIFFER:
		t->focus = RF_FOCUS_SNIFFER;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_HEADER);
		return;
	case RF_MENU_ITEM_FOCUS_PROTOCOL:
		t->focus = RF_FOCUS_PROTOCOL;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_HEADER);
		return;
	case RF_MENU_ITEM_FOCUS_ANALYSIS:
		t->focus = RF_FOCUS_ANALYSIS;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_HEADER);
		return;

	case RF_MENU_ITEM_TOGGLE_SCAN:
		if (t->scan_active) {
			t->scan_active = 0;
			t->scan_next_tick = 0;
			rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
			return;
		}
		t->scan_active = 1;
		t->scan_chan = t->channel_range_lo;
		t->scan_next_tick = 0;
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_OVERLAY);
		return;

	case RF_MENU_ITEM_TOGGLE_WATERFALL:
		t->waterfall_frozen = !t->waterfall_frozen;
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_WATERFALL | RF_DIRTY_OVERLAY);
		return;

	case RF_MENU_ITEM_TOGGLE_CAPTURE:
		t->capture_paused = !t->capture_paused;
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_SNIFFER | RF_DIRTY_OVERLAY);
		return;

	case RF_MENU_ITEM_TOGGLE_RECORDING:
		if (t->recording) {
			(void)rf_recording_stop(t, NULL, 0);
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		{
			const char *initial = t->record_name[0] ? t->record_name : "session";
			rf_prompt_open(t, RF_PROMPT_START_RECORDING, "Start recording session name", initial);
			rf_menu_close(t);
			return;
		}

	case RF_MENU_ITEM_ADD_ANNOT_NOW:
	case RF_MENU_ITEM_ADD_ANNOT_SELECTED:
		{
			uint64_t tick = t->now_tick;
			if (t->replay_active)
				tick = t->replay_now_tick;
			if (id == RF_MENU_ITEM_ADD_ANNOT_SELECTED) {
				uint64_t pkt_tick = 0;
				if (rf_selection_selected_packet_tick(t, &pkt_tick))
					tick = pkt_tick;
			}
			rf_annotations_begin(t, tick);
			rf_menu_close(t);
			return;
		}

	case RF_MENU_ITEM_AUTOMATION_ARM:
		rf_automation_toggle_arm(t);
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL | RF_DIRTY_OVERLAY);
		rf_menu_close(t);
		return;

	case RF_MENU_ITEM_LOAD_SESSION:
		{
			const char *initial = "session";
			if (t->replay)
				initial = t->replay->name;
			rf_prompt_open(t, RF_PROMPT_LOAD_SESSION, "Load session name (from /rf/sessions)", initial);
			rf_menu_close(t);
			return;
		}
	case RF_MENU_ITEM_EXIT_REPLAY:
		rf_replay_exit(t);
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_ALL);
		return;
	case RF_MENU_ITEM_REPLAY_PLAY_PAUSE:
		if (t->replay_active) {
			t->replay_playing = !t->replay_playing;
			rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		}
		return;
	case RF_MENU_ITEM_REPLAY_SEEK:
		if (!t->replay_active)
			return;
		{
			int offset_ms = 0;
			if (t->replay && t->replay_now_tick >= t->replay->start_tick)
				offset_ms = (int)(t->replay_now_tick - t->replay->start_tick);
			char initial[24];
			snprintf(initial, sizeof(initial), "%d", offset_ms);
			rf_prompt_open(t, RF_PROMPT_REPLAY_SEEK, "Seek to t(ms) from session start", initial);
			rf_menu_close(t);
			return;
		}
	case RF_MENU_ITEM_REPLAY_SPEED:
		if (!t->replay_active)
			return;
		switch (t->replay_speed) {
		case 1:
			t->replay_speed = 2;
			break;
		case 2:
			t->replay_speed = 4;
			break;
		case 4:
			t->replay_speed = 8;
			break;
		case 8:
			t->replay_speed = 16;
			break;
		default:
			t->replay_speed = 1;
			break;
		}
		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		return;

	case RF_MENU_ITEM_EXPORT_CSV:
		{
			const char *initial = "export";
			if (t->replay)
				initial = t->replay->name;
			rf_prompt_open(t, RF_PROMPT_EXPORT_CSV, "Export CSV name (to /rf/exports)", initial);
			rf_menu_close(t);
			return;
		}
	case RF_MENU_ITEM_EXPORT_PCAP:
		{
			const char *initial = "export";
			if (t->replay)
				initial = t->replay->name;
			rf_prompt_open(t, RF_PROMPT_EXPORT_PCAP, "Export PCAP name (to /rf/exports)", initial);
			rf_menu_close(t);
			return;
		}
	case RF_MENU_ITEM_EXPORT_RFPKT:
		{
			const char *initial = "export";
			if (t->replay)
				initial = t->replay->name;
			rf_prompt_open(t, RF_PROMPT_EXPORT_RFPKT, "Export raw packet dump name (to /rf/exports)", initial);
			rf_menu_close(t);
			return;
		}
	case RF_MENU_ITEM_LOAD_COMPARE_SESSION:
		{
			const char *initial = "session";
			if (t->compare)
				initial = t->compare->name;
			rf_prompt_open(t, RF_PROMPT_LOAD_COMPARE_SESSION, "Load compare session name (from /rf/sessions)", initial);
			rf_menu_close(t);
			return;
		}
	case RF_MENU_ITEM_CLEAR_COMPARE:
		if (t->compare) {
			rf_session_free(t->compare);
			t->compare = NULL;
		}
		t->compare_err[0] = 0;
		rf_task_invalidate(t, RF_DIRTY_ANALYSIS | RF_DIRTY_SPECTRUM | RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
		return;

	case RF_MENU_ITEM_RESET_VIEW:
		rf_view_reset(t);
		return;

	case RF_MENU_ITEM_SET_CHANNEL: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->selected_channel);
		rf_prompt_open(t, RF_PROMPT_SET_CHANNEL, "Set channel (0..125)", initial);
		rf_menu_close(t);
		return;
	}
	case RF_MENU_ITEM_SET_RANGE_LO: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->channel_range_lo);
		rf_prompt_open(t, RF_PROMPT_SET_RANGE_LO, "Set range LO", initial);
		rf_menu_close(t);
		return;
	}
	case RF_MENU_ITEM_SET_RANGE_HI: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->channel_range_hi);
		rf_prompt_open(t, RF_PROMPT_SET_RANGE_HI, "Set range HI", initial);
		rf_menu_close(t);
		return;
	}
	case RF_MENU_ITEM_SET_DWELL: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->dwell_time_ms);
		rf_prompt_open(t, RF_PROMPT_SET_DWELL, "Set dwell time (ms)", initial);
		rf_menu_close(t);
		return;
	}
	case RF_MENU_ITEM_SET_SCAN_STEP: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->scan_speed_scalar, 1, 10));
		rf_prompt_open(t, RF_PROMPT_SET_SCAN_STEP, "Set scan step (1..10)", initial);
		rf_menu_close(t);
		return;
	}

	case RF_MENU_ITEM_CYCLE_RATE:
		t->data_rate = (enum rf_data_rate)rf_wrap_enum((int)t->data_rate + 1, 3);
		t->preset_dirty = 1;
		t->scan_next_tick = 0;
		rf_recording_record_config(t, t->now_tick);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_SPECTRUM | RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		return;
	case RF_MENU_ITEM_CYCLE_CRC:
		t->crc_mode = (enum rf_crc_mode)rf_wrap_enum((int)t->crc_mode + 1, 3);
		t->preset_dirty = 1;
		rf_recording_record_config(t, t->now_tick);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		return;
	case RF_MENU_ITEM_TOGGLE_AUTO_ACK:
		t->auto_ack = !t->auto_ack;
		t->preset_dirty = 1;
		rf_recording_record_config(t, t->now_tick);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		return;
	case RF_MENU_ITEM_CYCLE_POWER:
		t->power_level = (enum rf_power_level)rf_wrap_enum((int)t->power_level + 1, 4);
		t->preset_dirty = 1;
		rf_recording_record_config(t, t->now_tick);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		return;

	case RF_MENU_ITEM_CYCLE_PALETTE:
		t->wf_palette = (enum rf_wf_palette)rf_wrap_enum((int)t->wf_palette + 1, 4);
		rf_waterfall_rebuild_palette(t);
		t->preset_dirty = 1;
		rf_recording_record_config(t, t->now_tick);
		rf_task_invalidate(t, RF_DIRTY_WATERFALL | RF_DIRTY_STATUS | RF_DIRTY_OVERLAY);
		return;

	case RF_MENU_ITEM_TOGGLE_PROTO_MODE:
		t->proto_mode = (t->proto_mode == RF_PROTO_DECODED) ? RF_PROTO_RAW : RF_PROTO_DECODED;
		rf_task_invalidate(t, RF_DIRTY_PROTOCOL | RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
		return;

	case RF_MENU_ITEM_SAVE_PRESET:
		rf_prompt_open(t, RF_PROMPT_SAVE_PRESET, "Save preset name", "scan");
		rf_menu_close(t);
		return;
	case RF_MENU_ITEM_LOAD_PRESET:
		rf_prompt_open(t, RF_PROMPT_LOAD_PRESET, "Load preset name", "scan");
		rf_menu_close(t);
		return;

	case RF_MENU_ITEM_AUTOMATION_CONFIG:
		rf_automation_open(t);
		rf_menu_close(t);
		return;
	case RF_MENU_ITEM_PRESET_PROFILES:
		rf_preset_profiles_open(t);
		rf_menu_close(t);
		return;

	case RF_MENU_ITEM_OPEN_HELP:
		t->show_help = 1;
		rf_menu_close(t);
		rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
		return;

	default:
		return;
	}
}

void rf_menu_open(struct rf_task *t)
{
	if (!t)
		return;
	t->show_menu = 1;
	t->menu_sel = 0;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_HEADER | RF_DIRTY_STATUS);
}

void rf_menu_close(struct rf_task *t)
{
	if (!t || !t->show_menu)
		return;
	t->show_menu = 0;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_HEADER | RF_DIRTY_STATUS);
}

void rf_menu_handle_key(struct rf_task *t, const struct rf_key *k)
{
	if (!t || !k)
		return;

	switch (k->kind) {
	case RF_KEY_ESC:
		rf_menu_close(t);
		return;
	case RF_KEY_RUNE:
		if (k->r == 'm' || k->r == 'M') {
			rf_menu_close(t);
			return;
		}
		break;
	case RF_KEY_LEFT:
		if (t->menu_cat == 0)
			t->menu_cat = RF_MENU_HELP;
		else
			t->menu_cat = (enum rf_menu_category)((int)t->menu_cat - 1);
		t->menu_sel = 0;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_HEADER);
		return;
	case RF_KEY_RIGHT:
		if (t->menu_cat >= RF_MENU_HELP)
			t->menu_cat = 0;
		else
			t->menu_cat = (enum rf_menu_category)((int)t->menu_cat + 1);
		t->menu_sel = 0;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_HEADER);
		return;
	case RF_KEY_UP:
	case RF_KEY_DOWN: {
		int count = 0;
		(void)rf_menu_items(t->menu_cat, &count);
		if (count <= 0)
			return;
		if (k->kind == RF_KEY_UP) {
			if (t->menu_sel <= 0)
				t->menu_sel = count - 1;
			else
				t->menu_sel--;
		} else {
			if (t->menu_sel >= count - 1)
				t->menu_sel = 0;
			else
				t->menu_sel++;
		}
		rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		return;
	}
	case RF_KEY_ENTER: {
		int count = 0;
		const struct rf_menu_item *items = rf_menu_items(t->menu_cat, &count);
		if (!items || count <= 0)
			return;
		if (t->menu_sel < 0 || t->menu_sel >= count)
			return;
		activate_menu_item(t, items[t->menu_sel].id);
		return;
	}
	default:
		break;
	}
}
