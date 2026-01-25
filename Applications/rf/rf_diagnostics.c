#include "rf_diagnostics.h"

#include "rf_task.h"

void rf_tick_stats_update(struct rf_task *t, uint64_t now)
{
	if (!t || now == 0)
		return;
	if (t->tick_stats_last == 0) {
		t->tick_stats_last = now;
		return;
	}
	if (now <= t->tick_stats_last) {
		t->tick_stats_last = now;
		return;
	}
	uint32_t dt = (uint32_t)(now - t->tick_stats_last);
	t->tick_stats_last = now;
	if (dt == 0)
		return;

	t->tick_stats_count++;
	if (t->tick_stats_count == 1) {
		t->tick_stats_avg_ms = dt;
		t->tick_stats_min_ms = dt;
		t->tick_stats_max_ms = dt;
		return;
	}

	uint32_t avg = t->tick_stats_avg_ms;
	avg += (dt - avg) / t->tick_stats_count;
	t->tick_stats_avg_ms = avg;
	if (t->tick_stats_min_ms == 0 || dt < t->tick_stats_min_ms)
		t->tick_stats_min_ms = dt;
	if (dt > t->tick_stats_max_ms)
		t->tick_stats_max_ms = dt;
}

void rf_diagnostics_run(struct rf_task *t, uint64_t now)
{
	if (!t)
		return;

	t->diag_last_run_tick = now;

	int sum = 0;
	for (int i = 0; i < RF_NUM_CHANNELS; i++)
		sum += (int)t->energy_avg[i];
	t->diag_rf_ok = (sum > 0) || t->scan_active;

	/* SPI/RF IO stub: until a real nRF24 driver is wired in. */
	t->diag_spi_ok = 1;

	int32_t jitter = (int32_t)t->tick_stats_max_ms - (int32_t)t->tick_stats_min_ms;
	if (t->tick_stats_count == 0)
		t->diag_timing_ok = 0;
	else
		t->diag_timing_ok = jitter <= 2;

	int score = 100;
	if (t->record_err[0])
		score -= 40;
	if (t->pkt_dropped > 0)
		score -= 20;
	score = rf_clamp_int(score, 0, 100);
	t->diag_stability_score = score;

	rf_task_invalidate(t, RF_DIRTY_ANALYSIS | RF_DIRTY_STATUS);
}

