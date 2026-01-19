#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 8000
#endif

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

int main(int argc, char **argv)
{
	int fd;
	int freq = 440;
	int ms = 200;
	int half;
	int remain;
	int count = 0;
	int high = 0;
	uint8_t buf[128];

	if (argc > 1)
		freq = atoi(argv[1]);
	if (argc > 2)
		ms = atoi(argv[2]);
	if (freq <= 0 || ms <= 0)
		return 1;

	fd = open("/dev/audio", O_WRONLY);
	if (fd < 0)
		return 1;

	half = AUDIO_SAMPLE_RATE / (freq * 2);
	if (half < 1)
		half = 1;

	remain = (AUDIO_SAMPLE_RATE * ms + 999) / 1000;
	while (remain) {
		uint16_t n = remain > (int)sizeof(buf) ? sizeof(buf) : (uint16_t)remain;
		uint16_t i;

		for (i = 0; i < n; i++) {
			if (count == 0) {
				high = !high;
				count = half;
			}
			count--;
			buf[i] = high ? (128 + 64) : (128 - 64);
		}

		if (write_all(fd, buf, n))
			return 1;
		remain -= n;
	}
	return 0;
}

