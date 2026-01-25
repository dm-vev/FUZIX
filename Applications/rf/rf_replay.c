#include "rf_replay.h"

#include "rf_analytics.h"
#include "rf.h"
#include "rf_recording.h"
#include "rf_session.h"
#include "rf_sniffer.h"
#include "rf_task.h"
#include "rf_view.h"
#include "rf_waterfall.h"

#include <stdio.h>
#include <string.h>

static int clamp_speed(int v)
{
	return rf_clamp_int(v, 1, 32);
}

static int upper_bound_sweep(const struct rf_session_sweep_index *sweeps, size_t n, uint64_t tick)
{
	size_t lo = 0;
	size_t hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (sweeps[mid].tick <= tick)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (int)lo - 1;
}

static int upper_bound_packet(const struct rf_session_packet_meta *pkts, size_t n, uint64_t tick)
{
	size_t lo = 0;
	size_t hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (pkts[mid].tick <= tick)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (int)lo;
}

static int upper_bound_config(const struct rf_session_config_event *cfgs, size_t n, uint64_t tick)
{
	size_t lo = 0;
	size_t hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (cfgs[mid].tick <= tick)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (int)lo - 1;
}

static void apply_cfg_snapshot(struct rf_task *t, struct rf_session_cfg_snapshot cfg)
{
	if (!t)
		return;

	t->channel_range_lo = rf_clamp_int(cfg.channel_range_lo, 0, RF_MAX_CHANNEL);
	t->channel_range_hi = rf_clamp_int(cfg.channel_range_hi, 0, RF_MAX_CHANNEL);
	if (t->channel_range_lo > t->channel_range_hi) {
		int tmp = t->channel_range_lo;
		t->channel_range_lo = t->channel_range_hi;
		t->channel_range_hi = tmp;
	}
	t->dwell_time_ms = rf_clamp_int(cfg.dwell_time_ms, 1, 50);
	t->scan_speed_scalar = rf_clamp_int(cfg.scan_step, 1, 10);
	t->data_rate = cfg.data_rate;
	t->crc_mode = cfg.crc_mode;
	t->auto_ack = cfg.auto_ack ? 1 : 0;
	t->power_level = cfg.power_level;
	t->wf_palette = cfg.wf_palette;
	rf_waterfall_rebuild_palette(t);
	t->scan_next_tick = 0;
}

static void apply_replay_config_at(struct rf_task *t, uint64_t session_tick)
{
	if (!t || !t->replay_active || !t->replay || t->replay->config_count == 0)
		return;

	int i = upper_bound_config(t->replay->configs, t->replay->config_count, session_tick);
	if (i < 0)
		i = 0;
	if ((size_t)i >= t->replay->config_count)
		i = (int)t->replay->config_count - 1;
	if (i == t->replay_cfg_idx)
		return;

	t->replay_cfg_idx = i;
	struct rf_session_config_event ev = t->replay->configs[i];

	apply_cfg_snapshot(t, ev.cfg);
	t->selected_channel = rf_clamp_int(ev.selected_channel, 0, RF_MAX_CHANNEL);
	t->preset_dirty = 0;
}

static void apply_replay_sweep(struct rf_task *t, int idx)
{
	if (!t || !t->replay || idx < 0 || (size_t)idx >= t->replay->sweep_count)
		return;

	uint32_t off = t->replay->sweeps[idx].off;
	uint64_t tick = 0;
	uint8_t energy[RF_NUM_CHANNELS];
	char err[96];
	if (rf_session_read_sweep(t->replay, off, &tick, energy, err, sizeof(err)) != 0) {
		snprintf(t->replay_err, sizeof(t->replay_err), "%s", err[0] ? err : "bad sweep");
		return;
	}

	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		uint8_t v = energy[ch];
		t->energy_cur[ch] = v;
		t->energy_avg[ch] = v;
		if (t->energy_peak[ch] > 0)
			t->energy_peak[ch]--;
		if (v > t->energy_peak[ch])
			t->energy_peak[ch] = v;
	}

	t->sweep_count = (uint64_t)(idx + 1);
	t->last_sweep_tick = tick;
}

static void advance_replay_sweep_to(struct rf_task *t, int target)
{
	if (!t || !t->replay || target < 0 || (size_t)target >= t->replay->sweep_count)
		return;

	if (t->replay_sweep_idx < 0) {
		apply_replay_sweep(t, target);
		rf_analytics_on_sweep(t, t->replay->sweeps[target].tick);
		t->replay_sweep_idx = target;
		return;
	}

	if (target <= t->replay_sweep_idx) {
		apply_replay_sweep(t, target);
		t->replay_sweep_idx = target;
		return;
	}

	for (int i = t->replay_sweep_idx + 1; i <= target; i++) {
		apply_replay_sweep(t, i);
		rf_analytics_on_sweep(t, t->replay->sweeps[i].tick);
		t->replay_sweep_idx = i;
		if (!t->waterfall_frozen)
			rf_waterfall_push_row(t);
	}
}

static void rebuild_replay_waterfall_at(struct rf_task *t, int sweep_idx)
{
	if (!t || !t->replay || t->replay->sweep_count == 0)
		return;
	if (!rf_waterfall_ensure_alloc(t))
		return;

	if (t->wf_buf && t->wf_cap)
		memset(t->wf_buf, 0, t->wf_cap);
	t->wf_head = 0;

	if (sweep_idx < 0)
		sweep_idx = 0;
	if ((size_t)sweep_idx >= t->replay->sweep_count)
		sweep_idx = (int)t->replay->sweep_count - 1;

	int start = sweep_idx - (t->wf_h - 1);
	if (start < 0)
		start = 0;

	for (int i = start; i <= sweep_idx && (size_t)i < t->replay->sweep_count; i++) {
		apply_replay_sweep(t, i);
		rf_analytics_on_sweep(t, t->replay->sweeps[i].tick);
		rf_waterfall_push_row(t);
	}
	rf_task_invalidate(t, RF_DIRTY_WATERFALL);
}

static void update_replay_position(struct rf_task *t, uint64_t session_tick, int force)
{
	if (!t || !t->replay_active || !t->replay)
		return;

	int old_sweep_idx = t->replay_sweep_idx;
	int old_pkt_limit = t->replay_pkt_limit;

	int new_sweep_idx = -1;
	if (t->replay->sweep_count > 0) {
		new_sweep_idx = upper_bound_sweep(t->replay->sweeps, t->replay->sweep_count, session_tick);
		if (new_sweep_idx < 0)
			new_sweep_idx = 0;
		if ((size_t)new_sweep_idx >= t->replay->sweep_count)
			new_sweep_idx = (int)t->replay->sweep_count - 1;
	}

	int new_pkt_limit = upper_bound_packet(t->replay->packets, t->replay->packet_count, session_tick);
	if (new_pkt_limit < 0)
		new_pkt_limit = 0;
	if ((size_t)new_pkt_limit > t->replay->packet_count)
		new_pkt_limit = (int)t->replay->packet_count;

	int jump = force || old_sweep_idx < 0 || (new_sweep_idx >= 0 && new_sweep_idx < old_sweep_idx) ||
		   (old_sweep_idx >= 0 && new_sweep_idx - old_sweep_idx > 8) || new_pkt_limit < old_pkt_limit;

	if (jump) {
		rf_analytics_reset(t);
		if (new_sweep_idx >= 0) {
			rebuild_replay_waterfall_at(t, new_sweep_idx);
			t->replay_sweep_idx = new_sweep_idx;
		}

		const int pkt_window = 512;
		int start = new_pkt_limit - pkt_window;
		if (start < 0)
			start = 0;
		for (int i = start; i < new_pkt_limit; i++) {
			if ((size_t)i >= t->replay->packet_count)
				break;
			rf_analytics_on_packet_meta(t, &t->replay->packets[i]);
		}
		t->pkt_sec_start = 0;
		t->pkt_sec_count = 0;
		t->pkts_per_sec = 0;
	} else if (new_sweep_idx >= 0 && new_sweep_idx != old_sweep_idx) {
		advance_replay_sweep_to(t, new_sweep_idx);
		rf_task_invalidate(t, RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
	}

	if (new_pkt_limit != old_pkt_limit) {
		if (!jump && new_pkt_limit > old_pkt_limit) {
			for (int i = old_pkt_limit; i < new_pkt_limit; i++) {
				if ((size_t)i >= t->replay->packet_count)
					break;
				rf_analytics_on_packet_meta(t, &t->replay->packets[i]);
			}
			t->pkt_sec_count += new_pkt_limit - old_pkt_limit;
		}
		t->replay_pkt_limit = new_pkt_limit;
		t->replay_pkt_cache_ok = 0;
		rf_sniffer_reconcile_selection(t);
		rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
	}

	if (jump) {
		rf_task_invalidate(t, RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL |
					      RF_DIRTY_STATUS | RF_DIRTY_ANALYSIS);
	}
}

int rf_replay_enter(struct rf_task *t, const char *input, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}

	if (t->recording)
		(void)rf_recording_stop(t, NULL, 0);

	char lerr[128];
	struct rf_session *sess = rf_session_load(input, lerr, sizeof(lerr));
	if (!sess) {
		if (err && errsz)
			snprintf(err, errsz, "%s", lerr[0] ? lerr : "load session failed");
		return -1;
	}

	if (t->replay) {
		rf_session_free(t->replay);
		t->replay = NULL;
	}

	t->replay = sess;
	t->replay_active = 1;
	t->replay_playing = 0;
	if (t->replay_speed <= 0)
		t->replay_speed = 1;
	t->replay_host_last_tick = 0;
	t->replay_now_tick = sess->start_tick;
	t->replay_sweep_idx = -1;
	t->replay_pkt_limit = 0;
	t->replay_cfg_idx = -1;
	t->replay_err[0] = 0;
	t->replay_pkt_cache_ok = 0;
	t->replay_pkt_cache_seq = 0;

	t->scan_active = 0;
	t->scan_next_tick = 0;
	t->capture_paused = 0;

	rf_view_reset(t);
	t->pkt_head = 0;
	t->pkt_count = 0;
	t->pkt_seq = 0;
	t->pkt_dropped = 0;
	t->pkt_sec_start = 0;
	t->pkt_sec_count = 0;
	t->pkts_per_sec = 0;
	t->sniffer_sel = 0;
	t->sniffer_top = 0;
	t->sniffer_sel_seq = 0;

	apply_replay_config_at(t, t->replay_now_tick);
	update_replay_position(t, t->replay_now_tick, 1);
	rf_task_invalidate(t, RF_DIRTY_ALL);
	return 0;
}

void rf_replay_exit(struct rf_task *t)
{
	if (!t)
		return;
	if (!t->replay_active && !t->replay)
		return;

	t->replay_active = 0;
	t->replay_playing = 0;
	t->replay_host_last_tick = 0;
	t->replay_now_tick = 0;
	t->replay_sweep_idx = -1;
	t->replay_pkt_limit = 0;
	t->replay_cfg_idx = -1;
	t->replay_err[0] = 0;
	t->replay_pkt_cache_ok = 0;
	t->replay_pkt_cache_seq = 0;

	if (t->replay) {
		rf_session_free(t->replay);
		t->replay = NULL;
	}

	rf_analytics_reset(t);
	rf_task_invalidate(t, RF_DIRTY_ALL);
}

void rf_replay_tick(struct rf_task *t, uint64_t host_tick)
{
	if (!t || !t->replay_active || !t->replay)
		return;

	if (t->replay_host_last_tick == 0)
		t->replay_host_last_tick = host_tick;
	uint64_t delta = host_tick - t->replay_host_last_tick;
	t->replay_host_last_tick = host_tick;

	if (t->replay_playing) {
		uint64_t advance = delta * (uint64_t)clamp_speed(t->replay_speed);
		t->replay_now_tick += advance;
		if (t->replay_now_tick > t->replay->end_tick) {
			t->replay_now_tick = t->replay->end_tick;
			t->replay_playing = 0;
		}
	}

	apply_replay_config_at(t, t->replay_now_tick);
	update_replay_position(t, t->replay_now_tick, 0);
}

void rf_replay_seek_ms(struct rf_task *t, uint64_t offset_ms)
{
	if (!t || !t->replay_active || !t->replay)
		return;

	uint64_t new_tick = t->replay->start_tick + offset_ms;
	if (new_tick < t->replay->start_tick)
		new_tick = t->replay->start_tick;
	if (new_tick > t->replay->end_tick)
		new_tick = t->replay->end_tick;

	t->replay_now_tick = new_tick;
	t->replay_host_last_tick = 0;
	t->replay_playing = 0;
	update_replay_position(t, t->replay_now_tick, 1);
}

void rf_replay_reset_view(struct rf_task *t)
{
	if (!t)
		return;

	rf_view_reset(t);
	if (!t->replay_active || !t->replay)
		return;

	t->replay_cfg_idx = -1;
	t->replay_sweep_idx = -1;
	t->replay_pkt_limit = 0;
	t->sniffer_sel = 0;
	t->sniffer_top = 0;
	t->sniffer_sel_seq = 0;
	t->replay_pkt_cache_ok = 0;
	t->replay_pkt_cache_seq = 0;
	t->replay_host_last_tick = 0;

	apply_replay_config_at(t, t->replay_now_tick);
	update_replay_position(t, t->replay_now_tick, 1);
}

void rf_replay_update_packet_cache(struct rf_task *t)
{
	if (!t || !t->replay_active || !t->replay)
		return;

	struct rf_session_packet_meta meta;
	memset(&meta, 0, sizeof(meta));
	if (!rf_sniffer_filtered_replay_packet_meta_by_index(t, t->sniffer_sel, &meta)) {
		t->replay_pkt_cache_ok = 0;
		return;
	}
	if (t->replay_pkt_cache_ok && t->replay_pkt_cache_seq == meta.seq)
		return;

	struct rf_packet p;
	char err[96];
	if (rf_session_read_packet(t->replay, meta.off, &p, err, sizeof(err)) != 0) {
		snprintf(t->replay_err, sizeof(t->replay_err), "%s", err[0] ? err : "bad packet");
		t->replay_pkt_cache_ok = 0;
		return;
	}
	p.delta_ms = meta.delta_ms;
	p.flags = meta.flags;
	t->replay_pkt_cache = p;
	t->replay_pkt_cache_seq = p.seq;
	t->replay_pkt_cache_ok = 1;
	rf_task_invalidate(t, RF_DIRTY_PROTOCOL);
}

void rf_replay_time_text(const struct rf_task *t, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t || !t->replay)
		return;
	if (t->replay_now_tick < t->replay->start_tick) {
		snprintf(out, outsz, "t:-");
		return;
	}
	snprintf(out, outsz, "t:%ds", (int)((t->replay_now_tick - t->replay->start_tick) / 1000u));
}
