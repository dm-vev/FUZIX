#ifndef RF_ANALYTICS_H
#define RF_ANALYTICS_H

#include <stddef.h>
#include <stdint.h>

struct rf_task;
struct rf_packet;
struct rf_session_packet_meta;
struct rf_device_stat;

struct rf_chan_score {
	int ch;
	int score;
};

struct rf_corr_entry {
	int ch;
	int jacc_pct;
	int both;
};

struct rf_conflict_row {
	int ch;
	int bad_pct;
	int retry_pct;
	uint32_t pkt;
	int score;
};

void rf_analytics_reset(struct rf_task *t);
void rf_analytics_on_sweep(struct rf_task *t, uint64_t tick);
void rf_analytics_prepare_live_packet(struct rf_task *t, struct rf_packet *p);
void rf_analytics_on_packet(struct rf_task *t, const struct rf_packet *p);
void rf_analytics_on_packet_meta(struct rf_task *t, const struct rf_session_packet_meta *m);

int rf_analytics_channel_score(const struct rf_task *t, int ch);
int rf_analytics_best_channel_now(const struct rf_task *t, int *ch_out, int *score_out);
int rf_analytics_top_channels(const struct rf_task *t, int n, struct rf_chan_score *out, int out_cap);
void rf_analytics_periodic_text(const struct rf_task *t, int ch, char *out, size_t outsz);
void rf_analytics_best_history_line(const struct rf_task *t, int n, char *out, size_t outsz);

struct rf_device_stat *rf_analytics_find_device(struct rf_task *t, uint8_t addr_len, const uint8_t addr[5]);
const struct rf_device_stat *rf_analytics_find_device_const(const struct rf_task *t, uint8_t addr_len, const uint8_t addr[5]);
int rf_analytics_top_device_indices(const struct rf_task *t, int n, int *out, int out_cap);
void rf_analytics_device_hop_seq_text(const struct rf_device_stat *d, int n, char *out, size_t outsz);

int rf_analytics_top_correlated_channels(const struct rf_task *t, int ref_ch, int n, struct rf_corr_entry *out, int out_cap);
int rf_analytics_top_conflict_channels(const struct rf_task *t, int n, struct rf_conflict_row *out, int out_cap);

#endif

