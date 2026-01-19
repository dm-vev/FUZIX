#include "vec_number.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
	fprintf(out, "vector (FUZIX) - Spark Vector port (WIP)\n");
	fprintf(out, "usage: vector\n");
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (argc > 1) {
		usage(stderr);
		return 1;
	}

	{
		char buf[64];
		vec_number pi = vec_float(M_PI);
		puts("Vector (FUZIX) - port in progress");
		printf("pi = %s\n", vec_number_string(pi, 12, buf, sizeof(buf)));
	}
	puts("Type :quit to exit.");

	for (;;) {
		char line[256];
		fputs("V> ", stdout);
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin)) {
			if (ferror(stdin))
				fprintf(stderr, "read: %s\n", strerror(errno));
			break;
		}
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!strcmp(line, ":quit") || !strcmp(line, ":q"))
			break;
		if (!line[0])
			continue;
		printf("echo: %s\n", line);
	}

	return 0;
}
