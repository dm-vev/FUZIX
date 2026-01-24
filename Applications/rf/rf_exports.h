#ifndef RF_EXPORTS_H
#define RF_EXPORTS_H

#include <stddef.h>

struct rf_task;

int rf_exports_export_csv(struct rf_task *t, const char *name, char *err, size_t errsz);
int rf_exports_export_pcap(struct rf_task *t, const char *name, char *err, size_t errsz);
int rf_exports_export_rfpkt(struct rf_task *t, const char *name, char *err, size_t errsz);

#endif

