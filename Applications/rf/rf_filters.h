#ifndef RF_FILTERS_H
#define RF_FILTERS_H

struct rf_key;
struct rf_task;

void rf_filters_toggle(struct rf_task *t);
void rf_filters_handle_key(struct rf_task *t, const struct rf_key *k);

#endif

