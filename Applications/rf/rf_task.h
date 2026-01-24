#ifndef RF_TASK_H
#define RF_TASK_H

#include "rf.h"
#include "rf_fb.h"
#include "rf_types.h"

#include <stdint.h>
#include <stddef.h>

struct rf_session;

enum rf_focus_panel {
	RF_FOCUS_SPECTRUM = 0,
	RF_FOCUS_WATERFALL,
	RF_FOCUS_RFCONTROL,
	RF_FOCUS_SNIFFER,
	RF_FOCUS_PROTOCOL,
	RF_FOCUS_ANALYSIS,
};

enum rf_dirty_flags {
	RF_DIRTY_HEADER = 1u << 0,
	RF_DIRTY_SPECTRUM = 1u << 1,
	RF_DIRTY_WATERFALL = 1u << 2,
	RF_DIRTY_RFCONTROL = 1u << 3,
	RF_DIRTY_SNIFFER = 1u << 4,
	RF_DIRTY_PROTOCOL = 1u << 5,
	RF_DIRTY_ANALYSIS = 1u << 6,
	RF_DIRTY_STATUS = 1u << 7,
	RF_DIRTY_OVERLAY = 1u << 8,
	RF_DIRTY_ALL = (1u << 9) - 1,
};

struct rf_task {
	struct rf_fb fb;
	int cols;
	int rows;
	int main_rows;

	uint16_t dirty;
	int active;
	enum rf_focus_panel focus;

	uint8_t inbuf[256];
	size_t inlen;

	uint64_t now_tick;
	uint64_t next_render_tick;

	int scan_active;
	int waterfall_frozen;
	int capture_paused;
	int selected_channel;
	int channel_range_lo;
	int channel_range_hi;
	int dwell_time_ms;
	int scan_speed_scalar;
	enum rf_data_rate data_rate;
	enum rf_crc_mode crc_mode;
	int auto_ack;
	enum rf_power_level power_level;
	int selected_setting;

	int scan_chan;
	uint64_t scan_next_tick;
	uint64_t sweep_count;
	uint64_t last_sweep_tick;

	uint8_t energy_cur[RF_NUM_CHANNELS];
	uint8_t energy_avg[RF_NUM_CHANNELS];
	uint8_t energy_peak[RF_NUM_CHANNELS];

	enum rf_wf_palette wf_palette;
	struct rf_color wf_palette888[256];
	int wf_w;
	int wf_h;
	int wf_head;
	uint8_t *wf_buf;
	size_t wf_cap;

	char active_preset[32];
	int preset_dirty;

	int recording;
	char record_name[32];
	char record_path[64];
	uint8_t *record_buf;
	size_t record_len;
	size_t record_cap;
	uint64_t record_next_flush_tick;
	uint32_t record_sweeps;
	uint32_t record_packets;
	uint32_t record_bytes;
	char record_err[64];

	int show_menu;
	enum rf_menu_category menu_cat;
	int menu_sel;

	int replay_active;
	int replay_playing;
	int replay_speed;
	uint64_t replay_host_last_tick;
	uint64_t replay_now_tick;
	int replay_sweep_idx;
	int replay_pkt_limit;
	int replay_cfg_idx;
	struct rf_session *replay;
	char replay_err[64];

	uint32_t rng;
};

static inline void rf_task_invalidate(struct rf_task *t, uint16_t flags)
{
	if (t)
		t->dirty |= flags;
}

int rf_task_init(struct rf_task *t, int fb_mode, char *err, size_t errsz);
int rf_task_run(struct rf_task *t);
void rf_task_destroy(struct rf_task *t);

#endif
