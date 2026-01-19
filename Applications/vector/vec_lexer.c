#include "vec_lexer.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int is_ident_start(int c)
{
	return c == '_' || isalpha((unsigned char)c);
}

static int is_ident_continue(int c)
{
	return is_ident_start(c) || isdigit((unsigned char)c);
}

static size_t scan_number(const char *s, size_t i)
{
	size_t start = i;
	if (s[i] == '.')
		i++;
	while (s[i] && isdigit((unsigned char)s[i]))
		i++;
	if (s[i] == '.') {
		i++;
		while (s[i] && isdigit((unsigned char)s[i]))
			i++;
	}
	if (s[i] == 'e' || s[i] == 'E') {
		size_t j = i + 1;
		if (s[j] == '+' || s[j] == '-')
			j++;
		size_t k = j;
		while (s[k] && isdigit((unsigned char)s[k]))
			k++;
		if (k > j)
			i = k;
	}
	if (i == start)
		return start;
	return i;
}

static int parse_number_token(const char *s, size_t len, vec_number *out)
{
	int is_float = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '.' || s[i] == 'e' || s[i] == 'E')
			is_float = 1;
	}

	if (!is_float) {
		int64_t v = 0;
		size_t i = 0;
		if (len && s[0] == '+')
			i = 1;
		if (i < len && s[i] == '-')
			return 0; /* sign handled by parser */
		for (; i < len; i++) {
			unsigned char c = (unsigned char)s[i];
			if (!isdigit(c))
				return 0;
			int d = c - '0';
			if (v > (INT64_MAX - d) / 10) {
				is_float = 1;
				break;
			}
			v = v * 10 + d;
		}
		if (!is_float) {
			*out = vec_rat_number(vec_rat_int(v));
			return 1;
		}
	}

	if (len < 64) {
		char tmp[64];
		memcpy(tmp, s, len);
		tmp[len] = 0;
		char *endp = NULL;
		errno = 0;
		double f = strtod(tmp, &endp);
		if (errno == 0 && endp && *endp == 0) {
			*out = vec_float(f);
			return 1;
		}
	}

	return 0;
}

void vec_lexer_init(vec_lexer *l, const char *s)
{
	if (!l)
		return;
	l->s = s ? s : "";
	l->i = 0;
}

vec_token vec_lexer_next(vec_lexer *l)
{
	vec_token t;
	memset(&t, 0, sizeof(t));
	t.kind = VEC_TOK_EOF;

	if (!l || !l->s)
		return t;

	while (l->s[l->i]) {
		unsigned char c = (unsigned char)l->s[l->i];
		if (c == '\n' || c == ';') {
			t.kind = VEC_TOK_SEMI;
			t.start = l->s + l->i;
			t.len = 1;
			l->i++;
			return t;
		}
		if (!isspace(c))
			break;
		l->i++;
	}

	if (!l->s[l->i])
		return t;

	t.start = l->s + l->i;
	t.len = 1;

	switch (l->s[l->i]) {
	case ';':
		t.kind = VEC_TOK_SEMI;
		l->i++;
		return t;
	case '+':
		t.kind = VEC_TOK_PLUS;
		l->i++;
		return t;
	case '-':
		t.kind = VEC_TOK_MINUS;
		l->i++;
		return t;
	case '*':
		t.kind = VEC_TOK_STAR;
		l->i++;
		return t;
	case '/':
		t.kind = VEC_TOK_SLASH;
		l->i++;
		return t;
	case '^':
		t.kind = VEC_TOK_CARET;
		l->i++;
		return t;
	case '(':
	case '[':
		t.kind = VEC_TOK_LPAREN;
		l->i++;
		return t;
	case ')':
	case ']':
		t.kind = VEC_TOK_RPAREN;
		l->i++;
		return t;
	case ',':
		t.kind = VEC_TOK_COMMA;
		l->i++;
		return t;
	case '=':
		if (l->s[l->i + 1] == '=') {
			t.kind = VEC_TOK_EQ;
			t.len = 2;
			l->i += 2;
			return t;
		}
		t.kind = VEC_TOK_ASSIGN;
		l->i++;
		return t;
	case '!':
		if (l->s[l->i + 1] == '=') {
			t.kind = VEC_TOK_NE;
			t.len = 2;
			l->i += 2;
			return t;
		}
		break;
	case '<':
		if (l->s[l->i + 1]) {
			if (l->s[l->i + 1] == '=') {
				t.kind = VEC_TOK_LE;
				t.len = 2;
				l->i += 2;
				return t;
			}
			if (l->s[l->i + 1] == '>') {
				t.kind = VEC_TOK_NE;
				t.len = 2;
				l->i += 2;
				return t;
			}
		}
		t.kind = VEC_TOK_LT;
		l->i++;
		return t;
	case '>':
		if (l->s[l->i + 1] == '=') {
			t.kind = VEC_TOK_GE;
			t.len = 2;
			l->i += 2;
			return t;
		}
		t.kind = VEC_TOK_GT;
		l->i++;
		return t;
	default:
		break;
	}

	{
		int ch = (unsigned char)l->s[l->i];
		if (is_ident_start(ch)) {
			size_t start = l->i;
			l->i++;
			while (l->s[l->i] && is_ident_continue((unsigned char)l->s[l->i]))
				l->i++;
			t.kind = VEC_TOK_IDENT;
			t.start = l->s + start;
			t.len = l->i - start;
			return t;
		}
		if (ch == '.' || isdigit(ch)) {
			size_t start = l->i;
			size_t end = scan_number(l->s, l->i);
			if (end > start) {
				t.start = l->s + start;
				t.len = end - start;
				l->i = end;
				t.kind = VEC_TOK_NUMBER;
				if (!parse_number_token(t.start, t.len, &t.num)) {
					t.kind = VEC_TOK_EOF;
				}
				return t;
			}
		}
	}

	/* Unknown character. */
	t.kind = VEC_TOK_EOF;
	t.start = l->s + l->i;
	t.len = 1;
	l->i++;
	return t;
}

const char *vec_token_text(vec_tok_kind kind)
{
	switch (kind) {
	case VEC_TOK_EOF: return "EOF";
	case VEC_TOK_NUMBER: return "number";
	case VEC_TOK_IDENT: return "ident";
	case VEC_TOK_SEMI: return ";";
	case VEC_TOK_PLUS: return "+";
	case VEC_TOK_MINUS: return "-";
	case VEC_TOK_STAR: return "*";
	case VEC_TOK_SLASH: return "/";
	case VEC_TOK_CARET: return "^";
	case VEC_TOK_LPAREN: return "(";
	case VEC_TOK_RPAREN: return ")";
	case VEC_TOK_COMMA: return ",";
	case VEC_TOK_ASSIGN: return "=";
	case VEC_TOK_EQ: return "==";
	case VEC_TOK_NE: return "!=";
	case VEC_TOK_LT: return "<";
	case VEC_TOK_LE: return "<=";
	case VEC_TOK_GT: return ">";
	case VEC_TOK_GE: return ">=";
	default: return "?";
	}
}
