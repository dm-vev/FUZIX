#ifndef RF_STRESS_H
#define RF_STRESS_H

#include <stdint.h>

struct rf_task;

void rf_stress_start(struct rf_task *t, uint64_t now);
void rf_stress_stop(struct rf_task *t);
void rf_stress_tick(struct rf_task *t, uint64_t now);

#endif

