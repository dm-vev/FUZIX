#include <errno.h>
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

	puts("Vector (FUZIX) - port in progress");
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

