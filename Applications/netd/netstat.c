#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void print_addr(int sock, struct ifreq *ifr, int req)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
	if (ioctl(sock, req, ifr) < 0) {
		fputs("-", stdout);
		return;
	}
	fputs(inet_ntoa(sin->sin_addr), stdout);
}

int main(int argc, char *argv[])
{
	int sock;
	unsigned int i;

	(void)argc;
	(void)argv;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		fprintf(stderr, "netstat: networking not enabled.\n");
		return 1;
	}

	puts("Iface    MTU   Address         Gateway         Flags");
	for (i = 0; i < 32; i++) {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_ifindex = (int)i;
		if (ioctl(sock, SIOCGIFNAME, &ifr) < 0)
			continue;

		printf("%-8s ", ifr.ifr_name);

		if (ioctl(sock, SIOCGIFMTU, &ifr) == 0)
			printf("%-5d ", ifr.ifr_mtu);
		else
			printf("%-5s ", "-");

		print_addr(sock, &ifr, SIOCGIFADDR);
		fputs("  ", stdout);
		print_addr(sock, &ifr, SIOCGIFGWADDR);

		if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0)
			printf("  0x%04x", (unsigned int)ifr.ifr_flags);
		putchar('\n');
	}
	return 0;
}
