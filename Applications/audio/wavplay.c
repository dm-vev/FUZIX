#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 8000
#endif

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_full(int fd, void *buf, uint16_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int write_all(int fd, const uint8_t *buf, uint16_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

static int skip_bytes(int fd, uint32_t len)
{
	uint8_t tmp[64];

	while (len) {
		uint16_t chunk = len > sizeof(tmp) ? sizeof(tmp) : (uint16_t)len;
		if (read_full(fd, tmp, chunk))
			return -1;
		len -= chunk;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int in_fd;
	int out_fd;
	uint8_t hdr[12];
	uint8_t chdr[8];
	uint8_t fmt[16];
	uint32_t sample_rate = 0;
	uint16_t channels = 0;
	uint16_t bits = 0;
	uint16_t format = 0;
	uint8_t buf[256];
	int have_fmt = 0;

	if (argc != 2)
		return 1;

	in_fd = open(argv[1], O_RDONLY);
	if (in_fd < 0)
		return 1;

	if (read_full(in_fd, hdr, sizeof(hdr)))
		return 1;
	if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
		return 1;

	out_fd = open("/dev/audio", O_WRONLY);
	if (out_fd < 0)
		return 1;

	for (;;) {
		uint32_t size;

		if (read(in_fd, chdr, sizeof(chdr)) != (ssize_t)sizeof(chdr))
			break;

		size = get_le32(chdr + 4);

		if (memcmp(chdr, "fmt ", 4) == 0) {
			if (size < sizeof(fmt))
				return 1;
			if (read_full(in_fd, fmt, sizeof(fmt)))
				return 1;

			format = get_le16(fmt + 0);
			channels = get_le16(fmt + 2);
			sample_rate = get_le32(fmt + 4);
			bits = get_le16(fmt + 14);

			if (skip_bytes(in_fd, size - sizeof(fmt)))
				return 1;
			if (size & 1) {
				uint8_t pad;
				if (read_full(in_fd, &pad, 1))
					return 1;
			}
			have_fmt = 1;
			continue;
		}

		if (memcmp(chdr, "data", 4) == 0) {
			uint32_t remain = size;

			if (!have_fmt)
				return 1;
			if (format != 1 || channels != 1 || bits != 8 || sample_rate != AUDIO_SAMPLE_RATE)
				return 1;

			while (remain) {
				uint16_t want = remain > sizeof(buf) ? sizeof(buf) : (uint16_t)remain;
				if (read_full(in_fd, buf, want))
					return 1;
				if (write_all(out_fd, buf, want))
					return 1;
				remain -= want;
			}
			if (size & 1) {
				uint8_t pad;
				if (read_full(in_fd, &pad, 1))
					return 1;
			}
			continue;
		}

		if (skip_bytes(in_fd, size))
			return 1;
		if (size & 1) {
			uint8_t pad;
			if (read_full(in_fd, &pad, 1))
				return 1;
		}
	}

	return 0;
}

