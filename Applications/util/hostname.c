#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int usage(void)
{
	fputs("usage: hostname [name]\n", stderr);
	return 1;
}

int main(int argc, char *argv[])
{
	char buf[256];

	if (argc == 1) {
		if (gethostname(buf, sizeof(buf) - 1) < 0) {
			perror("hostname");
			return 1;
		}
		buf[sizeof(buf) - 1] = 0;
		puts(buf);
		return 0;
	}
	if (argc == 2) {
		size_t len = strlen(argv[1]);
		if (sethostname(argv[1], len) < 0) {
			perror("hostname");
			return 1;
		}
		return 0;
	}
	return usage();
}
