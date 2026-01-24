#ifndef RF_RECORDING_H
#define RF_RECORDING_H

#include <stddef.h>
#include <stdint.h>

struct rf_packet;
struct rf_task;

int rf_recording_start(struct rf_task *t, const char *name, char *err, size_t errsz);
int rf_recording_stop(struct rf_task *t, char *err, size_t errsz);
void rf_recording_flush(struct rf_task *t, uint64_t now, int force);

void rf_recording_record_config(struct rf_task *t, uint64_t now);
void rf_recording_record_sweep(struct rf_task *t, uint64_t now);
void rf_recording_record_packet(struct rf_task *t, const struct rf_packet *p);

#endif
