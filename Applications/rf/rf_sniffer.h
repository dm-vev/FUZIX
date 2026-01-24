#ifndef RF_SNIFFER_H
#define RF_SNIFFER_H

#include "rf.h"
#include "rf_types.h"

#include <stdint.h>

enum {
	RF_PKT_FLAG_RETRY = 1u << 0,
	RF_PKT_FLAG_BURST = 1u << 1,
};

struct rf_packet {
	uint32_t seq;
	uint64_t tick;

	uint16_t delta_ms;
	uint8_t flags;

	uint8_t channel;
	enum rf_data_rate rate;

	uint8_t addr_len;
	uint8_t addr[5];

	uint8_t length;
	uint8_t payload[32];

	uint32_t payload_hash;

	uint8_t crc_len;
	uint8_t crc[2];
	uint8_t crc_ok;
};

struct rf_packet_summary {
	uint32_t seq;
	uint64_t tick;

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

struct rf_task;

char rf_rate_short(enum rf_data_rate r);
void rf_crc_text(uint8_t crc_len, uint8_t crc_ok, char out[3]);
void rf_addr_suffix3(uint8_t addr_len, const uint8_t addr[5], char out[7]);

void rf_sniffer_tick_pps(struct rf_task *t, uint64_t now);
void rf_sniffer_append_packet(struct rf_task *t, struct rf_packet p);
void rf_sniffer_maybe_capture_packet(struct rf_task *t, int ch, uint8_t energy, uint64_t tick);

int rf_sniffer_filtered_count(const struct rf_task *t);
int rf_sniffer_filtered_packet_summary_by_index(const struct rf_task *t, int idx, struct rf_packet_summary *out);
const struct rf_packet *rf_sniffer_filtered_live_packet_by_index(const struct rf_task *t, int idx);
void rf_sniffer_reconcile_selection(struct rf_task *t);
void rf_sniffer_move_selection(struct rf_task *t, int delta);

void rf_sniffer_filter_summary(const struct rf_task *t, char *out, unsigned outsz);

#endif

