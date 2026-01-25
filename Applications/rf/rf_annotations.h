#ifndef RF_ANNOTATIONS_H
#define RF_ANNOTATIONS_H

#include <stdint.h>

/* Matches Spark rfanalyzer/annotations.go (with fixed-size strings). */
struct rf_annotation {
	uint64_t start_tick;
	uint64_t end_tick;
	char tag[17];
	char note[33];
};

static inline uint64_t rf_annotation_duration_ms(struct rf_annotation a)
{
	if (a.end_tick <= a.start_tick)
		return 0;
	return a.end_tick - a.start_tick;
}

#endif
