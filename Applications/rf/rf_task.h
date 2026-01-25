#ifndef RF_TASK_H
#define RF_TASK_H

#include "rf.h"
#include "rf_fb.h"
#include "rf_types.h"
#include "rf_sniffer.h"

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
	int record_fd;
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

	int show_help;
	int show_filters;

	int show_prompt;
	enum rf_prompt_kind prompt_kind;
	char prompt_title[64];
	char prompt_err[64];
	uint32_t prompt_buf[32];
	int prompt_len;
	int prompt_cursor;

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
	struct rf_packet replay_pkt_cache;
	uint32_t replay_pkt_cache_seq;
	int replay_pkt_cache_ok;
	struct rf_session *compare;
	char compare_err[64];

	uint32_t rng;

	uint64_t tick_stats_last;
	uint32_t tick_stats_count;
	uint32_t tick_stats_avg_ms;
	uint32_t tick_stats_min_ms;
	uint32_t tick_stats_max_ms;

	uint64_t diag_last_run_tick;
	int diag_rf_ok;
	int diag_spi_ok;
	int diag_timing_ok;
	int diag_stability_score;

	int stress_running;
	int stress_pps;
	int stress_duration_ms;
	uint64_t stress_next_tick;
	uint64_t stress_start_tick;
	uint32_t stress_sent;
	uint32_t stress_recv;
	uint32_t stress_lost;
	uint32_t stress_lat_avg_ms;
	uint32_t stress_lat_max_ms;

	enum rf_protocol_mode proto_mode;

	enum rf_analysis_view analysis_view;
	int analysis_sel;
	int analysis_top;

	uint32_t ana_sweep_count;
	uint32_t ana_occ_count[RF_NUM_CHANNELS];
	uint32_t ana_energy_sum[RF_NUM_CHANNELS];
	uint32_t ana_chan_pkt[RF_NUM_CHANNELS];
	uint32_t ana_chan_bad[RF_NUM_CHANNELS];
	uint32_t ana_chan_retry[RF_NUM_CHANNELS];

	uint8_t ana_high[RF_NUM_CHANNELS];
	uint64_t ana_last_rise[RF_NUM_CHANNELS];
	uint8_t ana_rise_count[RF_NUM_CHANNELS];
	uint32_t ana_rise_avg_ms[RF_NUM_CHANNELS];
	uint32_t ana_rise_min_ms[RF_NUM_CHANNELS];
	uint32_t ana_rise_max_ms[RF_NUM_CHANNELS];

	struct rf_best_chan_entry {
		uint64_t tick;
		uint8_t ch;
		uint8_t score;
	} best_hist[64];
	int best_head;
	int best_count;
	uint64_t best_next_tick;

	struct rf_device_stat {
		uint8_t used;

		uint8_t addr_len;
		uint8_t addr[5];

		uint64_t first_tick;
		uint64_t last_tick;

		uint8_t last_channel;
		uint32_t hop_count;

		uint32_t pkt_count;
		uint32_t crc_ok;
		uint32_t crc_bad;

		uint32_t retries;
		uint32_t last_payload_hash;
		uint64_t last_payload_tick;

		uint32_t int_count;
		uint32_t int_avg;
		uint32_t int_jitter;
		uint32_t int_min;
		uint32_t int_max;
		uint16_t burst_cur;
		uint16_t burst_max;
		uint32_t burst_count;

		uint8_t hop_seq_head;
		uint8_t hop_seq_count;
		uint8_t hop_seq[16];
	} devices[RF_MAX_DEVICES];
	int device_count;

	uint8_t occ_hist[RF_OCC_HIST_LEN][RF_OCC_BYTES];
	int occ_hist_head;
	int occ_hist_count;

	struct rf_packet packets[RF_MAX_PACKETS];
	int pkt_head;
	int pkt_count;
	uint32_t pkt_seq;
	uint32_t pkt_dropped;
	uint64_t pkt_sec_start;
	int pkt_sec_count;
	int pkts_per_sec;

	int sniffer_sel;
	int sniffer_top;
	uint32_t sniffer_sel_seq;

	enum rf_filter_crc filter_crc;
	enum rf_filter_channel filter_channel;
	int filter_min_len;
	int filter_max_len;
	uint8_t filter_addr[5];
	uint8_t filter_addr_mask[5];
	int filter_addr_len;
	uint8_t filter_payload[RF_PAYLOAD_PREFIX_BYTES];
	uint8_t filter_payload_mask[RF_PAYLOAD_PREFIX_BYTES];
	int filter_payload_len;
	int filter_age_ms;
	int filter_burst_max_ms;
	int filter_sel;
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
