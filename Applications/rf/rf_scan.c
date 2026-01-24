#include "rf_scan.h"

#include "rf_task.h"
#include "rf_sniffer.h"
#include "rf_waterfall.h"

static int toggle_amp(uint64_t tick, uint64_t period_ticks, int on_amp, int off_amp)
{
	if (period_ticks == 0)
		return on_amp;
	if ((tick % period_ticks) < (period_ticks * 2) / 3)
		return on_amp;
	return off_amp;
}

static int bump(int ch, int center, int width, int amp)
{
	if (width <= 0 || amp <= 0)
		return 0;
	int d = ch - center;
	if (d < 0)
		d = -d;
	if (d > width)
		return 0;
	return amp * (width - d) / width;
}

static uint8_t sample_energy(struct rf_task *t, int ch, uint64_t tick)
{
	if (!t)
		return 0;
	if (t->rng == 0)
		t->rng = 0xA341316Cu;

	/* xorshift32 */
	uint32_t x = t->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	t->rng = x;

	int noise = (int)(x & 0x0F) + 4;
	int v = noise;

	v += bump(ch, 12, 12, toggle_amp(tick, 650, 220, 70));
	v += bump(ch, 37, 12, toggle_amp(tick, 480, 200, 60));
	v += bump(ch, 62, 12, toggle_amp(tick, 720, 210, 80));

	int bt_center = 2 + (int)((tick / 11) % 80);
	v += bump(ch, bt_center, 3, 170);

	v += bump(ch, 76, 2, toggle_amp(tick, 900, 240, 0));
	v += bump(ch, 91, 2, toggle_amp(tick, 1100, 180, 0));

	if (v < 0)
		v = 0;
	if (v > 255)
		v = 255;
	return (uint8_t)v;
}

static void update_channel(struct rf_task *t, int ch, uint8_t v)
{
	if (!t || ch < 0 || ch >= RF_NUM_CHANNELS)
		return;

	t->energy_cur[ch] = v;

	int avg = (int)t->energy_avg[ch];
	int cur = (int)v;
	avg += (cur - avg) / 4;
	if (avg < 0)
		avg = 0;
	if (avg > 255)
		avg = 255;
	t->energy_avg[ch] = (uint8_t)avg;

	if (v > t->energy_peak[ch])
		t->energy_peak[ch] = v;
}

static void on_sweep_complete(struct rf_task *t, uint64_t now)
{
	if (!t)
		return;

	t->sweep_count++;
	t->last_sweep_tick = now;

	for (int i = 0; i < RF_NUM_CHANNELS; i++) {
		if (t->energy_peak[i] > 0)
			t->energy_peak[i]--;
		if (t->energy_avg[i] > 0 && (t->scan_speed_scalar > 1))
			t->energy_avg[i]--;
	}

	if (t->waterfall_frozen)
		return;
	if (!rf_waterfall_ensure_alloc(t))
		return;
	rf_waterfall_push_row(t);
	rf_task_invalidate(t, RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
}

void rf_scan_tick(struct rf_task *t, uint64_t tick)
{
	if (!t || !t->scan_active)
		return;
	if (t->replay_active)
		return;

	uint64_t now = tick;

	if (t->scan_next_tick == 0 || t->scan_chan < t->channel_range_lo || t->scan_chan > t->channel_range_hi) {
		t->scan_chan = t->channel_range_lo;
		t->scan_next_tick = now;
	}

	uint64_t dwell = (uint64_t)t->dwell_time_ms;
	if (dwell == 0)
		dwell = 1;
	int step = rf_clamp_int(t->scan_speed_scalar, 1, 10);

	const int max_ops_per_tick = 8;
	for (int ops = 0; ops < max_ops_per_tick && now >= t->scan_next_tick; ops++) {
		int ch = t->scan_chan;
		if (ch < t->channel_range_lo || ch > t->channel_range_hi)
			ch = t->channel_range_lo;
		if (ch < 0)
			ch = 0;
		if (ch > RF_MAX_CHANNEL)
			ch = RF_MAX_CHANNEL;

		uint8_t v = sample_energy(t, ch, now);
		update_channel(t, ch, v);
		rf_sniffer_maybe_capture_packet(t, ch, v, now);
		rf_task_invalidate(t, RF_DIRTY_SPECTRUM);

		t->scan_chan += step;
		if (t->scan_chan > t->channel_range_hi) {
			t->scan_chan = t->channel_range_lo;
			on_sweep_complete(t, now);
		}
		t->scan_next_tick += dwell;
	}
}
