#include <stdio.h>
#include <string.h>

static int usage(void)
{
	fputs("usage: arp [-a]\n", stderr);
	return 1;
}

int main(int argc, char *argv[])
{
	if (argc == 1)
		return 0;
	if (argc == 2 && strcmp(argv[1], "-a") == 0)
		return 0;
	return usage();
}
