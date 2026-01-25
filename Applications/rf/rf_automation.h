#ifndef RF_AUTOMATION_H
#define RF_AUTOMATION_H

#include <stddef.h>
#include <stdint.h>

struct rf_key;
struct rf_task;

void rf_automation_toggle_arm(struct rf_task *t);
void rf_automation_tick(struct rf_task *t, uint64_t tick);
void rf_automation_status_line(const struct rf_task *t, uint64_t now, char *out, size_t outsz);

void rf_automation_open(struct rf_task *t);
void rf_automation_close(struct rf_task *t);
void rf_automation_handle_key(struct rf_task *t, const struct rf_key *k);

#endif
