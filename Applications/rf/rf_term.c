#include "rf_term.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios rf_saved;
static int rf_have_saved;

int rf_term_setup_stdin(char *err, size_t errsz)
{
	struct termios t;

	if (tcgetattr(0, &t) != 0) {
		snprintf(err, errsz, "rf: tcgetattr: %s", strerror(errno));
		return -1;
	}
	rf_saved = t;
	rf_have_saved = 1;

	(void)cfmakeraw(&t);
	t.c_iflag &= ~(IXON | IXOFF);
	t.c_oflag |= OPOST;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &t) != 0) {
		snprintf(err, errsz, "rf: tcsetattr: %s", strerror(errno));
		return -1;
	}
	return 0;
}

void rf_term_restore_stdin(void)
{
	if (!rf_have_saved)
		return;
	(void)tcsetattr(0, TCSANOW, &rf_saved);
	rf_have_saved = 0;
}

