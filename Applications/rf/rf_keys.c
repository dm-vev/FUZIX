#include "rf_keys.h"

#include "rf_utf8.h"

static int parse_escape_key(const uint8_t *b, size_t len, size_t *consumed, struct rf_key *out)
{
	if (len < 2) {
		*consumed = 1;
		*out = (struct rf_key){.kind = RF_KEY_ESC};
		return 1;
	}

	/* SS3 sequences: ESC O P/Q/R for F1/F2/F3. */
	if (b[1] == 'O') {
		if (len < 3)
			return 0;
		switch (b[2]) {
		case 'P':
			*consumed = 3;
			*out = (struct rf_key){.kind = RF_KEY_F1};
			return 1;
		case 'Q':
			*consumed = 3;
			*out = (struct rf_key){.kind = RF_KEY_F2};
			return 1;
		case 'R':
			*consumed = 3;
			*out = (struct rf_key){.kind = RF_KEY_F3};
			return 1;
		default:
			*consumed = 1;
			*out = (struct rf_key){.kind = RF_KEY_ESC};
			return 1;
		}
	}

	if (b[1] != '[') {
		*consumed = 1;
		*out = (struct rf_key){.kind = RF_KEY_ESC};
		return 1;
	}
	if (len < 3)
		return 0;

	switch (b[2]) {
	case 'A':
		*consumed = 3;
		*out = (struct rf_key){.kind = RF_KEY_UP};
		return 1;
	case 'B':
		*consumed = 3;
		*out = (struct rf_key){.kind = RF_KEY_DOWN};
		return 1;
	case 'C':
		*consumed = 3;
		*out = (struct rf_key){.kind = RF_KEY_RIGHT};
		return 1;
	case 'D':
		*consumed = 3;
		*out = (struct rf_key){.kind = RF_KEY_LEFT};
		return 1;
	case 'H':
		*consumed = 3;
		*out = (struct rf_key){.kind = RF_KEY_HOME};
		return 1;
	case 'F':
		*consumed = 3;
		*out = (struct rf_key){.kind = RF_KEY_END};
		return 1;
	default:
		break;
	}

	if (b[2] < '0' || b[2] > '9') {
		*consumed = 1;
		*out = (struct rf_key){.kind = RF_KEY_ESC};
		return 1;
	}

	int n = 0;
	size_t i = 2;
	while (i < len && b[i] >= '0' && b[i] <= '9') {
		n = n * 10 + (int)(b[i] - '0');
		i++;
	}
	if (i >= len)
		return 0;
	if (b[i] != '~') {
		*consumed = 1;
		*out = (struct rf_key){.kind = RF_KEY_ESC};
		return 1;
	}
	*consumed = i + 1;
	switch (n) {
	case 3:
		out->kind = RF_KEY_DELETE;
		return 1;
	case 5:
		out->kind = RF_KEY_PAGE_UP;
		return 1;
	case 6:
		out->kind = RF_KEY_PAGE_DOWN;
		return 1;
	case 11:
		out->kind = RF_KEY_F1;
		return 1;
	case 12:
		out->kind = RF_KEY_F2;
		return 1;
	case 13:
		out->kind = RF_KEY_F3;
		return 1;
	default:
		*consumed = 1;
		*out = (struct rf_key){.kind = RF_KEY_ESC};
		return 1;
	}
}

int rf_next_key(const uint8_t *b, size_t len, size_t *consumed, struct rf_key *out)
{
	if (!consumed || !out)
		return 0;
	*consumed = 0;
	*out = (struct rf_key){0};

	if (!b || len == 0)
		return 0;

	if (b[0] == 0x1b)
		return parse_escape_key(b, len, consumed, out);

	switch (b[0]) {
	case '\r':
	case '\n':
		*consumed = 1;
		out->kind = RF_KEY_ENTER;
		return 1;
	case 0x7f:
	case 0x08:
		*consumed = 1;
		out->kind = RF_KEY_BACKSPACE;
		return 1;
	case '\t':
		*consumed = 1;
		out->kind = RF_KEY_TAB;
		return 1;
	default:
		break;
	}

	if (b[0] < 0x20) {
		*consumed = 1;
		out->kind = RF_KEY_CTRL;
		out->ctrl = b[0];
		return 1;
	}

	uint32_t r = 0;
	size_t rsz = rf_utf8_decode(b, len, &r);
	if (rsz == 0)
		return 0;
	*consumed = rsz;
	out->kind = RF_KEY_RUNE;
	out->r = r;
	return 1;
}

