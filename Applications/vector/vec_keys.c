#include "vec_keys.h"

static int parse_escape(const uint8_t *b, size_t n, size_t *consumed, vec_key *out)
{
	if (n < 2) {
		*consumed = 1;
		out->kind = VEC_KEY_ESC;
		return 1;
	}

	/* SS3 sequences: ESC O P/Q/R */
	if (b[1] == 'O') {
		if (n < 3)
			return 0;
		*consumed = 3;
		switch (b[2]) {
		case 'P': out->kind = VEC_KEY_F1; return 1;
		case 'Q': out->kind = VEC_KEY_F2; return 1;
		case 'R': out->kind = VEC_KEY_F3; return 1;
		default:
			out->kind = VEC_KEY_ESC;
			*consumed = 1;
			return 1;
		}
	}

	if (b[1] != '[') {
		*consumed = 1;
		out->kind = VEC_KEY_ESC;
		return 1;
	}
	if (n < 3)
		return 0;

	*consumed = 3;
	switch (b[2]) {
	case 'A': out->kind = VEC_KEY_UP; return 1;
	case 'B': out->kind = VEC_KEY_DOWN; return 1;
	case 'C': out->kind = VEC_KEY_RIGHT; return 1;
	case 'D': out->kind = VEC_KEY_LEFT; return 1;
	case 'H': out->kind = VEC_KEY_HOME; return 1;
	case 'F': out->kind = VEC_KEY_END; return 1;
	default:
		break;
	}

	if (b[2] < '0' || b[2] > '9') {
		*consumed = 1;
		out->kind = VEC_KEY_ESC;
		return 1;
	}

	/* ESC [ <n> ~ */
	int val = 0;
	size_t i = 2;
	while (i < n && b[i] >= '0' && b[i] <= '9') {
		val = val * 10 + (int)(b[i] - '0');
		i++;
	}
	if (i >= n)
		return 0;
	if (b[i] != '~') {
		*consumed = 1;
		out->kind = VEC_KEY_ESC;
		return 1;
	}
	*consumed = i + 1;
	switch (val) {
	case 3: out->kind = VEC_KEY_DELETE; return 1;
	case 5: out->kind = VEC_KEY_PGUP; return 1;
	case 6: out->kind = VEC_KEY_PGDN; return 1;
	case 11: out->kind = VEC_KEY_F1; return 1;
	case 12: out->kind = VEC_KEY_F2; return 1;
	case 13: out->kind = VEC_KEY_F3; return 1;
	default:
		out->kind = VEC_KEY_ESC;
		*consumed = 1;
		return 1;
	}
}

int vec_next_key(const uint8_t *b, size_t n, size_t *consumed, vec_key *out)
{
	if (!b || !n || !consumed || !out)
		return 0;

	out->kind = VEC_KEY_RUNE;
	out->r = 0;
	out->ctrl = 0;

	if (b[0] == 0x1b)
		return parse_escape(b, n, consumed, out);

	switch (b[0]) {
	case '\r':
	case '\n':
		*consumed = 1;
		out->kind = VEC_KEY_ENTER;
		return 1;
	case 0x7f:
	case 0x08:
		*consumed = 1;
		out->kind = VEC_KEY_BACKSPACE;
		return 1;
	case '\t':
		*consumed = 1;
		out->kind = VEC_KEY_TAB;
		return 1;
	default:
		break;
	}

	if (b[0] < 0x20) {
		*consumed = 1;
		out->kind = VEC_KEY_CTRL;
		out->ctrl = b[0];
		return 1;
	}

	*consumed = 1;
	out->kind = VEC_KEY_RUNE;
	out->r = b[0];
	return 1;
}

