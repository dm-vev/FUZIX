#ifndef RF_REPLAY_H
#define RF_REPLAY_H

#include <stddef.h>
#include <stdint.h>

struct rf_task;

int rf_replay_enter(struct rf_task *t, const char *input, char *err, size_t errsz);
void rf_replay_exit(struct rf_task *t);
void rf_replay_tick(struct rf_task *t, uint64_t host_tick);
void rf_replay_seek_ms(struct rf_task *t, uint64_t offset_ms);
void rf_replay_reset_view(struct rf_task *t);
void rf_replay_update_packet_cache(struct rf_task *t);
void rf_replay_time_text(const struct rf_task *t, char *out, size_t outsz);

#endif

