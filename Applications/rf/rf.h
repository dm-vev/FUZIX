#ifndef RF_H
#define RF_H

#include <stdint.h>
#include <stddef.h>

/* Shared definitions for the rf analyzer application. */

struct rf_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/* Header/footer rows match Spark rfanalyzer. */
enum {
	RF_HEADER_ROWS = 2,
	RF_STATUS_ROWS = 2,
};

/* Radio constants match Spark rfanalyzer. */
enum {
	RF_MAX_CHANNEL = 125,
	RF_NUM_CHANNELS = 126,
};

/* App-wide limits (tuned for small systems). */
enum {
	RF_MAX_PACKETS = 256,
	RF_PAYLOAD_PREFIX_BYTES = 8,
	RF_MAX_DEVICES = 64,
	RF_OCC_HIST_LEN = 128,
	RF_ANNOT_MAX = 64,
};

enum {
	RF_OCC_BYTES = (RF_NUM_CHANNELS + 7) / 8,
};

enum {
	RF_ANA_OCC_THRESHOLD = 160,
	RF_ANA_RETRY_WINDOW_TICKS = 60,
	RF_ANA_BEST_INTERVAL_TICKS = 10 * 1000,
	RF_ANA_PERIODIC_MIN_INTERVALS = 3,
};

/* Utility clamp helpers. */
static inline int rf_clamp_int(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static inline uint8_t rf_clamp_u8_int(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

#endif
