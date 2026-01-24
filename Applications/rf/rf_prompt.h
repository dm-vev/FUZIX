#ifndef RF_PROMPT_H
#define RF_PROMPT_H

#include "rf_types.h"

struct rf_key;
struct rf_task;

void rf_prompt_open(struct rf_task *t, enum rf_prompt_kind kind, const char *title, const char *initial);
void rf_prompt_close(struct rf_task *t);
void rf_prompt_handle_key(struct rf_task *t, const struct rf_key *k);

#endif

