#ifndef RF_WATERFALL_H
#define RF_WATERFALL_H

#include "rf_layout.h"

#include <stdint.h>

struct rf_task;

int rf_waterfall_plot_rect(const struct rf_task *t, struct rf_layout l, struct rf_rect *plot, int16_t *header_y);
int rf_waterfall_ensure_alloc(struct rf_task *t);
void rf_waterfall_push_row(struct rf_task *t);
void rf_waterfall_rebuild_palette(struct rf_task *t);
void rf_waterfall_blit(const struct rf_task *t, struct rf_rect plot);

#endif

