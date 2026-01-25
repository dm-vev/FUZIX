#ifndef RF_PRESET_PROFILES_H
#define RF_PRESET_PROFILES_H

#include <stdint.h>

struct rf_key;
struct rf_task;

void rf_preset_profiles_maybe_autoload(struct rf_task *t);

void rf_preset_profiles_open(struct rf_task *t);
void rf_preset_profiles_close(struct rf_task *t);
void rf_preset_profiles_handle_key(struct rf_task *t, const struct rf_key *k);
void rf_preset_profiles_render_overlay(const struct rf_task *t);

#endif
