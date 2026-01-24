#ifndef RF_TASK_H
#define RF_TASK_H

#include "rf.h"
#include "rf_fb.h"

#include <stdint.h>
#include <stddef.h>

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

	uint64_t now_tick;
	uint64_t next_render_tick;

	/* TODO: port full Spark state machine incrementally. */
};

int rf_task_init(struct rf_task *t, int fb_mode, char *err, size_t errsz);
int rf_task_run(struct rf_task *t);
void rf_task_destroy(struct rf_task *t);

#endif

