/*
 * PPP over UART for netd (uIP) on FUZIX.
 *
 * Supports:
 *  - PPP framing (async HDLC), FCS16, escape/unescape
 *  - LCP (MRU, ACCM, PAP auth negotiation, echo keepalive)
 *  - IPCP (IPv4, DNS options)
 *  - ESP-AT and SIM800 AT init profiles
 *
 * This is a point-to-point L3 device: netd expects an Ethernet-like buffer
 * and we fake the ethertype field (0x0800) like slip.c does.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/net_native.h>

#include "device.h"
#include "uip.h"

extern int knet;
extern int netd_argc;
extern char **netd_argv;

/* netd device globals */
uint8_t has_arp = 0;
uint16_t mtu = 1280;

static int fd = -1;

enum modem_type {
	MODEM_ESPAT = 1,
	MODEM_SIM800 = 2,
};

struct ppp_cfg {
	char device[64];
	enum modem_type type;
	uint8_t debug;

	/* ESP-AT */
	char wifi_ssid[64];
	char wifi_pass[64];

	/* SIM800 */
	char apn[64];
	char user[64];
	char pass[64];
};

static struct ppp_cfg cfg;

struct ppp_state {
	uint8_t lcp_id;
	uint8_t ipcp_id;
	uint8_t pap_id;

	uint8_t lcp_us_up;
	uint8_t lcp_them_up;
	uint8_t ipcp_us_up;
	uint8_t ipcp_them_up;
	uint8_t pap_done;

	uint16_t our_mru;
	uint16_t peer_mru;

	uint32_t our_ip;
	uint32_t peer_ip;
	uint32_t dns1;
	uint32_t dns2;

	uint32_t magic;

	time_t last_rx;
	time_t last_echo;
	uint8_t echo_failures;

	uint16_t auth_proto;

	uint8_t ipcp_send_dns;
};

static struct ppp_state st;

/* PPP framing */
#define PPP_FLAG	0x7E
#define PPP_ESC		0x7D
#define PPP_ESC_XOR	0x20

/* PPP protocol values */
#define PPP_PROTO_IP		0x0021
#define PPP_PROTO_LCP		0xC021
#define PPP_PROTO_PAP		0xC023
#define PPP_PROTO_IPCP		0x8021

/* LCP codes */
#define LCP_CONF_REQ		1
#define LCP_CONF_ACK		2
#define LCP_CONF_NAK		3
#define LCP_CONF_REJ		4
#define LCP_TERM_REQ		5
#define LCP_TERM_ACK		6
#define LCP_ECHO_REQ		9
#define LCP_ECHO_REP		10

/* IPCP codes */
#define IPCP_CONF_REQ		1
#define IPCP_CONF_ACK		2
#define IPCP_CONF_NAK		3
#define IPCP_CONF_REJ		4

/* PAP codes */
#define PAP_AUTH_REQ		1
#define PAP_AUTH_ACK		2
#define PAP_AUTH_NAK		3

/* LCP options */
#define LCP_OPT_MRU		1
#define LCP_OPT_ACCM		2
#define LCP_OPT_AUTH		3
#define LCP_OPT_MAGIC		5

/* IPCP options */
#define IPCP_OPT_IPADDR		3
#define IPCP_OPT_DNS1		129
#define IPCP_OPT_DNS2		131

static void logf(const char *fmt, ...)
{
	va_list ap;
	if (!cfg.debug)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void log_perror(const char *what)
{
	if (cfg.debug)
		perror(what);
}

static int write_all(int wfd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = write(wfd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += (size_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static char *trim(char *s)
{
	char *e;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e))
		*e-- = 0;
	return s;
}

static int parse_bool(const char *v)
{
	if (!v || !*v)
		return 0;
	if (!strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
	    !strcasecmp(v, "on"))
		return 1;
	return 0;
}

static int load_ppp_conf(const char *section)
{
	FILE *f;
	char line[256];
	char cur[32];
	int in = 0;
	int found = 0;

	memset(&cfg, 0, sizeof(cfg));
	memset(cur, 0, sizeof(cur));
	strncpy(cfg.device, "/dev/ttyS0", sizeof(cfg.device) - 1);
	cfg.type = MODEM_ESPAT;

	f = fopen("/etc/ppp/ppp.conf", "r");
	if (!f) {
		log_perror("/etc/ppp/ppp.conf");
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		char *p, *eq, *k, *v;
		p = line;
		if ((p = strchr(p, '#')))
			*p = 0;
		p = line;
		if ((p = strchr(p, ';')))
			*p = 0;
		p = trim(line);
		if (!*p)
			continue;
		if (*p == '[') {
			char *r = strchr(p, ']');
			if (!r)
				continue;
			*r = 0;
			strncpy(cur, trim(p + 1), sizeof(cur) - 1);
			cur[sizeof(cur) - 1] = 0;
			in = (section && *section && strcmp(cur, section) == 0);
			if (in)
				found = 1;
			continue;
		}
		if (!in)
			continue;
		eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = 0;
		k = trim(p);
		v = trim(eq + 1);
		if (!*k)
			continue;

		if (!strcmp(k, "device")) {
			strncpy(cfg.device, v, sizeof(cfg.device) - 1);
			cfg.device[sizeof(cfg.device) - 1] = 0;
		} else if (!strcmp(k, "type")) {
			if (!strcmp(v, "espat"))
				cfg.type = MODEM_ESPAT;
			else if (!strcmp(v, "sim800"))
				cfg.type = MODEM_SIM800;
		} else if (!strcmp(k, "wifi_ssid")) {
			strncpy(cfg.wifi_ssid, v, sizeof(cfg.wifi_ssid) - 1);
			cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = 0;
		} else if (!strcmp(k, "wifi_pass")) {
			strncpy(cfg.wifi_pass, v, sizeof(cfg.wifi_pass) - 1);
			cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = 0;
		} else if (!strcmp(k, "apn")) {
			strncpy(cfg.apn, v, sizeof(cfg.apn) - 1);
			cfg.apn[sizeof(cfg.apn) - 1] = 0;
		} else if (!strcmp(k, "user")) {
			strncpy(cfg.user, v, sizeof(cfg.user) - 1);
			cfg.user[sizeof(cfg.user) - 1] = 0;
		} else if (!strcmp(k, "pass")) {
			strncpy(cfg.pass, v, sizeof(cfg.pass) - 1);
			cfg.pass[sizeof(cfg.pass) - 1] = 0;
		} else if (!strcmp(k, "debug")) {
			cfg.debug = (uint8_t)parse_bool(v);
		}
	}
	fclose(f);

	if (!found)
		return -1;
	if (!cfg.device[0])
		return -1;
	if (cfg.type == MODEM_ESPAT) {
		if (!cfg.wifi_ssid[0])
			return -1;
	} else if (cfg.type == MODEM_SIM800) {
		if (!cfg.apn[0])
			return -1;
	} else {
		return -1;
	}

	return 0;
}

static int tty_make_raw(int tfd)
{
	struct termios t;
	if (tcgetattr(tfd, &t) < 0)
		return -1;
	t.c_iflag = IGNBRK;
	t.c_oflag = 0;
	t.c_cflag &= ~(CSIZE | PARENB | HUPCL);
	t.c_cflag |= CREAD | CS8;
	t.c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK);
	memset(t.c_cc, 0, NCCS);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 3; /* 0.3 seconds */
	if (tcsetattr(tfd, 0, &t) < 0)
		return -1;
	return 0;
}

static int tty_set_speed(int tfd, speed_t speed)
{
	struct termios t;
	if (tcgetattr(tfd, &t) < 0)
		return -1;
	if (cfsetispeed(&t, speed) < 0 || cfsetospeed(&t, speed) < 0)
		return -1;
	if (tcsetattr(tfd, 0, &t) < 0)
		return -1;
	return 0;
}

static int at_write(const char *s)
{
	char buf[192];
	size_t n = strlen(s);
	if (n + 2 >= sizeof(buf))
		return -1;
	memcpy(buf, s, n);
	buf[n++] = '\r';
	buf[n] = 0;
	return write_all(fd, buf, n);
}

static int at_read_line(char *out, size_t outlen, int timeout_s)
{
	time_t start, now;
	size_t used = 0;
	char c;

	if (outlen == 0)
		return -1;

	time(&start);
	while (1) {
		time(&now);
		if ((int)(now - start) >= timeout_s)
			return 0;
		if (read(fd, &c, 1) == 1) {
			if (c == '\r')
				continue;
			if (c == '\n') {
				out[used] = 0;
				return 1;
			}
			if (used + 1 < outlen)
				out[used++] = c;
		}
	}
}

static int at_expect_ok(int timeout_s)
{
	char line[160];
	while (1) {
		int r = at_read_line(line, sizeof(line), timeout_s);
		if (r <= 0)
			return -1;
		if (!*line)
			continue;
		logf("AT< %s\n", line);
		if (!strcmp(line, "OK"))
			return 0;
		if (!strcmp(line, "ERROR"))
			return -1;
	}
}

static int at_cmd_ok(const char *cmd, int timeout_s)
{
	logf("AT> %s\n", cmd);
	if (at_write(cmd) < 0)
		return -1;
	return at_expect_ok(timeout_s);
}

static int at_cmd_expect_connect(const char *cmd, int timeout_s)
{
	char line[160];
	logf("AT> %s\n", cmd);
	if (at_write(cmd) < 0)
		return -1;
	while (1) {
		int r = at_read_line(line, sizeof(line), timeout_s);
		if (r <= 0)
			return -1;
		if (!*line)
			continue;
		logf("AT< %s\n", line);
		if (!strcmp(line, "CONNECT"))
			return 0;
		if (!strcmp(line, "OK"))
			return 0;
		if (!strcmp(line, "ERROR"))
			return -1;
	}
}

static int at_sync(void)
{
	static const speed_t speeds[] = { B115200, B57600, B38400, B19200, B9600 };
	unsigned int i;

	for (i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
		if (tty_set_speed(fd, speeds[i]) < 0)
			continue;
		(void)tcflush(fd, TCIOFLUSH);
		if (at_write("AT") < 0)
			continue;
		if (at_expect_ok(1) == 0) {
			logf("AT sync OK at speed index %u\n", i);
			return 0;
		}
	}
	return -1;
}

/* --- PPP FCS16 (RFC 1662) --- */

static const uint16_t fcstab[256] = {
	0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
	0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
	0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
	0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
	0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
	0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
	0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
	0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
	0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
	0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
	0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
	0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
	0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
	0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
	0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
	0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
	0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
	0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
	0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
	0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
	0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
	0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
	0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
	0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
	0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
	0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
	0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
	0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
	0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
	0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
	0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
	0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

static uint16_t ppp_fcs16(const uint8_t *cp, size_t len)
{
	uint16_t fcs = 0xFFFF;
	while (len--)
		fcs = (fcs >> 8) ^ fcstab[(fcs ^ *cp++) & 0xFF];
	return fcs ^ 0xFFFF;
}

static int ppp_put_escaped(uint8_t c)
{
	uint8_t b[2];
	if (c == PPP_FLAG || c == PPP_ESC || c < 0x20) {
		b[0] = PPP_ESC;
		b[1] = c ^ PPP_ESC_XOR;
		return write_all(fd, b, 2);
	}
	return write_all(fd, &c, 1);
}

static int ppp_send_frame(uint16_t proto, const uint8_t *payload, size_t payload_len)
{
	uint16_t fcs;
	size_t i;
	uint8_t h[4];
	uint8_t flag = PPP_FLAG;

	h[0] = 0xFF;
	h[1] = 0x03;
	h[2] = (uint8_t)(proto >> 8);
	h[3] = (uint8_t)(proto & 0xFF);

	fcs = 0xFFFF;
	for (i = 0; i < sizeof(h); i++)
		fcs = (fcs >> 8) ^ fcstab[(fcs ^ h[i]) & 0xFF];
	for (i = 0; i < payload_len; i++)
		fcs = (fcs >> 8) ^ fcstab[(fcs ^ payload[i]) & 0xFF];
	fcs ^= 0xFFFF;

	if (write_all(fd, &flag, 1) < 0)
		return -1;
	for (i = 0; i < sizeof(h); i++) {
		if (ppp_put_escaped(h[i]) < 0)
			return -1;
	}
	for (i = 0; i < payload_len; i++) {
		if (ppp_put_escaped(payload[i]) < 0)
			return -1;
	}
	if (ppp_put_escaped((uint8_t)(fcs & 0xFF)) < 0)
		return -1;
	if (ppp_put_escaped((uint8_t)(fcs >> 8)) < 0)
		return -1;
	if (write_all(fd, &flag, 1) < 0)
		return -1;
	return 0;
}

static uint8_t rx_frame[2048];
static size_t rx_flen;

static int ppp_poll_frame(void)
{
	static uint8_t scratch[256];
	static uint8_t *sp = scratch;
	static int sl = 0;
	static uint8_t esc = 0;
	static uint8_t inframe = 0;
	int n;
	uint8_t c;

	if (scratch != sp && sl)
		memmove(scratch, sp, sl);
	sp = scratch;

	if (sl < 192) {
		n = read(fd, scratch + sl, (int)(sizeof(scratch) - (size_t)sl));
		if (n == -1) {
			if (errno != EAGAIN && errno != EINTR)
				return -1;
			n = 0;
		}
		sl += n;
		if (sl == 0)
			return 0;
	}

	while (sl--) {
		c = *sp++;
		if (c == PPP_FLAG) {
			if (inframe && rx_flen >= 6) {
				uint16_t got, calc;
				got = (uint16_t)rx_frame[rx_flen - 2] |
				      ((uint16_t)rx_frame[rx_flen - 1] << 8);
				calc = ppp_fcs16(rx_frame, rx_flen - 2);
				if (calc == got)
					return 1;
				logf("ppp: bad fcs\n");
			}
			rx_flen = 0;
			esc = 0;
			inframe = 1;
			continue;
		}
		if (!inframe)
			continue;
		if (c == PPP_ESC) {
			esc = 1;
			continue;
		}
		if (esc) {
			c ^= PPP_ESC_XOR;
			esc = 0;
		}
		if (rx_flen < sizeof(rx_frame))
			rx_frame[rx_flen++] = c;
		else {
			logf("ppp: overlong frame\n");
			rx_flen = 0;
			inframe = 0;
			esc = 0;
		}
	}
	sl = 0;
	return 0;
}

static int ppp_parse(uint16_t *proto, const uint8_t **payload, size_t *payload_len)
{
	size_t off = 0;
	size_t plen;

	if (rx_flen < 6)
		return -1;

	if (rx_frame[0] == 0xFF && rx_frame[1] == 0x03)
		off = 2;

	if (off >= rx_flen - 2)
		return -1;

	if (rx_frame[off] & 1) {
		*proto = rx_frame[off];
		off += 1;
	} else {
		if (off + 1 >= rx_flen - 2)
			return -1;
		*proto = ((uint16_t)rx_frame[off] << 8) | rx_frame[off + 1];
		off += 2;
	}
	plen = (rx_flen - 2) - off;
	*payload = rx_frame + off;
	*payload_len = plen;
	return 0;
}

static uint16_t be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xFF);
}

static int lcp_send_simple(uint8_t code, uint8_t id)
{
	uint8_t p[4];
	p[0] = code;
	p[1] = id;
	put_be16(p + 2, 4);
	return ppp_send_frame(PPP_PROTO_LCP, p, sizeof(p));
}

static int lcp_send_echo(uint8_t id)
{
	uint8_t p[8];
	p[0] = LCP_ECHO_REQ;
	p[1] = id;
	put_be16(p + 2, 8);
	memcpy(p + 4, &st.magic, 4);
	return ppp_send_frame(PPP_PROTO_LCP, p, sizeof(p));
}

static int lcp_send_confreq(void)
{
	uint8_t p[4 + 4 + 6];
	size_t n = 0;

	st.lcp_id++;
	p[n++] = LCP_CONF_REQ;
	p[n++] = st.lcp_id;
	/* len filled later */
	n += 2;

	/* MRU */
	p[n++] = LCP_OPT_MRU;
	p[n++] = 4;
	put_be16(p + n, st.our_mru);
	n += 2;

	/* Magic */
	p[n++] = LCP_OPT_MAGIC;
	p[n++] = 6;
	memcpy(p + n, &st.magic, 4);
	n += 4;

	put_be16(p + 2, (uint16_t)n);
	return ppp_send_frame(PPP_PROTO_LCP, p, n);
}

static int ipcp_send_confreq(void)
{
	uint8_t p[4 + 6 + 6 + 6];
	size_t n = 0;

	st.ipcp_id++;
	p[n++] = IPCP_CONF_REQ;
	p[n++] = st.ipcp_id;
	n += 2;

	/* Our IP address (0.0.0.0 to request) */
	p[n++] = IPCP_OPT_IPADDR;
	p[n++] = 6;
	memcpy(p + n, &st.our_ip, 4);
	n += 4;

	if (st.ipcp_send_dns) {
		/* Ask for DNS if unknown */
		p[n++] = IPCP_OPT_DNS1;
		p[n++] = 6;
		memcpy(p + n, &st.dns1, 4);
		n += 4;

		p[n++] = IPCP_OPT_DNS2;
		p[n++] = 6;
		memcpy(p + n, &st.dns2, 4);
		n += 4;
	}

	put_be16(p + 2, (uint16_t)n);
	return ppp_send_frame(PPP_PROTO_IPCP, p, n);
}

static int pap_send_authreq(void)
{
	uint8_t p[4 + 1 + 64 + 1 + 64];
	size_t n = 0;
	size_t ulen = strlen(cfg.user);
	size_t plen = strlen(cfg.pass);

	if (!ulen || !plen)
		return -1;
	if (ulen > 64 || plen > 64)
		return -1;

	st.pap_id++;
	p[n++] = PAP_AUTH_REQ;
	p[n++] = st.pap_id;
	n += 2;

	p[n++] = (uint8_t)ulen;
	memcpy(p + n, cfg.user, ulen);
	n += ulen;
	p[n++] = (uint8_t)plen;
	memcpy(p + n, cfg.pass, plen);
	n += plen;

	put_be16(p + 2, (uint16_t)n);
	return ppp_send_frame(PPP_PROTO_PAP, p, n);
}

static int lcp_handle(const uint8_t *p, size_t len)
{
	uint8_t code, id;
	uint16_t plen;

	if (len < 4)
		return 0;
	code = p[0];
	id = p[1];
	plen = be16(p + 2);
	if (plen > len || plen < 4)
		return 0;

	switch (code) {
	case LCP_CONF_REQ: {
		uint8_t rej[128];
		size_t rn = 0;
		size_t i = 4;
		while (i + 2 <= plen) {
			uint8_t opt = p[i];
			uint8_t olen = p[i + 1];
			if (olen < 2 || i + olen > plen)
				break;
			if (opt == LCP_OPT_MRU && olen == 4) {
				st.peer_mru = be16(p + i + 2);
			} else if (opt == LCP_OPT_AUTH && olen == 4) {
				st.auth_proto = be16(p + i + 2);
				if (st.auth_proto != PPP_PROTO_PAP) {
					if (rn + olen < sizeof(rej)) {
						memcpy(rej + rn, p + i, olen);
						rn += olen;
					}
				}
			} else if (opt != LCP_OPT_ACCM && opt != LCP_OPT_MAGIC) {
				if (rn + olen < sizeof(rej)) {
					memcpy(rej + rn, p + i, olen);
					rn += olen;
				}
			}
			i += olen;
		}
		if (rn) {
			uint8_t out[4 + 128];
			out[0] = LCP_CONF_REJ;
			out[1] = id;
			put_be16(out + 2, (uint16_t)(4 + rn));
			memcpy(out + 4, rej, rn);
			(void)ppp_send_frame(PPP_PROTO_LCP, out, 4 + rn);
		} else {
			uint8_t out[256];
			if (plen > sizeof(out))
				return 1;
			memcpy(out, p, plen);
			out[0] = LCP_CONF_ACK;
			(void)ppp_send_frame(PPP_PROTO_LCP, out, plen);
			st.lcp_them_up = 1;
		}
		return 1;
	}
	case LCP_CONF_ACK:
		if (id == st.lcp_id)
			st.lcp_us_up = 1;
		return 1;
	case LCP_CONF_NAK:
		/* Peer may suggest different MRU */
		(void)lcp_send_confreq();
		return 1;
	case LCP_TERM_REQ:
		(void)lcp_send_simple(LCP_TERM_ACK, id);
		return -1;
	case LCP_ECHO_REQ: {
		uint8_t out[8];
		if (plen < 8)
			return 1;
		out[0] = LCP_ECHO_REP;
		out[1] = id;
		put_be16(out + 2, 8);
		memcpy(out + 4, p + 4, 4);
		(void)ppp_send_frame(PPP_PROTO_LCP, out, sizeof(out));
		return 1;
	}
	case LCP_ECHO_REP:
		st.echo_failures = 0;
		return 1;
	default:
		return 0;
	}
}

static int ipcp_handle(const uint8_t *p, size_t len)
{
	uint8_t code, id;
	uint16_t plen;

	if (len < 4)
		return 0;
	code = p[0];
	id = p[1];
	plen = be16(p + 2);
	if (plen > len || plen < 4)
		return 0;

	switch (code) {
	case IPCP_CONF_REQ: {
		uint8_t rej[128];
		size_t rn = 0;
		size_t i = 4;
		while (i + 2 <= plen) {
			uint8_t opt = p[i];
			uint8_t olen = p[i + 1];
			if (olen < 2 || i + olen > plen)
				break;
			if (opt == IPCP_OPT_IPADDR && olen == 6) {
				memcpy(&st.peer_ip, p + i + 2, 4);
			} else if (opt != IPCP_OPT_DNS1 && opt != IPCP_OPT_DNS2) {
				if (rn + olen < sizeof(rej)) {
					memcpy(rej + rn, p + i, olen);
					rn += olen;
				}
			}
			i += olen;
		}
		if (rn) {
			uint8_t out[4 + 128];
			out[0] = IPCP_CONF_REJ;
			out[1] = id;
			put_be16(out + 2, (uint16_t)(4 + rn));
			memcpy(out + 4, rej, rn);
			(void)ppp_send_frame(PPP_PROTO_IPCP, out, 4 + rn);
		} else {
			uint8_t out[256];
			if (plen > sizeof(out))
				return 1;
			memcpy(out, p, plen);
			out[0] = IPCP_CONF_ACK;
			(void)ppp_send_frame(PPP_PROTO_IPCP, out, plen);
			st.ipcp_them_up = 1;
		}
		return 1;
	}
	case IPCP_CONF_ACK:
		if (id == st.ipcp_id)
			st.ipcp_us_up = 1;
		return 1;
	case IPCP_CONF_NAK: {
		size_t i = 4;
		while (i + 2 <= plen) {
			uint8_t opt = p[i];
			uint8_t olen = p[i + 1];
			if (olen < 2 || i + olen > plen)
				break;
			if (opt == IPCP_OPT_IPADDR && olen == 6) {
				memcpy(&st.our_ip, p + i + 2, 4);
			} else if (opt == IPCP_OPT_DNS1 && olen == 6) {
				memcpy(&st.dns1, p + i + 2, 4);
			} else if (opt == IPCP_OPT_DNS2 && olen == 6) {
				memcpy(&st.dns2, p + i + 2, 4);
			}
			i += olen;
		}
		(void)ipcp_send_confreq();
		return 1;
	}
	case IPCP_CONF_REJ: {
		size_t i = 4;
		while (i + 2 <= plen) {
			uint8_t opt = p[i];
			uint8_t olen = p[i + 1];
			if (olen < 2 || i + olen > plen)
				break;
			if (opt == IPCP_OPT_DNS1 || opt == IPCP_OPT_DNS2) {
				st.ipcp_send_dns = 0;
			} else if (opt == IPCP_OPT_IPADDR) {
				return -1;
			}
			i += olen;
		}
		(void)ipcp_send_confreq();
		return 1;
	}
	default:
		return 0;
	}
}

static int pap_handle(const uint8_t *p, size_t len)
{
	uint8_t code;
	if (len < 4)
		return 0;
	code = p[0];
	if (code == PAP_AUTH_ACK) {
		st.pap_done = 1;
		return 1;
	}
	if (code == PAP_AUTH_NAK)
		return -1;
	return 0;
}

static uint32_t ip4_u32(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
	uint32_t a = 0;
	uint8_t *p = (uint8_t *)&a;
	p[0] = b0;
	p[1] = b1;
	p[2] = b2;
	p[3] = b3;
	return a;
}

static void apply_network_config(void)
{
	uip_ipaddr_t ipaddr;
	uint8_t *p;
	uint32_t mask;

	/* uIP config */
	p = (uint8_t *)&st.our_ip;
	uip_ipaddr(&ipaddr, p[0], p[1], p[2], p[3]);
	uip_sethostaddr(&ipaddr);

	p = (uint8_t *)&st.peer_ip;
	uip_ipaddr(&ipaddr, p[0], p[1], p[2], p[3]);
	uip_setdraddr(&ipaddr);

	mask = ip4_u32(255, 255, 255, 255);
	p = (uint8_t *)&mask;
	uip_ipaddr(&ipaddr, p[0], p[1], p[2], p[3]);
	uip_setnetmask(&ipaddr);

	/* Kernel view (for ifconfig) */
	(void)ioctl(knet, NET_IPADDR, &st.our_ip);
	(void)ioctl(knet, NET_GATEWAY, &st.peer_ip);
	(void)ioctl(knet, NET_MASK, &mask);
	(void)ioctl(knet, NET_MTU, &mtu);
}

static void write_resolv_conf(void)
{
	int rfd;
	char buf[80];
	uint8_t *p;

	rfd = open("/etc/resolv.conf", O_WRONLY | O_TRUNC | O_CREAT, 0644);
	if (rfd < 0)
		return;

	if (st.dns1) {
		p = (uint8_t *)&st.dns1;
		snprintf(buf, sizeof(buf), "nameserver %u.%u.%u.%u\n", p[0], p[1], p[2], p[3]);
		(void)write_all(rfd, buf, strlen(buf));
	}
	if (st.dns2) {
		p = (uint8_t *)&st.dns2;
		snprintf(buf, sizeof(buf), "nameserver %u.%u.%u.%u\n", p[0], p[1], p[2], p[3]);
		(void)write_all(rfd, buf, strlen(buf));
	}
	close(rfd);
}

static int ppp_negotiate(void)
{
	time_t start, now;

	memset(&st, 0, sizeof(st));
	st.our_mru = mtu;
	st.peer_mru = 1500;
	st.ipcp_send_dns = 1;
	time(&st.last_rx);
	st.magic = (uint32_t)st.last_rx ^ 0x5a5aa5a5UL;

	if (lcp_send_confreq() < 0)
		return -1;

	time(&start);
	while (!(st.lcp_us_up && st.lcp_them_up)) {
		uint16_t proto;
		const uint8_t *payload;
		size_t plen;
		int r = ppp_poll_frame();
		if (r < 0)
			return -1;
		if (r == 0) {
			time(&now);
			if ((int)(now - start) > 20)
				return -1;
			continue;
		}
		time(&st.last_rx);
		if (ppp_parse(&proto, &payload, &plen) == 0) {
			if (proto == PPP_PROTO_LCP) {
				if (lcp_handle(payload, plen) < 0)
					return -1;
			}
		}
	}

	/* PAP if requested */
	if (st.auth_proto == PPP_PROTO_PAP) {
		if (pap_send_authreq() < 0)
			return -1;
		time(&start);
		while (!st.pap_done) {
			uint16_t proto;
			const uint8_t *payload;
			size_t plen;
			int r = ppp_poll_frame();
			if (r < 0)
				return -1;
			if (r == 0) {
				time(&now);
				if ((int)(now - start) > 20)
					return -1;
				continue;
			}
			time(&st.last_rx);
			if (ppp_parse(&proto, &payload, &plen) == 0) {
				if (proto == PPP_PROTO_PAP) {
					if (pap_handle(payload, plen) < 0)
						return -1;
				} else if (proto == PPP_PROTO_LCP) {
					if (lcp_handle(payload, plen) < 0)
						return -1;
				}
			}
		}
	}

	/* IPCP */
	if (ipcp_send_confreq() < 0)
		return -1;
	time(&start);
	while (!(st.ipcp_us_up && st.ipcp_them_up)) {
		uint16_t proto;
		const uint8_t *payload;
		size_t plen;
		int r = ppp_poll_frame();
		if (r < 0)
			return -1;
		if (r == 0) {
			time(&now);
			if ((int)(now - start) > 20)
				return -1;
			continue;
		}
		time(&st.last_rx);
		if (ppp_parse(&proto, &payload, &plen) == 0) {
			if (proto == PPP_PROTO_IPCP) {
				if (ipcp_handle(payload, plen) < 0)
					return -1;
			} else if (proto == PPP_PROTO_LCP) {
				if (lcp_handle(payload, plen) < 0)
					return -1;
			}
		}
	}

	if (!st.our_ip || !st.peer_ip)
		return -1;

	apply_network_config();
	write_resolv_conf();
	return 0;
}

static int modem_espat_init(void)
{
	char cmd[160];
	if (at_sync() < 0)
		return -1;
	if (at_cmd_ok("ATE0", 2) < 0)
		return -1;
	if (at_cmd_ok("AT+CWMODE=1", 2) < 0)
		return -1;
	if (!cfg.wifi_ssid[0])
		return -1;
	snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", cfg.wifi_ssid, cfg.wifi_pass);
	if (at_cmd_ok(cmd, 30) < 0)
		return -1;
	if (at_cmd_expect_connect("AT+PPP=1", 10) < 0)
		return -1;
	return 0;
}

static int modem_sim800_init(void)
{
	char cmd[160];
	if (at_sync() < 0)
		return -1;
	if (at_cmd_ok("ATE0", 2) < 0)
		return -1;
	if (at_cmd_ok("AT+CPIN?", 5) < 0)
		return -1;
	if (at_cmd_ok("AT+CGATT=1", 20) < 0)
		return -1;
	snprintf(cmd, sizeof(cmd), "AT+CSTT=\"%s\",\"%s\",\"%s\"", cfg.apn, cfg.user, cfg.pass);
	if (at_cmd_ok(cmd, 10) < 0)
		return -1;
	if (at_cmd_ok("AT+CIICR", 60) < 0)
		return -1;
	if (at_cmd_expect_connect("ATD*99#", 30) < 0)
		return -1;
	return 0;
}

/* initialize network device */
int device_init(void)
{
	const char *section = "ppp0";

	if (netd_argc > 1 && netd_argv && netd_argv[1] && netd_argv[1][0] != '-')
		section = netd_argv[1];

	if (load_ppp_conf(section) < 0)
		return -1;

	fd = open(cfg.device, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		log_perror(cfg.device);
		return -1;
	}
	if (tty_make_raw(fd) < 0) {
		log_perror("tty_make_raw");
		return -1;
	}

	if (cfg.type == MODEM_ESPAT) {
		if (modem_espat_init() < 0)
			return -1;
	} else if (cfg.type == MODEM_SIM800) {
		if (modem_sim800_init() < 0)
			return -1;
	} else {
		return -1;
	}

	if (ppp_negotiate() < 0)
		return -1;

	return 0;
}

int device_send(uint8_t *sbuf, int len)
{
	if (len <= 0)
		return 0;
	/* Skip fake ethernet header */
	sbuf += 14;
	if (len > (int)st.peer_mru)
		len = st.peer_mru;
	if (ppp_send_frame(PPP_PROTO_IP, sbuf, (size_t)len) < 0)
		return -1;
	return 0;
}

int device_read(uint8_t *buf, int len)
{
	time_t now;

	/* keepalive */
	time(&now);
	if (st.lcp_us_up && st.lcp_them_up) {
		if ((int)(now - st.last_rx) > 10 && (int)(now - st.last_echo) > 10) {
			st.last_echo = now;
			st.echo_failures++;
			if (st.echo_failures > 3)
				exit(1);
			(void)lcp_send_echo(++st.lcp_id);
		}
	}

	while (1) {
		uint16_t proto;
		const uint8_t *payload;
		size_t plen;
		int r = ppp_poll_frame();
		if (r < 0)
			exit(1);
		if (r == 0)
			return -1;
		time(&st.last_rx);
		if (ppp_parse(&proto, &payload, &plen) < 0)
			continue;

		if (proto == PPP_PROTO_IP) {
			if ((int)plen + 14 > len)
				return -1;
			/* Mark it as IP */
			buf[12] = 0x08;
			buf[13] = 0x00;
			memcpy(buf + 14, payload, plen);
			return (int)plen;
		}
		if (proto == PPP_PROTO_LCP) {
			if (lcp_handle(payload, plen) < 0)
				exit(1);
		} else if (proto == PPP_PROTO_IPCP) {
			if (ipcp_handle(payload, plen) < 0)
				exit(1);
		}
	}
}
