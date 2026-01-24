#ifndef RF_H
#define RF_H

#include <stdint.h>
#include <stddef.h>

/* Shared definitions for the rf analyzer application. */

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
	RF_MAX_DEVICES = 32,
	RF_OCC_HIST_LEN = 64,
	RF_ANNOT_MAX = 64,
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

