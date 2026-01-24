#ifndef RF_PRESETS_H
#define RF_PRESETS_H

#include <stddef.h>

struct rf_task;

int rf_presets_save(struct rf_task *t, const char *name, char *err, size_t errsz);
int rf_presets_load(struct rf_task *t, const char *name, char *err, size_t errsz);

#endif
