#include "rf_analytics.h"

#include "rf.h"
#include "rf_session.h"
#include "rf_sniffer.h"
#include "rf_task.h"

#include <stdio.h>
#include <string.h>

enum {
	RF_BEST_INTERVAL_TICKS = RF_ANA_BEST_INTERVAL_TICKS,
	RF_PERIODIC_MIN_INTERVALS = RF_ANA_PERIODIC_MIN_INTERVALS,
};

static uint32_t abs_diff_u32(uint32_t a, uint32_t b)
{
	if (a >= b)
		return a - b;
	return b - a;
}

static void set_occ_bit(uint8_t bits[RF_OCC_BYTES], int ch)
{
	if (!bits || ch < 0 || ch >= RF_NUM_CHANNELS)
		return;
	int byte_idx = ch / 8;
	uint8_t mask = (uint8_t)(1u << (unsigned)(ch % 8));
	bits[byte_idx] |= mask;
}

static int occ_bit(const uint8_t bits[RF_OCC_BYTES], int ch)
{
	if (!bits || ch < 0 || ch >= RF_NUM_CHANNELS)
		return 0;
	int byte_idx = ch / 8;
	uint8_t mask = (uint8_t)(1u << (unsigned)(ch % 8));
	return (bits[byte_idx] & mask) != 0;
}

static void push_occ_hist(struct rf_task *t, const uint8_t bits[RF_OCC_BYTES])
{
	if (!t || !bits)
		return;
	memcpy(t->occ_hist[t->occ_hist_head], bits, RF_OCC_BYTES);
	t->occ_hist_head++;
	if (t->occ_hist_head >= RF_OCC_HIST_LEN)
		t->occ_hist_head = 0;
	if (t->occ_hist_count < RF_OCC_HIST_LEN)
		t->occ_hist_count++;
}

static int count_devices_used(const struct rf_task *t)
{
	if (!t)
		return 0;
	int n = 0;
	for (int i = 0; i < RF_MAX_DEVICES; i++) {
		if (t->devices[i].used)
			n++;
	}
	return n;
}

struct rf_device_stat *rf_analytics_find_device(struct rf_task *t, uint8_t addr_len, const uint8_t addr[5])
{
	if (!t || !addr || addr_len == 0)
		return NULL;
	if (addr_len > 5)
		addr_len = 5;
	for (int i = 0; i < RF_MAX_DEVICES; i++) {
		struct rf_device_stat *d = &t->devices[i];
		if (!d->used || d->addr_len != addr_len)
			continue;
		int match = 1;
		for (int j = 0; j < (int)addr_len; j++) {
			if (d->addr[j] != addr[j]) {
				match = 0;
				break;
			}
		}
		if (match)
			return d;
	}
	return NULL;
}

const struct rf_device_stat *rf_analytics_find_device_const(const struct rf_task *t, uint8_t addr_len, const uint8_t addr[5])
{
	return rf_analytics_find_device((struct rf_task *)t, addr_len, addr);
}

static struct rf_device_stat *find_or_alloc_device(struct rf_task *t, uint8_t addr_len, const uint8_t addr[5], uint64_t tick)
{
	if (!t || !addr || addr_len == 0)
		return NULL;
	if (addr_len > 5)
		addr_len = 5;

	struct rf_device_stat *d = rf_analytics_find_device(t, addr_len, addr);
	if (d)
		return d;

	int evict = -1;
	uint64_t oldest = 0;
	for (int i = 0; i < RF_MAX_DEVICES; i++) {
		if (!t->devices[i].used) {
			evict = i;
			break;
		}
		if (evict == -1 || t->devices[i].last_tick < oldest) {
			oldest = t->devices[i].last_tick;
			evict = i;
		}
	}
	if (evict < 0)
		return NULL;

	d = &t->devices[evict];
	memset(d, 0, sizeof(*d));
	d->used = 1;
	d->addr_len = addr_len;
	memcpy(d->addr, addr, 5);
	d->first_tick = tick;
	d->last_tick = tick;
	t->device_count = count_devices_used(t);
	return d;
}

void rf_analytics_reset(struct rf_task *t)
{
	if (!t)
		return;

	t->ana_sweep_count = 0;
	for (int i = 0; i < RF_NUM_CHANNELS; i++) {
		t->ana_occ_count[i] = 0;
		t->ana_energy_sum[i] = 0;
		t->ana_chan_pkt[i] = 0;
		t->ana_chan_bad[i] = 0;
		t->ana_chan_retry[i] = 0;
		t->ana_high[i] = 0;
		t->ana_last_rise[i] = 0;
		t->ana_rise_count[i] = 0;
		t->ana_rise_avg_ms[i] = 0;
		t->ana_rise_min_ms[i] = 0;
		t->ana_rise_max_ms[i] = 0;
	}

	memset(t->best_hist, 0, sizeof(t->best_hist));
	t->best_head = 0;
	t->best_count = 0;
	t->best_next_tick = 0;

	memset(t->devices, 0, sizeof(t->devices));
	t->device_count = 0;

	memset(t->occ_hist, 0, sizeof(t->occ_hist));
	t->occ_hist_head = 0;
	t->occ_hist_count = 0;

	t->analysis_sel = 0;
	t->analysis_top = 0;
	rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
}

int rf_analytics_channel_score(const struct rf_task *t, int ch)
{
	if (!t || ch < 0 || ch >= RF_NUM_CHANNELS)
		return 0;
	if (t->ana_sweep_count == 0)
		return 0;

	int occ_pct = (int)(t->ana_occ_count[ch] * 100u / t->ana_sweep_count);
	int avg_energy = (int)(t->ana_energy_sum[ch] / t->ana_sweep_count);

	uint32_t pkt = t->ana_chan_pkt[ch];
	int bad_pct = 0;
	int retry_pct = 0;
	if (pkt > 0) {
		bad_pct = (int)(t->ana_chan_bad[ch] * 100u / pkt);
		retry_pct = (int)(t->ana_chan_retry[ch] * 100u / pkt);
	}

	int score = 100;
	score -= occ_pct * 50 / 100;
	score -= avg_energy * 30 / 255;
	score -= bad_pct * 15 / 100;
	score -= retry_pct * 5 / 100;
	if (score < 0)
		score = 0;
	if (score > 100)
		score = 100;
	return score;
}

int rf_analytics_best_channel_now(const struct rf_task *t, int *ch_out, int *score_out)
{
	if (ch_out)
		*ch_out = 0;
	if (score_out)
		*score_out = 0;
	if (!t)
		return 0;

	int best_ch = 0;
	int best_score = -1;
	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		int s = rf_analytics_channel_score(t, ch);
		if (s > best_score) {
			best_score = s;
			best_ch = ch;
		}
	}
	if (best_score < 0)
		best_score = 0;
	if (ch_out)
		*ch_out = best_ch;
	if (score_out)
		*score_out = best_score;
	return 1;
}

int rf_analytics_top_channels(const struct rf_task *t, int n, struct rf_chan_score *out, int out_cap)
{
	if (!out || out_cap <= 0 || n < 1)
		return 0;
	if (!t)
		return 0;
	if (n > out_cap)
		n = out_cap;
	if (n > RF_NUM_CHANNELS)
		n = RF_NUM_CHANNELS;

	int count = 0;
	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		int score = rf_analytics_channel_score(t, ch);
		if (count < n) {
			out[count++] = (struct rf_chan_score){.ch = ch, .score = score};
			for (int j = count - 1; j > 0 && out[j].score > out[j - 1].score; j--) {
				struct rf_chan_score tmp = out[j];
				out[j] = out[j - 1];
				out[j - 1] = tmp;
			}
			continue;
		}
		if (score <= out[count - 1].score)
			continue;
		out[count - 1] = (struct rf_chan_score){.ch = ch, .score = score};
		for (int j = count - 1; j > 0 && out[j].score > out[j - 1].score; j--) {
			struct rf_chan_score tmp = out[j];
			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	}
	return count;
}

void rf_analytics_periodic_text(const struct rf_task *t, int ch, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t || ch < 0 || ch >= RF_NUM_CHANNELS)
		return;

	uint8_t cnt = t->ana_rise_count[ch];
	if ((int)cnt < RF_PERIODIC_MIN_INTERVALS)
		return;
	uint32_t avg = t->ana_rise_avg_ms[ch];
	uint32_t minv = t->ana_rise_min_ms[ch];
	uint32_t maxv = t->ana_rise_max_ms[ch];
	if (avg == 0)
		return;
	uint32_t jitter = maxv - minv;
	if (jitter > avg / 4)
		return;
	snprintf(out, outsz, "P~%lums", (unsigned long)avg);
}

void rf_analytics_best_history_line(const struct rf_task *t, int n, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t) {
		snprintf(out, outsz, "best: (none)");
		return;
	}
	if (t->best_count == 0) {
		snprintf(out, outsz, "best: (none)");
		return;
	}
	if (n < 1)
		n = 1;
	if (n > t->best_count)
		n = t->best_count;

	size_t w = 0;
	w += (size_t)snprintf(out + w, outsz - w, "best:");

	int i = t->best_head - 1;
	for (int k = 0; k < n; k++) {
		if (w + 1 >= outsz)
			break;
		if (i < 0)
			i = (int)(sizeof(t->best_hist) / sizeof(t->best_hist[0])) - 1;
		struct rf_best_chan_entry ev = t->best_hist[i];
		if (ev.score == 0) {
			i--;
			continue;
		}
		w += (size_t)snprintf(out + w, outsz - w, " %03u(%u)", (unsigned)ev.ch, (unsigned)ev.score);
		i--;
	}
}

void rf_analytics_on_sweep(struct rf_task *t, uint64_t tick)
{
	if (!t)
		return;

	uint8_t bits[RF_OCC_BYTES];
	memset(bits, 0, sizeof(bits));

	t->ana_sweep_count++;
	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		uint8_t v = t->energy_avg[ch];
		t->ana_energy_sum[ch] += (uint32_t)v;
		if (v >= RF_ANA_OCC_THRESHOLD)
			t->ana_occ_count[ch]++;

		int high = v >= RF_ANA_OCC_THRESHOLD;
		if (high)
			set_occ_bit(bits, ch);
		if (high && !t->ana_high[ch]) {
			uint64_t last = t->ana_last_rise[ch];
			if (last != 0 && tick > last) {
				uint64_t dt = tick - last;
				uint32_t ms = (uint32_t)dt;
				if (ms == 0)
					ms = 1;

				uint8_t cnt = t->ana_rise_count[ch];
				if (cnt < 255)
					cnt++;
				t->ana_rise_count[ch] = cnt;
				if (cnt == 1) {
					t->ana_rise_avg_ms[ch] = ms;
					t->ana_rise_min_ms[ch] = ms;
					t->ana_rise_max_ms[ch] = ms;
				} else {
					uint32_t avg = t->ana_rise_avg_ms[ch];
					avg += (ms - avg) / (uint32_t)cnt;
					t->ana_rise_avg_ms[ch] = avg;
					if (ms < t->ana_rise_min_ms[ch])
						t->ana_rise_min_ms[ch] = ms;
					if (ms > t->ana_rise_max_ms[ch])
						t->ana_rise_max_ms[ch] = ms;
				}
			}
			t->ana_last_rise[ch] = tick;
		}
		t->ana_high[ch] = (uint8_t)high;
	}
	push_occ_hist(t, bits);

	if (t->best_next_tick == 0 || tick >= t->best_next_tick) {
		int best_ch = 0;
		int best_score = 0;
		(void)rf_analytics_best_channel_now(t, &best_ch, &best_score);
		if (best_score > 0) {
			t->best_hist[t->best_head] = (struct rf_best_chan_entry){.tick = tick, .ch = (uint8_t)best_ch, .score = (uint8_t)best_score};
			t->best_head++;
			if (t->best_head >= (int)(sizeof(t->best_hist) / sizeof(t->best_hist[0])))
				t->best_head = 0;
			if (t->best_count < (int)(sizeof(t->best_hist) / sizeof(t->best_hist[0])))
				t->best_count++;
		}
		t->best_next_tick = tick + RF_BEST_INTERVAL_TICKS;
	}

	rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
}

void rf_analytics_prepare_live_packet(struct rf_task *t, struct rf_packet *p)
{
	if (!t || !p)
		return;

	p->delta_ms = 0;
	p->flags = 0;
	if (p->addr_len == 0)
		return;
	const struct rf_device_stat *d = rf_analytics_find_device_const(t, p->addr_len, p->addr);
	if (!d)
		return;

	if (d->last_tick != 0 && p->tick > d->last_tick) {
		uint64_t dt = p->tick - d->last_tick;
		if (dt > 0xFFFFu)
			dt = 0xFFFFu;
		p->delta_ms = (uint16_t)dt;
		if (dt <= 12)
			p->flags |= RF_PKT_FLAG_BURST;
	}
	if (d->last_payload_hash != 0 && d->last_payload_hash == p->payload_hash && p->tick > d->last_payload_tick &&
	    (p->tick - d->last_payload_tick) <= RF_ANA_RETRY_WINDOW_TICKS) {
		p->flags |= RF_PKT_FLAG_RETRY;
	}
}

static void analytics_on_packet_fields(struct rf_task *t, uint64_t tick, uint8_t channel, enum rf_data_rate rate, uint8_t addr_len,
				       const uint8_t addr[5], uint8_t length, uint32_t payload_hash, uint8_t crc_len, uint8_t crc_ok)
{
	if (!t)
		return;
	int ch = (int)channel;
	if (ch < 0 || ch >= RF_NUM_CHANNELS)
		return;

	t->ana_chan_pkt[ch]++;
	if (crc_len > 0 && !crc_ok)
		t->ana_chan_bad[ch]++;

	struct rf_device_stat *d = find_or_alloc_device(t, addr_len, addr, tick);
	if (!d) {
		rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
		return;
	}

	if (d->pkt_count == 0) {
		d->first_tick = tick;
		d->int_min = 0;
		d->int_max = 0;
	}

	if (d->last_tick != 0 && tick > d->last_tick) {
		uint32_t dt = (uint32_t)(tick - d->last_tick);
		if (dt != 0) {
			d->int_count++;
			if (d->int_count == 1) {
				d->int_avg = dt;
				d->int_min = dt;
				d->int_max = dt;
			} else {
				uint32_t avg = d->int_avg;
				avg += (dt - avg) / d->int_count;
				d->int_avg = avg;
				if (dt < d->int_min)
					d->int_min = dt;
				if (dt > d->int_max)
					d->int_max = dt;
				uint32_t j = abs_diff_u32(dt, avg);
				d->int_jitter += (j - d->int_jitter) / d->int_count;
			}

			if (dt <= 12) {
				d->burst_cur++;
				if (d->burst_cur > d->burst_max)
					d->burst_max = d->burst_cur;
			} else if (d->burst_cur > 0) {
				d->burst_count++;
				d->burst_cur = 0;
			}
		}
	}

	int retry = 0;
	if (d->last_payload_hash != 0 && d->last_payload_hash == payload_hash && tick > d->last_payload_tick &&
	    (tick - d->last_payload_tick) <= RF_ANA_RETRY_WINDOW_TICKS) {
		retry = 1;
	}
	d->last_payload_hash = payload_hash;
	d->last_payload_tick = tick;
	if (retry) {
		d->retries++;
		t->ana_chan_retry[ch]++;
	}

	if (d->pkt_count > 0 && d->last_channel != channel)
		d->hop_count++;
	d->last_channel = channel;

	d->hop_seq[d->hop_seq_head] = channel;
	d->hop_seq_head++;
	if (d->hop_seq_head >= (uint8_t)(sizeof(d->hop_seq) / sizeof(d->hop_seq[0])))
		d->hop_seq_head = 0;
	if (d->hop_seq_count < (uint8_t)(sizeof(d->hop_seq) / sizeof(d->hop_seq[0])))
		d->hop_seq_count++;

	d->pkt_count++;
	if (crc_len > 0 && !crc_ok)
		d->crc_bad++;
	else if (crc_len > 0)
		d->crc_ok++;

	d->last_tick = tick;
	(void)rate;
	(void)length;

	rf_task_invalidate(t, RF_DIRTY_ANALYSIS);
}

void rf_analytics_on_packet(struct rf_task *t, const struct rf_packet *p)
{
	if (!t || !p)
		return;
	analytics_on_packet_fields(t, p->tick, p->channel, p->rate, p->addr_len, p->addr, p->length, p->payload_hash, p->crc_len,
				   p->crc_ok);
}

void rf_analytics_on_packet_meta(struct rf_task *t, const struct rf_session_packet_meta *m)
{
	if (!t || !m)
		return;
	analytics_on_packet_fields(t, m->tick, m->channel, m->rate, m->addr_len, m->addr, m->length, m->payload_hash, m->crc_len,
				   m->crc_ok);
}

int rf_analytics_top_device_indices(const struct rf_task *t, int n, int *out, int out_cap)
{
	if (!t || !out || out_cap <= 0 || n < 1)
		return 0;
	if (n > out_cap)
		n = out_cap;

	int count = 0;
	for (int i = 0; i < RF_MAX_DEVICES; i++) {
		const struct rf_device_stat *d = &t->devices[i];
		if (!d->used || d->pkt_count == 0)
			continue;

		if (count < n) {
			out[count++] = i;
			for (int j = count - 1; j > 0 && t->devices[out[j]].pkt_count > t->devices[out[j - 1]].pkt_count; j--) {
				int tmp = out[j];
				out[j] = out[j - 1];
				out[j - 1] = tmp;
			}
			continue;
		}

		if (d->pkt_count <= t->devices[out[count - 1]].pkt_count)
			continue;
		out[count - 1] = i;
		for (int j = count - 1; j > 0 && t->devices[out[j]].pkt_count > t->devices[out[j - 1]].pkt_count; j--) {
			int tmp = out[j];
			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	}
	return count;
}

void rf_analytics_device_hop_seq_text(const struct rf_device_stat *d, int n, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!d || d->hop_seq_count == 0) {
		snprintf(out, outsz, "-");
		return;
	}
	if (n < 1)
		n = 1;
	if (n > (int)d->hop_seq_count)
		n = (int)d->hop_seq_count;

	int start = (int)d->hop_seq_head - n;
	while (start < 0)
		start += (int)(sizeof(d->hop_seq) / sizeof(d->hop_seq[0]));

	size_t w = 0;
	for (int i = 0; i < n; i++) {
		int idx = start + i;
		if (idx >= (int)(sizeof(d->hop_seq) / sizeof(d->hop_seq[0])))
			idx -= (int)(sizeof(d->hop_seq) / sizeof(d->hop_seq[0]));
		if (i > 0)
			w += (size_t)snprintf(out + w, outsz - w, " ");
		w += (size_t)snprintf(out + w, outsz - w, "%03u", (unsigned)d->hop_seq[idx]);
		if (w >= outsz)
			break;
	}
}

int rf_analytics_top_correlated_channels(const struct rf_task *t, int ref_ch, int n, struct rf_corr_entry *out, int out_cap)
{
	if (!t || !out || out_cap <= 0 || n < 1 || t->occ_hist_count == 0)
		return 0;
	if (n > out_cap)
		n = out_cap;

	ref_ch = rf_clamp_int(ref_ch, 0, RF_MAX_CHANNEL);

	int ref_count = 0;
	int ch_count[RF_NUM_CHANNELS];
	int both_count[RF_NUM_CHANNELS];
	memset(ch_count, 0, sizeof(ch_count));
	memset(both_count, 0, sizeof(both_count));

	for (int i = 0; i < t->occ_hist_count; i++) {
		int row_idx = t->occ_hist_head - 1 - i;
		while (row_idx < 0)
			row_idx += RF_OCC_HIST_LEN;
		const uint8_t *row = t->occ_hist[row_idx];
		int ref_high = occ_bit(row, ref_ch);
		if (ref_high)
			ref_count++;
		for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
			int high = occ_bit(row, ch);
			if (high)
				ch_count[ch]++;
			if (ref_high && high)
				both_count[ch]++;
		}
	}
	if (ref_count == 0)
		return 0;

	int count = 0;
	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		if (ch == ref_ch)
			continue;
		int uni = ref_count + ch_count[ch] - both_count[ch];
		if (uni <= 0)
			continue;
		int j = both_count[ch] * 100 / uni;
		if (j <= 0)
			continue;
		struct rf_corr_entry e = {.ch = ch, .jacc_pct = j, .both = both_count[ch]};

		if (count < n) {
			out[count++] = e;
		} else {
			int worst = 0;
			for (int i = 1; i < count; i++) {
				if (out[i].jacc_pct < out[worst].jacc_pct)
					worst = i;
			}
			if (e.jacc_pct <= out[worst].jacc_pct)
				continue;
			out[worst] = e;
		}
	}

	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (out[j].jacc_pct > out[i].jacc_pct) {
				struct rf_corr_entry tmp = out[i];
				out[i] = out[j];
				out[j] = tmp;
			}
		}
	}
	return count;
}

int rf_analytics_top_conflict_channels(const struct rf_task *t, int n, struct rf_conflict_row *out, int out_cap)
{
	if (!t || !out || out_cap <= 0 || n < 1)
		return 0;
	if (n > out_cap)
		n = out_cap;

	int count = 0;
	for (int ch = 0; ch < RF_NUM_CHANNELS; ch++) {
		uint32_t pkt = t->ana_chan_pkt[ch];
		if (pkt < 8)
			continue;
		int bad_pct = (int)(t->ana_chan_bad[ch] * 100u / pkt);
		int retry_pct = (int)(t->ana_chan_retry[ch] * 100u / pkt);
		int score = bad_pct * 2 + retry_pct;
		if (score == 0)
			continue;

		struct rf_conflict_row r = {.ch = ch, .bad_pct = bad_pct, .retry_pct = retry_pct, .pkt = pkt, .score = score};
		if (count < n) {
			out[count++] = r;
		} else {
			int worst = 0;
			int worst_score = out[0].score;
			for (int i = 1; i < count; i++) {
				if (out[i].score < worst_score) {
					worst_score = out[i].score;
					worst = i;
				}
			}
			if (score <= worst_score)
				continue;
			out[worst] = r;
		}
	}

	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (out[j].score > out[i].score) {
				struct rf_conflict_row tmp = out[i];
				out[i] = out[j];
				out[j] = tmp;
			}
		}
	}
	return count;
}

