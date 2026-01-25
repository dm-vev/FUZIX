#ifndef RF_SESSION_H
#define RF_SESSION_H

#include "rf_annotations.h"
#include "rf.h"
#include "rf_types.h"

#include <stddef.h>
#include <stdint.h>

struct rf_packet;

struct rf_session_sweep_index {
	uint32_t off;
	uint64_t tick;
};

struct rf_session_packet_meta {
	uint32_t off;
	uint64_t tick;
	uint32_t seq;

	uint16_t delta_ms;
	uint8_t flags;

	uint8_t channel;
	enum rf_data_rate rate;

	uint8_t addr_len;
	uint8_t addr[5];

	uint8_t length;

	uint32_t payload_hash;
	uint8_t payload_prefix[RF_PAYLOAD_PREFIX_BYTES];

	uint8_t crc_len;
	uint8_t crc_ok;
};

struct rf_session_cfg_snapshot {
	int channel_range_lo;
	int channel_range_hi;
	int dwell_time_ms;
	int scan_step;
	enum rf_data_rate data_rate;
	enum rf_crc_mode crc_mode;
	int auto_ack;
	enum rf_power_level power_level;
	enum rf_wf_palette wf_palette;
};

struct rf_session_config_event {
	uint64_t tick;
	int selected_channel;
	struct rf_session_cfg_snapshot cfg;
};

struct rf_session {
	char name[32];
	char path[96];
	int fd;
	uint32_t size;

	uint64_t start_tick;
	uint64_t end_tick;

	uint32_t sweep_count_total;
	uint32_t occ_count[RF_NUM_CHANNELS];
	uint64_t energy_sum[RF_NUM_CHANNELS];
	uint32_t pkt_count[RF_NUM_CHANNELS];
	uint32_t pkt_bad[RF_NUM_CHANNELS];

	uint32_t bucket_ms;
	uint8_t *band_occ_pct;
	size_t band_occ_pct_len;

	struct rf_session_sweep_index *sweeps;
	size_t sweep_count;
	size_t sweep_cap;

	struct rf_session_packet_meta *packets;
	size_t packet_count;
	size_t packet_cap;

	struct rf_session_config_event *configs;
	size_t config_count;
	size_t config_cap;

	struct rf_annotation *annotations;
	size_t annotation_count;
	size_t annotation_cap;
};

struct rf_session *rf_session_load(const char *input, char *err, size_t errsz);
void rf_session_free(struct rf_session *s);

int rf_session_read_sweep(const struct rf_session *s, uint32_t off, uint64_t *tick_out, uint8_t energy_avg[RF_NUM_CHANNELS],
			  char *err, size_t errsz);
int rf_session_read_packet(const struct rf_session *s, uint32_t off, struct rf_packet *out, char *err, size_t errsz);

#endif
