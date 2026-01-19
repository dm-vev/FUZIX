#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pico_ioctl.h>

static void usage(void)
{
	puts("usage: picocalcd [--help] [--interval <seconds>] [--out <path>]");
}

int main(int argc, char **argv)
{
	const char *out_path = "/tmp/picocalc.status";
	unsigned interval = 1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		}
		if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
			interval = (unsigned)strtoul(argv[++i], NULL, 10);
			if (interval == 0)
				interval = 1;
			continue;
		}
		if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			out_path = argv[++i];
			continue;
		}
		usage();
		return 1;
	}

	int fd = open("/dev/sys", O_RDWR, 0);
	if (fd < 0) {
		perror("picocalcd: open /dev/sys");
		return 1;
	}

	for (;;) {
		struct picocalc_status st;
		struct picocalc_i2c_stats i2c;
		int ok_st = (ioctl(fd, PICOIOC_GET_STATUS, &st) == 0);
		int ok_i2c = (ioctl(fd, PICOIOC_GET_I2C_STATS, &i2c) == 0);

		char buf[256];
		size_t n = 0;

		if (ok_st) {
			if (st.battery_percent == 0xFF)
				n += (size_t)snprintf(buf + n, sizeof(buf) - n, "bat=unknown ");
			else
				n += (size_t)snprintf(buf + n, sizeof(buf) - n, "bat=%u%%%s ",
				                      st.battery_percent,
				                      (st.battery_flags & PICOCALC_BATF_CHARGING) ? "(chg)" : "");
			if (st.lcd_backlight != 0xFF)
				n += (size_t)snprintf(buf + n, sizeof(buf) - n, "lcd_bl=%u ", st.lcd_backlight);
			if (st.kbd_backlight != 0xFF)
				n += (size_t)snprintf(buf + n, sizeof(buf) - n, "kbd_bl=%u ", st.kbd_backlight);
		} else {
			n += (size_t)snprintf(buf + n, sizeof(buf) - n, "status=EIO ");
		}

		if (ok_i2c) {
			n += (size_t)snprintf(buf + n, sizeof(buf) - n,
			                      "i2c_backoff_us=%lu fifo_err=%lu reg_err=%lu wr_err=%lu\n",
			                      (unsigned long)i2c.backoff_us,
			                      (unsigned long)i2c.fifo_errors,
			                      (unsigned long)i2c.reg_errors,
			                      (unsigned long)i2c.write_errors);
		} else {
			n += (size_t)snprintf(buf + n, sizeof(buf) - n, "i2cstats=EIO\n");
		}

		int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out >= 0) {
			(void)write(out, buf, n);
			close(out);
		}

		sleep(interval);
	}
}

