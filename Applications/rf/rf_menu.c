#include "rf_menu.h"

#include "rf_replay.h"
#include "rf_session.h"
#include "rf_task.h"

#include <stdio.h>

static const struct rf_menu_item menu_view_items[] = {
	{RF_MENU_ITEM_FOCUS_SPECTRUM, "Focus Spectrum"},
	{RF_MENU_ITEM_FOCUS_WATERFALL, "Focus Waterfall"},
	{RF_MENU_ITEM_FOCUS_RFCONTROL, "Focus RF Control"},
	{RF_MENU_ITEM_FOCUS_SNIFFER, "Focus Sniffer"},
	{RF_MENU_ITEM_FOCUS_PROTOCOL, "Focus Protocol"},
	{RF_MENU_ITEM_FOCUS_ANALYSIS, "Focus Analysis"},
	{RF_MENU_ITEM_RESET_VIEW, "Reset View"},
};

static const struct rf_menu_item menu_rf_items[] = {
	{RF_MENU_ITEM_SET_CHANNEL, "Set Selected Channel…"},
	{RF_MENU_ITEM_SET_RANGE_LO, "Set Range LO…"},
	{RF_MENU_ITEM_SET_RANGE_HI, "Set Range HI…"},
	{RF_MENU_ITEM_SET_DWELL, "Set Dwell (ms)…"},
	{RF_MENU_ITEM_SET_SCAN_STEP, "Set Scan Step…"},
	{RF_MENU_ITEM_CYCLE_RATE, "Data Rate"},
	{RF_MENU_ITEM_CYCLE_CRC, "CRC"},
	{RF_MENU_ITEM_TOGGLE_AUTO_ACK, "Auto-Ack"},
	{RF_MENU_ITEM_CYCLE_POWER, "Power"},
};

static const struct rf_menu_item menu_capture_items[] = {
	{RF_MENU_ITEM_TOGGLE_SCAN, "Start/Stop Scan"},
	{RF_MENU_ITEM_TOGGLE_WATERFALL, "Freeze/Resume Waterfall"},
	{RF_MENU_ITEM_TOGGLE_CAPTURE, "Pause/Resume Capture"},
	{RF_MENU_ITEM_TOGGLE_RECORDING, "Start/Stop Recording…"},
	{RF_MENU_ITEM_ADD_ANNOT_NOW, "Add Annotation @Now…"},
	{RF_MENU_ITEM_ADD_ANNOT_SELECTED, "Add Annotation @Selected…"},
	{RF_MENU_ITEM_AUTOMATION_ARM, "Automation Arm/Disarm"},
	{RF_MENU_ITEM_LOAD_SESSION, "Load Session (Replay)…"},
	{RF_MENU_ITEM_LOAD_COMPARE_SESSION, "Load Compare Session…"},
	{RF_MENU_ITEM_CLEAR_COMPARE, "Clear Compare Session"},
	{RF_MENU_ITEM_REPLAY_PLAY_PAUSE, "Replay Play/Pause"},
	{RF_MENU_ITEM_REPLAY_SEEK, "Replay Seek…"},
	{RF_MENU_ITEM_REPLAY_SPEED, "Replay Speed"},
	{RF_MENU_ITEM_EXPORT_CSV, "Export CSV…"},
	{RF_MENU_ITEM_EXPORT_PCAP, "Export PCAP (DLT_USER0)…"},
	{RF_MENU_ITEM_EXPORT_RFPKT, "Export Raw Packets…"},
	{RF_MENU_ITEM_EXIT_REPLAY, "Exit Replay (Live)"},
};

static const struct rf_menu_item menu_decode_items[] = {
	{RF_MENU_ITEM_TOGGLE_PROTO_MODE, "Protocol View Mode"},
};

static const struct rf_menu_item menu_display_items[] = {
	{RF_MENU_ITEM_CYCLE_PALETTE, "Waterfall Palette"},
};

static const struct rf_menu_item menu_advanced_items[] = {
	{RF_MENU_ITEM_AUTOMATION_CONFIG, "Automation / Monitoring…"},
	{RF_MENU_ITEM_PRESET_PROFILES, "Preset Profiles…"},
	{RF_MENU_ITEM_SAVE_PRESET, "Save Preset…"},
	{RF_MENU_ITEM_LOAD_PRESET, "Load Preset…"},
};

static const struct rf_menu_item menu_help_items[] = {
	{RF_MENU_ITEM_OPEN_HELP, "Hotkeys / Help"},
};

const char *rf_menu_category_label(enum rf_menu_category cat)
{
	switch (cat) {
	case RF_MENU_VIEW:
		return "View";
	case RF_MENU_RF:
		return "RF";
	case RF_MENU_CAPTURE:
		return "Capture";
	case RF_MENU_DECODE:
		return "Decode";
	case RF_MENU_DISPLAY:
		return "Display";
	case RF_MENU_ADVANCED:
		return "Advanced";
	case RF_MENU_HELP:
		return "Help";
	default:
		return "?";
	}
}

const struct rf_menu_item *rf_menu_items(enum rf_menu_category cat, int *count)
{
	if (count)
		*count = 0;

	switch (cat) {
	case RF_MENU_VIEW:
		if (count)
			*count = (int)(sizeof(menu_view_items) / sizeof(menu_view_items[0]));
		return menu_view_items;
	case RF_MENU_RF:
		if (count)
			*count = (int)(sizeof(menu_rf_items) / sizeof(menu_rf_items[0]));
		return menu_rf_items;
	case RF_MENU_CAPTURE:
		if (count)
			*count = (int)(sizeof(menu_capture_items) / sizeof(menu_capture_items[0]));
		return menu_capture_items;
	case RF_MENU_DECODE:
		if (count)
			*count = (int)(sizeof(menu_decode_items) / sizeof(menu_decode_items[0]));
		return menu_decode_items;
	case RF_MENU_DISPLAY:
		if (count)
			*count = (int)(sizeof(menu_display_items) / sizeof(menu_display_items[0]));
		return menu_display_items;
	case RF_MENU_ADVANCED:
		if (count)
			*count = (int)(sizeof(menu_advanced_items) / sizeof(menu_advanced_items[0]));
		return menu_advanced_items;
	case RF_MENU_HELP:
		if (count)
			*count = (int)(sizeof(menu_help_items) / sizeof(menu_help_items[0]));
		return menu_help_items;
	default:
		return NULL;
	}
}

void rf_menu_item_line(const struct rf_task *t, struct rf_menu_item it, char *out, unsigned outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;

	const char *label = it.label ? it.label : "";
	if (!t) {
		snprintf(out, outsz, "%s", label);
		return;
	}

	switch (it.id) {
	case RF_MENU_ITEM_TOGGLE_SCAN:
		snprintf(out, outsz, "%s  [%s]", label, t->scan_active ? "ON" : "OFF");
		return;
	case RF_MENU_ITEM_TOGGLE_WATERFALL:
		snprintf(out, outsz, "%s  [%s]", label, t->waterfall_frozen ? "FROZEN" : "RUN");
		return;
	case RF_MENU_ITEM_TOGGLE_CAPTURE:
		snprintf(out, outsz, "%s  [%s]", label, t->capture_paused ? "PAUSED" : "LIVE");
		return;
	case RF_MENU_ITEM_TOGGLE_RECORDING:
		if (t->record_err[0])
			snprintf(out, outsz, "%s  [ERR]", label);
		else
			snprintf(out, outsz, "%s  [%s]", label, t->recording ? "ON" : "OFF");
		return;
	case RF_MENU_ITEM_LOAD_SESSION:
		if (t->replay_active && t->replay)
			snprintf(out, outsz, "%s  <%s>", label, t->replay->name);
		else
			snprintf(out, outsz, "%s  [LIVE]", label);
		return;
	case RF_MENU_ITEM_LOAD_COMPARE_SESSION:
		snprintf(out, outsz, "%s  [OFF]", label);
		return;
	case RF_MENU_ITEM_CLEAR_COMPARE:
		snprintf(out, outsz, "%s  [N/A]", label);
		return;
	case RF_MENU_ITEM_REPLAY_PLAY_PAUSE:
		if (!t->replay_active)
			snprintf(out, outsz, "%s  [N/A]", label);
		else
			snprintf(out, outsz, "%s  [%s]", label, t->replay_playing ? "PLAY" : "PAUSE");
		return;
	case RF_MENU_ITEM_REPLAY_SEEK:
		if (!t->replay_active)
			snprintf(out, outsz, "%s  [N/A]", label);
		else {
			char tt[24];
			rf_replay_time_text(t, tt, sizeof(tt));
			snprintf(out, outsz, "%s  (%s)", label, tt);
		}
		return;
	case RF_MENU_ITEM_REPLAY_SPEED:
		if (!t->replay_active)
			snprintf(out, outsz, "%s  [N/A]", label);
		else
			snprintf(out, outsz, "%s  <x%d>", label, rf_clamp_int(t->replay_speed, 1, 32));
		return;
	case RF_MENU_ITEM_EXIT_REPLAY:
		snprintf(out, outsz, "%s  [%s]", label, t->replay_active ? "OK" : "N/A");
		return;
	case RF_MENU_ITEM_CYCLE_RATE:
		snprintf(out, outsz, "%s  <%s>", label, rf_data_rate_str(t->data_rate));
		return;
	case RF_MENU_ITEM_CYCLE_CRC:
		snprintf(out, outsz, "%s  <%s>", label, rf_crc_mode_str(t->crc_mode));
		return;
	case RF_MENU_ITEM_TOGGLE_AUTO_ACK:
		snprintf(out, outsz, "%s  [%c]", label, t->auto_ack ? 'x' : ' ');
		return;
	case RF_MENU_ITEM_CYCLE_POWER:
		snprintf(out, outsz, "%s  <%s>", label, rf_power_level_str(t->power_level));
		return;
	case RF_MENU_ITEM_CYCLE_PALETTE:
		snprintf(out, outsz, "%s  <%s>", label, rf_wf_palette_str(t->wf_palette));
		return;
	case RF_MENU_ITEM_TOGGLE_PROTO_MODE:
		snprintf(out, outsz, "%s  <%s>", label, rf_protocol_mode_str(t->proto_mode));
		return;
	default:
		snprintf(out, outsz, "%s", label);
		return;
	}
}
