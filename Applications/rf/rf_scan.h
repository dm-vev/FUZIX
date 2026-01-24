#ifndef RF_SCAN_H
#define RF_SCAN_H

#include <stdint.h>

struct rf_task;

void rf_scan_tick(struct rf_task *t, uint64_t tick);

#endif

