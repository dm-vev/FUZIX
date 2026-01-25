#ifndef RF_DIAGNOSTICS_H
#define RF_DIAGNOSTICS_H

#include <stdint.h>

struct rf_task;

void rf_tick_stats_update(struct rf_task *t, uint64_t now);
void rf_diagnostics_run(struct rf_task *t, uint64_t now);

#endif

