#include "rf_task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-m direct|memory]\n", argv0 ? argv0 : "rf");
}

int main(int argc, char **argv)
{
	int fb_mode = RF_FB_MODE_MEMORY;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "direct")) {
				fb_mode = RF_FB_MODE_DIRECT;
			} else if (!strcmp(argv[i], "memory")) {
				fb_mode = RF_FB_MODE_MEMORY;
			} else {
				usage(argv[0]);
				return 1;
			}
			continue;
		}
		usage(argv[0]);
		return 1;
	}

	struct rf_task task;
	char err[128];
	if (rf_task_init(&task, fb_mode, err, sizeof(err)) != 0) {
		fprintf(stderr, "%s\n", err[0] ? err : "rf: init failed");
		return 1;
	}

	int rc = rf_task_run(&task);
	rf_task_destroy(&task);
	return rc;
}

