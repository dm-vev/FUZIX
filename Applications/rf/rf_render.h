#ifndef RF_RENDER_H
#define RF_RENDER_H

#include <stdint.h>

struct rf_task;

void rf_render_dirty(struct rf_task *t);

/* Helpers for building status lines. */
void rf_render_status_line1(const struct rf_task *t, char *out, unsigned outsz);
void rf_render_status_line2(const struct rf_task *t, char *out, unsigned outsz);

#endif

