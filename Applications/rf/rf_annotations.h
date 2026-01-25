#ifndef RF_ANNOTATIONS_H
#define RF_ANNOTATIONS_H

#include <stddef.h>
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

struct rf_task;

void rf_annotations_begin(struct rf_task *t, uint64_t start_tick);
int rf_annotations_add(struct rf_task *t, struct rf_annotation a, char *err, size_t errsz);
int rf_annotations_visible(const struct rf_task *t, uint64_t now, struct rf_annotation *out, int limit);

#endif
