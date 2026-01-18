#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "netdb.h"

static int usage(void)
{
	fputs("usage: nc host port\n", stderr);
	return 1;
}

int main(int argc, char *argv[])
{
	struct sockaddr_in addr;
	struct hostent *he;
	int s;
	int port;
	pid_t pid;

	if (argc != 3)
		return usage();

	port = atoi(argv[2]);
	if (port <= 0 || port > 65535) {
		fputs("nc: invalid port\n", stderr);
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);

	he = gethostbyname(argv[1]);
	if (he == NULL) {
		fputs("nc: cannot resolve hostname\n", stderr);
		return 1;
	}
	if (he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
		fputs("nc: resolver returned no addresses\n", stderr);
		return 1;
	}
	memcpy(&addr.sin_addr.s_addr, he->h_addr_list[0], 4);

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("nc: socket");
		return 1;
	}
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("nc: connect");
		close(s);
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		perror("nc: fork");
		close(s);
		return 1;
	}
	if (pid == 0) {
		char buf[256];
		int n;
		while ((n = read(0, buf, sizeof(buf))) > 0) {
			if (write(s, buf, n) != n)
				break;
		}
		(void)shutdown(s, 1);
		_exit(0);
	}

	while (1) {
		char buf[256];
		int n = read(s, buf, sizeof(buf));
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		(void)write(1, buf, n);
	}
	close(s);
	return 0;
}
