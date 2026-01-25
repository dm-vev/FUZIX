#include "rf_stress.h"

#include "rf_sniffer.h"
#include "rf_task.h"

#include <string.h>

static uint32_t xorshift32(uint32_t x)
{
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

static void u32_le(uint8_t out[4], uint32_t v)
{
	out[0] = (uint8_t)(v & 0xFF);
	out[1] = (uint8_t)((v >> 8) & 0xFF);
	out[2] = (uint8_t)((v >> 16) & 0xFF);
	out[3] = (uint8_t)((v >> 24) & 0xFF);
}

void rf_stress_start(struct rf_task *t, uint64_t now)
{
	if (!t)
		return;
	t->stress_running = 1;
	t->stress_start_tick = now;
	t->stress_next_tick = now;
	t->stress_sent = 0;
	t->stress_recv = 0;
	t->stress_lost = 0;
	t->stress_lat_avg_ms = 0;
	t->stress_lat_max_ms = 0;
	rf_task_invalidate(t, RF_DIRTY_ANALYSIS | RF_DIRTY_STATUS);
}

void rf_stress_stop(struct rf_task *t)
{
	if (!t)
		return;
	t->stress_running = 0;
	t->stress_next_tick = 0;
	rf_task_invalidate(t, RF_DIRTY_ANALYSIS | RF_DIRTY_STATUS);
}

void rf_stress_tick(struct rf_task *t, uint64_t now)
{
	if (!t)
		return;
	if (t->replay_active || !t->stress_running)
		return;
	if (t->stress_start_tick == 0) {
		rf_stress_start(t, now);
		return;
	}
	if (t->stress_duration_ms > 0 && now > t->stress_start_tick) {
		if ((now - t->stress_start_tick) >= (uint64_t)t->stress_duration_ms) {
			rf_stress_stop(t);
			return;
		}
	}

	int pps = rf_clamp_int(t->stress_pps, 1, 1000);
	uint64_t interval = 1000u / (uint64_t)pps;
	if (interval == 0)
		interval = 1;

	const int max_ops_per_tick = 6;
	for (int ops = 0; ops < max_ops_per_tick && t->stress_next_tick != 0 && now >= t->stress_next_tick; ops++) {
		t->stress_next_tick += interval;
		t->stress_sent++;

		int ch = rf_clamp_int(t->selected_channel, 0, RF_MAX_CHANNEL);
		uint8_t energy = t->energy_avg[ch];

		t->rng = xorshift32(t->rng);
		uint32_t x = t->rng;

		int drop_pct = 5;
		if (energy >= 220)
			drop_pct = 80;
		else if (energy >= 200)
			drop_pct = 60;
		else if (energy >= 180)
			drop_pct = 40;
		else if (energy >= 160)
			drop_pct = 25;
		else if (energy >= 120)
			drop_pct = 12;

		if ((int)(x & 0xFFu) < drop_pct * 256 / 100) {
			t->stress_lost++;
			continue;
		}

		uint32_t lat = 1u + (uint32_t)((int)energy / 64) + (x & 0x07u);
		t->stress_recv++;
		if (t->stress_recv == 1) {
			t->stress_lat_avg_ms = lat;
		} else {
			uint32_t avg = t->stress_lat_avg_ms;
			avg += (lat - avg) / t->stress_recv;
			t->stress_lat_avg_ms = avg;
		}
		if (lat > t->stress_lat_max_ms)
			t->stress_lat_max_ms = lat;

		struct rf_packet p;
		memset(&p, 0, sizeof(p));
		p.tick = now;
		p.channel = (uint8_t)ch;
		p.rate = t->data_rate;
		p.addr_len = 5;
		p.addr[0] = 0xD1;
		p.addr[1] = 0xA6;
		p.addr[2] = 0xE5;
		p.addr[3] = 0x5A;
		p.addr[4] = 0x01;
		p.length = 16;
		p.crc_len = 2;
		p.crc_ok = 1;

		uint8_t b[4];
		u32_le(b, t->stress_sent);
		memcpy(p.payload + 0, b, 4);
		u32_le(b, lat);
		memcpy(p.payload + 4, b, 4);
		memcpy(p.payload + 8, "STRESSOK", 8);

		rf_sniffer_append_packet(t, p);
	}
}

