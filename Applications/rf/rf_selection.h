#ifndef RF_SELECTION_H
#define RF_SELECTION_H

#include <stdint.h>

struct rf_task;

int rf_selection_selected_packet_addr(const struct rf_task *t, uint8_t *addr_len_out, uint8_t addr_out[5]);
int rf_selection_selected_packet_tick(const struct rf_task *t, uint64_t *tick_out);

#endif

