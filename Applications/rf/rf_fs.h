#ifndef RF_FS_H
#define RF_FS_H

#include <stddef.h>

struct rf_task;

size_t rf_sanitize_name(const char *in, char *out, size_t outsz);
int rf_fs_ensure_dir(const char *path, unsigned mode, char *err, size_t errsz);
int rf_fs_write_all(int fd, const void *buf, size_t len, char *err, size_t errsz);

#endif
