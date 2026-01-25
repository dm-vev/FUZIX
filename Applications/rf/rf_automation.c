#include "rf_automation.h"

#include "rf_fs.h"
#include "rf_keys.h"
#include "rf_prompt.h"
#include "rf_recording.h"
#include "rf_task.h"

#include <stdio.h>
#include <string.h>

enum { automation_lines = 7 };

static void start_scan(struct rf_task *t)
{
	if (!t)
		return;
	t->scan_active = 1;
	t->scan_chan = t->channel_range_lo;
	t->scan_next_tick = 0;
}

static void stop_scan(struct rf_task *t)
{
	if (!t)
		return;
	t->scan_active = 0;
	t->scan_next_tick = 0;
}

static void arm_automation(struct rf_task *t, uint64_t now)
{
	if (!t)
		return;
	t->auto_armed = 1;
	t->auto_started = 0;
	t->auto_err[0] = 0;
	t->auto_start_tick = now + (uint64_t)rf_clamp_int(t->auto_start_delay_ms, 0, 1000000);
	t->auto_stop_tick = 0;
	rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
}

static void disarm_automation(struct rf_task *t)
{
	if (!t)
		return;

	int running = t->auto_started;

	t->auto_armed = 0;
	t->auto_started = 0;
	t->auto_start_tick = 0;
	t->auto_stop_tick = 0;
	t->auto_run_start_tick = 0;
	t->auto_run_start_sweeps = 0;
	t->auto_run_start_pkt_seq = 0;
	t->auto_err[0] = 0;

	if (running) {
		if (t->recording)
			(void)rf_recording_stop(t, NULL, 0);
		if (t->scan_active)
			stop_scan(t);
	}
	rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
}

void rf_automation_toggle_arm(struct rf_task *t)
{
	if (!t)
		return;
	if (t->auto_armed) {
		disarm_automation(t);
		return;
	}
	arm_automation(t, t->now_tick);
}

static void adjust_automation(struct rf_task *t, int delta)
{
	if (!t || delta == 0)
		return;

	int changed = 0;
	switch (t->auto_sel) {
	case 0: /* ARM */
		rf_automation_toggle_arm(t);
		changed = 1;
		break;
	case 1: /* START+ms */
		t->auto_start_delay_ms = rf_clamp_int(t->auto_start_delay_ms + delta * 1000, 0, 1000000);
		changed = 1;
		break;
	case 2: /* DURms */
		t->auto_duration_ms = rf_clamp_int(t->auto_duration_ms + delta * 10000, 0, 1000000);
		changed = 1;
		break;
	case 3: /* STOP_SW */
		t->auto_stop_sweeps = rf_clamp_int(t->auto_stop_sweeps + delta * 10, 0, 1000000);
		changed = 1;
		break;
	case 4: /* STOP_PK */
		t->auto_stop_packets = rf_clamp_int(t->auto_stop_packets + delta * 10, 0, 1000000);
		changed = 1;
		break;
	case 5: /* REC */
		t->auto_record = !t->auto_record;
		changed = 1;
		break;
	default:
		break;
	}

	if (!changed)
		return;

	if (t->auto_armed && !t->auto_started)
		t->auto_start_tick = t->now_tick + (uint64_t)t->auto_start_delay_ms;
	if (t->auto_armed && t->auto_started && t->auto_duration_ms > 0)
		t->auto_stop_tick = t->auto_run_start_tick + (uint64_t)t->auto_duration_ms;

	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
}

static void activate_automation_line(struct rf_task *t)
{
	if (!t)
		return;

	switch (t->auto_sel) {
	case 0:
		rf_automation_toggle_arm(t);
		return;
	case 1: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->auto_start_delay_ms, 0, 1000000));
		rf_prompt_open(t, RF_PROMPT_AUTO_START_DELAY, "Automation start delay (ms)", initial);
		return;
	}
	case 2: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->auto_duration_ms, 0, 1000000));
		rf_prompt_open(t, RF_PROMPT_AUTO_DURATION, "Automation duration (ms, 0=manual)", initial);
		return;
	}
	case 3: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->auto_stop_sweeps, 0, 1000000));
		rf_prompt_open(t, RF_PROMPT_AUTO_STOP_SWEEPS, "Auto-stop after sweeps (0=off)", initial);
		return;
	}
	case 4: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", rf_clamp_int(t->auto_stop_packets, 0, 1000000));
		rf_prompt_open(t, RF_PROMPT_AUTO_STOP_PACKETS, "Auto-stop after packets (0=off)", initial);
		return;
	}
	case 5:
		t->auto_record = !t->auto_record;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
		return;
	case 6: {
		char initial[32];
		snprintf(initial, sizeof(initial), "%s", t->auto_session_base[0] ? t->auto_session_base : "auto");
		rf_prompt_open(t, RF_PROMPT_AUTO_NAME, "Automation session base name", initial);
		return;
	}
	default:
		return;
	}
}

void rf_automation_open(struct rf_task *t)
{
	if (!t)
		return;
	t->show_automation = 1;
	t->auto_sel = 0;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
}

void rf_automation_close(struct rf_task *t)
{
	if (!t || !t->show_automation)
		return;
	t->show_automation = 0;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
}

void rf_automation_handle_key(struct rf_task *t, const struct rf_key *k)
{
	if (!t || !k)
		return;

	switch (k->kind) {
	case RF_KEY_ESC:
		rf_automation_close(t);
		return;
	case RF_KEY_UP:
		t->auto_sel--;
		if (t->auto_sel < 0)
			t->auto_sel = automation_lines - 1;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		return;
	case RF_KEY_DOWN:
		t->auto_sel++;
		if (t->auto_sel >= automation_lines)
			t->auto_sel = 0;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		return;
	case RF_KEY_LEFT:
		adjust_automation(t, -1);
		return;
	case RF_KEY_RIGHT:
		adjust_automation(t, +1);
		return;
	case RF_KEY_ENTER:
		activate_automation_line(t);
		return;
	default:
		return;
	}
}

void rf_automation_tick(struct rf_task *t, uint64_t tick)
{
	if (!t || t->replay_active || !t->auto_armed)
		return;

	if (!t->auto_started) {
		if (tick < t->auto_start_tick)
			return;

		t->auto_started = 1;
		t->auto_run_start_tick = tick;
		t->auto_run_start_sweeps = t->sweep_count;
		t->auto_run_start_pkt_seq = t->pkt_seq;
		t->auto_err[0] = 0;

		if (!t->scan_active)
			start_scan(t);

		if (t->auto_record && !t->recording) {
			char base[32];
			rf_sanitize_name(t->auto_session_base[0] ? t->auto_session_base : "auto", base, sizeof(base));
			if (!base[0])
				snprintf(base, sizeof(base), "auto");

			char name[40];
			snprintf(name, sizeof(name), "%s_%lu", base, (unsigned long)(tick / 1000u));
			char rerr[64];
			if (rf_recording_start(t, name, rerr, sizeof(rerr)) != 0) {
				snprintf(t->auto_err, sizeof(t->auto_err), "%s", rerr[0] ? rerr : "record start failed");
				t->auto_armed = 0;
				t->auto_started = 0;
				rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
				return;
			}
		}

		if (t->auto_duration_ms > 0)
			t->auto_stop_tick = tick + (uint64_t)t->auto_duration_ms;
		else
			t->auto_stop_tick = 0;

		rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
		return;
	}

	int should_stop = 0;
	if (!should_stop && t->auto_duration_ms > 0 && t->auto_stop_tick != 0 && tick >= t->auto_stop_tick)
		should_stop = 1;
	if (!should_stop && t->auto_stop_sweeps > 0 && t->sweep_count >= t->auto_run_start_sweeps) {
		if ((t->sweep_count - t->auto_run_start_sweeps) >= (uint64_t)t->auto_stop_sweeps)
			should_stop = 1;
	}
	if (!should_stop && t->auto_stop_packets > 0 && t->pkt_seq >= t->auto_run_start_pkt_seq) {
		if ((uint32_t)(t->pkt_seq - t->auto_run_start_pkt_seq) >= (uint32_t)t->auto_stop_packets)
			should_stop = 1;
	}
	if (!should_stop && t->record_err[0])
		should_stop = 1;

	if (!should_stop)
		return;
	disarm_automation(t);
}

void rf_automation_status_line(const struct rf_task *t, uint64_t now, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t)
		return;

	if (!t->auto_armed) {
		snprintf(out, outsz, "AUTO: off");
		return;
	}
	if (t->auto_err[0]) {
		snprintf(out, outsz, "AUTO: ERR %s", t->auto_err);
		return;
	}
	if (t->auto_started) {
		uint64_t age = 0;
		if (t->auto_run_start_tick != 0 && now > t->auto_run_start_tick)
			age = now - t->auto_run_start_tick;
		char stop[16];
		snprintf(stop, sizeof(stop), "manual");
		if (t->auto_stop_tick != 0 && now < t->auto_stop_tick)
			snprintf(stop, sizeof(stop), "in %ds", (int)((t->auto_stop_tick - now) / 1000u));
		else if (t->auto_stop_tick != 0 && now >= t->auto_stop_tick)
			snprintf(stop, sizeof(stop), "now");
		snprintf(out, outsz, "AUTO: RUN  age:%ds stop:%s", (int)(age / 1000u), stop);
		return;
	}
	if (t->auto_start_tick != 0 && now < t->auto_start_tick) {
		snprintf(out, outsz, "AUTO: ARMED  start in %ds", (int)((t->auto_start_tick - now) / 1000u));
		return;
	}
	snprintf(out, outsz, "AUTO: ARMED  start now");
}
