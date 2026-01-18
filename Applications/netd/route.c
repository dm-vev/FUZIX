#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int usage(void)
{
	fputs("usage:\n", stderr);
	fputs("  route\n", stderr);
	fputs("  route add default <gateway> [iface]\n", stderr);
	fputs("  route del default [iface]\n", stderr);
	return 1;
}

static int set_gateway(int sock, const char *iface, const char *gw)
{
	struct ifreq ifr;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_gwaddr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name));
	ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = 0;

	sin->sin_family = AF_INET;
	if (!inet_aton(gw, &sin->sin_addr)) {
		fprintf(stderr, "route: bad gateway: %s\n", gw);
		return 1;
	}
	if (ioctl(sock, SIOCSIFGWADDR, &ifr) < 0) {
		perror("route: SIOCSIFGWADDR");
		return 1;
	}
	return 0;
}

static int show_gateway(int sock, const char *iface)
{
	struct ifreq ifr;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_gwaddr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name));
	ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = 0;

	if (ioctl(sock, SIOCGIFGWADDR, &ifr) < 0) {
		perror("route: SIOCGIFGWADDR");
		return 1;
	}
	printf("default\t%s\n", inet_ntoa(sin->sin_addr));
	return 0;
}

int main(int argc, char *argv[])
{
	const char *iface = "ppp0";
	int sock;

	if (argc > 2 && argv[argc - 1][0] != '-') {
		/* iface is optional last argument */
		iface = argv[argc - 1];
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("route: socket");
		return 1;
	}

	if (argc == 1)
		return show_gateway(sock, iface);

	if (argc >= 4 && strcmp(argv[1], "add") == 0 && strcmp(argv[2], "default") == 0)
		return set_gateway(sock, iface, argv[3]);

	if (argc >= 3 && strcmp(argv[1], "del") == 0 && strcmp(argv[2], "default") == 0)
		return set_gateway(sock, iface, "0.0.0.0");

	return usage();
}
