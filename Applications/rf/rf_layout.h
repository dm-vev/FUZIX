#ifndef RF_LAYOUT_H
#define RF_LAYOUT_H

#include <stdint.h>

struct rf_task;

struct rf_rect {
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
};

static inline struct rf_rect rf_rect_inset(struct rf_rect r, int16_t dx, int16_t dy)
{
	int16_t nx = (int16_t)(r.x + dx);
	int16_t ny = (int16_t)(r.y + dy);
	int16_t nw = (int16_t)(r.w - 2 * dx);
	int16_t nh = (int16_t)(r.h - 2 * dy);
	if (nw < 0)
		nw = 0;
	if (nh < 0)
		nh = 0;
	return (struct rf_rect){.x = nx, .y = ny, .w = nw, .h = nh};
}

struct rf_layout {
	struct rf_rect menu;
	struct rf_rect toolbar;
	struct rf_rect status1;
	struct rf_rect status2;
	struct rf_rect spectrum;
	struct rf_rect waterfall;
	struct rf_rect rf;
	struct rf_rect sniffer;
	struct rf_rect proto;
	struct rf_rect analysis;

	int left_cols;
	int right_main_cols;
	int analysis_cols;
};

struct rf_layout rf_compute_layout(const struct rf_task *t);

#endif

