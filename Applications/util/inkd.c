#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/graphics.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static int fbfd = -1;

static void inkd_shutdown(void)
{
	struct display disp;

	if (fbfd < 0)
		return;
	disp.mode = FB_MODE_TEXT;
	ioctl(fbfd, GFXIOC_SETMODE, &disp);
	ioctl(fbfd, FBIOC_UNLOCK, 0);
	close(fbfd);
	fbfd = -1;
}

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	struct display disp;
	struct fb_rect rect;
	int interval_us = 20000;

	(void)argc;
	(void)argv;

	fbfd = open("/dev/fb", O_RDWR);
	if (fbfd < 0) {
		perror("inkd: open /dev/fb");
		return 1;
	}
	if (ioctl(fbfd, FBIOC_LOCK, 0) != 0) {
		perror("inkd: lock");
		close(fbfd);
		return 1;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGHUP, sig_handler);

	memset(&disp, 0, sizeof(disp));
	disp.mode = FB_MODE_MEMORY;
	if (ioctl(fbfd, GFXIOC_SETMODE, &disp) != 0) {
		perror("inkd: set mode");
		inkd_shutdown();
		return 1;
	}

	while (running) {
		memset(&rect, 0, sizeof(rect));
		if (ioctl(fbfd, FBIOC_GETDIRTY, &rect) != 0) {
			usleep(interval_us);
			continue;
		}
		if (rect.w && rect.h) {
			if (ioctl(fbfd, FBIOC_FLUSH, &rect) == 0)
				ioctl(fbfd, FBIOC_CLEARDIRTY, 0);
		} else {
			usleep(interval_us);
		}
	}

	inkd_shutdown();
	return 0;
}
