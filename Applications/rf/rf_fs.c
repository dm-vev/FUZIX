#include "rf_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

size_t rf_sanitize_name(const char *in, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return 0;
	out[0] = 0;
	if (!in)
		return 0;

	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		unsigned char c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.') {
			if (n + 1 < outsz)
				out[n++] = (char)c;
			continue;
		}
		if (c == ' ') {
			if (n + 1 < outsz)
				out[n++] = '_';
			continue;
		}
	}
	if (n >= outsz)
		n = outsz - 1;
	out[n] = 0;
	return n;
}

int rf_fs_write_all(int fd, const void *buf, size_t len, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (fd < 0 || (!buf && len))
		return -1;

	const unsigned char *p = (const unsigned char *)buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (err && errsz)
				snprintf(err, errsz, "write: %s", strerror(errno));
			return -1;
		}
		if (n == 0) {
			if (err && errsz)
				snprintf(err, errsz, "write: short write");
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

int rf_fs_ensure_dir(const char *path, unsigned mode, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!path || !path[0]) {
		if (err && errsz)
			snprintf(err, errsz, "mkdir: empty path");
		return -1;
	}

	if (mkdir(path, (mode_t)mode) == 0)
		return 0;
	if (errno != EEXIST) {
		if (err && errsz)
			snprintf(err, errsz, "mkdir %s: %s", path, strerror(errno));
		return -1;
	}

	struct stat st;
	if (stat(path, &st) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "stat %s: %s", path, strerror(errno));
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (err && errsz)
			snprintf(err, errsz, "%s: not a directory", path);
		return -1;
	}
	return 0;
}

