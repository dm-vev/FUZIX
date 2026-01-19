#include "vec_parser.h"

#include "vec_lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	vec_lexer l;
	vec_token cur;
} vec_parser;

static char *vec_strndup2(const char *s, size_t n)
{
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	memcpy(out, s, n);
	out[n] = 0;
	return out;
}

static void parser_next(vec_parser *p) { p->cur = vec_lexer_next(&p->l); }

static int actions_push(vec_actions *a, vec_action act)
{
	if (a->count + 1 > a->cap) {
		size_t ncap = a->cap ? a->cap * 2 : 8;
		vec_action *ni = realloc(a->items, ncap * sizeof(*ni));
		if (!ni)
			return -1;
		a->items = ni;
		a->cap = ncap;
	}
	a->items[a->count++] = act;
	return 0;
}

static vec_node *parse_expr(vec_parser *p, char *err, size_t errsz);
static vec_node *parse_compare(vec_parser *p, char *err, size_t errsz);
static vec_node *parse_sum(vec_parser *p, char *err, size_t errsz);
static vec_node *parse_product(vec_parser *p, char *err, size_t errsz);
static vec_node *parse_power(vec_parser *p, char *err, size_t errsz);
static vec_node *parse_unary(vec_parser *p, char *err, size_t errsz);
static vec_node *parse_primary(vec_parser *p, char *err, size_t errsz);

static int try_parse_func_def(vec_parser *p, const char *name, size_t name_len,
			      vec_token ident_tok, size_t after_ident_pos,
			      vec_action *out, char *err, size_t errsz)
{
	(void)err;
	(void)errsz;
	if (p->cur.kind != VEC_TOK_LPAREN)
		return 0;

	/* ident ( ident ) = expr */
	parser_next(p);
	if (p->cur.kind != VEC_TOK_IDENT) {
		p->cur = ident_tok;
		p->l.i = after_ident_pos;
		return 0;
	}
	vec_token param_tok = p->cur;
	parser_next(p);
	if (p->cur.kind != VEC_TOK_RPAREN) {
		p->cur = ident_tok;
		p->l.i = after_ident_pos;
		return 0;
	}
	parser_next(p);
	if (p->cur.kind != VEC_TOK_ASSIGN) {
		p->cur = ident_tok;
		p->l.i = after_ident_pos;
		return 0;
	}
	parser_next(p);
	vec_node *ex = parse_expr(p, err, errsz);
	if (!ex)
		return -1;

	memset(out, 0, sizeof(*out));
	out->kind = VEC_ACT_ASSIGN_FUNC;
	out->expr = ex;
	out->func_name = vec_strndup2(name, name_len);
	out->func_param = vec_strndup2(param_tok.start, param_tok.len);
	if (!out->func_name || !out->func_param) {
		free(out->func_name);
		free(out->func_param);
		vec_node_destroy(ex);
		return -1;
	}
	return 1;
}

static int parse_top(vec_parser *p, vec_action *out, char *err, size_t errsz)
{
	memset(out, 0, sizeof(*out));

	if (p->cur.kind == VEC_TOK_IDENT) {
		vec_token ident_tok = p->cur;
		size_t after_ident_pos = p->l.i;
		const char *name = ident_tok.start;
		size_t name_len = ident_tok.len;
		parser_next(p);

		if (p->cur.kind == VEC_TOK_ASSIGN) {
			parser_next(p);
			vec_node *ex = parse_expr(p, err, errsz);
			if (!ex)
				return -1;
			out->kind = VEC_ACT_ASSIGN_VAR;
			out->expr = ex;
			out->var_name = vec_strndup2(name, name_len);
			if (!out->var_name) {
				vec_node_destroy(ex);
				return -1;
			}
			return 0;
		}

		if (p->cur.kind == VEC_TOK_LPAREN) {
			int r = try_parse_func_def(p, name, name_len, ident_tok, after_ident_pos, out, err, errsz);
			if (r != 0)
				return r < 0 ? -1 : 0;
		}

		/* Restore state and treat as expression. */
		p->l.i = after_ident_pos;
		p->cur = ident_tok;
	}

	vec_node *ex = parse_expr(p, err, errsz);
	if (!ex)
		return -1;
	out->kind = VEC_ACT_EVAL;
	out->expr = ex;
	return 0;
}

void vec_actions_destroy(vec_actions *a)
{
	if (!a)
		return;
	for (size_t i = 0; i < a->count; i++) {
		vec_action *x = &a->items[i];
		vec_node_destroy(x->expr);
		free(x->var_name);
		free(x->func_name);
		free(x->func_param);
	}
	free(a->items);
	a->items = NULL;
	a->count = 0;
	a->cap = 0;
}

int vec_parse_input(const char *s, vec_actions *out, char *err, size_t errsz)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	vec_parser p;
	vec_lexer_init(&p.l, s ? s : "");
	p.cur = vec_lexer_next(&p.l);

	for (;;) {
		while (p.cur.kind == VEC_TOK_SEMI)
			parser_next(&p);
		if (p.cur.kind == VEC_TOK_EOF)
			return 0;

		vec_action act;
		if (parse_top(&p, &act, err, errsz) != 0) {
			vec_actions_destroy(out);
			return -1;
		}
		if (actions_push(out, act) != 0) {
			vec_node_destroy(act.expr);
			free(act.var_name);
			free(act.func_name);
			free(act.func_param);
			vec_actions_destroy(out);
			return -1;
		}

		while (p.cur.kind == VEC_TOK_SEMI)
			parser_next(&p);
		if (p.cur.kind == VEC_TOK_EOF)
			return 0;
	}
}

static vec_node *parse_expr(vec_parser *p, char *err, size_t errsz)
{
	return parse_compare(p, err, errsz);
}

static vec_node *parse_compare(vec_parser *p, char *err, size_t errsz)
{
	vec_node *left = parse_sum(p, err, errsz);
	if (!left)
		return NULL;

	vec_cmp_op op;
	switch (p->cur.kind) {
	case VEC_TOK_EQ: op = VEC_CMP_EQ; break;
	case VEC_TOK_NE: op = VEC_CMP_NE; break;
	case VEC_TOK_LT: op = VEC_CMP_LT; break;
	case VEC_TOK_LE: op = VEC_CMP_LE; break;
	case VEC_TOK_GT: op = VEC_CMP_GT; break;
	case VEC_TOK_GE: op = VEC_CMP_GE; break;
	default:
		return left;
	}

	parser_next(p);
	vec_node *right = parse_sum(p, err, errsz);
	if (!right) {
		vec_node_destroy(left);
		return NULL;
	}
	vec_node *n = vec_node_compare_new(op, left, right);
	if (!n) {
		snprintf(err, errsz, "parse: out of memory");
		vec_node_destroy(left);
		vec_node_destroy(right);
		return NULL;
	}
	return n;
}

static vec_node *parse_sum(vec_parser *p, char *err, size_t errsz)
{
	vec_node *left = parse_product(p, err, errsz);
	if (!left)
		return NULL;
	while (p->cur.kind == VEC_TOK_PLUS || p->cur.kind == VEC_TOK_MINUS) {
		char op = (p->cur.kind == VEC_TOK_PLUS) ? '+' : '-';
		parser_next(p);
		vec_node *right = parse_product(p, err, errsz);
		if (!right) {
			vec_node_destroy(left);
			return NULL;
		}
		left = vec_node_binary_new(op, left, right);
		if (!left) {
			snprintf(err, errsz, "parse: out of memory");
			return NULL;
		}
	}
	return left;
}

static vec_node *parse_product(vec_parser *p, char *err, size_t errsz)
{
	vec_node *left = parse_power(p, err, errsz);
	if (!left)
		return NULL;
	while (p->cur.kind == VEC_TOK_STAR || p->cur.kind == VEC_TOK_SLASH) {
		char op = (p->cur.kind == VEC_TOK_STAR) ? '*' : '/';
		parser_next(p);
		vec_node *right = parse_power(p, err, errsz);
		if (!right) {
			vec_node_destroy(left);
			return NULL;
		}
		left = vec_node_binary_new(op, left, right);
		if (!left) {
			snprintf(err, errsz, "parse: out of memory");
			return NULL;
		}
	}
	return left;
}

static vec_node *parse_power(vec_parser *p, char *err, size_t errsz)
{
	vec_node *left = parse_unary(p, err, errsz);
	if (!left)
		return NULL;
	if (p->cur.kind == VEC_TOK_CARET) {
		parser_next(p);
		vec_node *right = parse_power(p, err, errsz);
		if (!right) {
			vec_node_destroy(left);
			return NULL;
		}
		left = vec_node_binary_new('^', left, right);
		if (!left) {
			snprintf(err, errsz, "parse: out of memory");
			return NULL;
		}
	}
	return left;
}

static vec_node *parse_unary(vec_parser *p, char *err, size_t errsz)
{
	if (p->cur.kind == VEC_TOK_PLUS || p->cur.kind == VEC_TOK_MINUS) {
		char op = (p->cur.kind == VEC_TOK_PLUS) ? '+' : '-';
		parser_next(p);
		vec_node *x = parse_unary(p, err, errsz);
		if (!x)
			return NULL;
		vec_node *n = vec_node_unary_new(op, x);
		if (!n) {
			snprintf(err, errsz, "parse: out of memory");
			return NULL;
		}
		return n;
	}
	return parse_primary(p, err, errsz);
}

static vec_node *parse_primary(vec_parser *p, char *err, size_t errsz)
{
	switch (p->cur.kind) {
	case VEC_TOK_NUMBER: {
		vec_number v = p->cur.num;
		parser_next(p);
		vec_node *n = vec_node_number_new(v);
		if (!n)
			snprintf(err, errsz, "parse: out of memory");
		return n;
	}
	case VEC_TOK_IDENT: {
		const char *name = p->cur.start;
		size_t name_len = p->cur.len;
		parser_next(p);
		if (p->cur.kind == VEC_TOK_LPAREN) {
			parser_next(p);
			vec_node **args = NULL;
			size_t argc = 0, cap = 0;
			if (p->cur.kind != VEC_TOK_RPAREN) {
				for (;;) {
					vec_node *ex = parse_expr(p, err, errsz);
					if (!ex) {
						for (size_t i = 0; i < argc; i++)
							vec_node_destroy(args[i]);
						free(args);
						return NULL;
					}
					if (argc + 1 > cap) {
						size_t ncap = cap ? cap * 2 : 4;
						vec_node **na = realloc(args, ncap * sizeof(*na));
						if (!na) {
							vec_node_destroy(ex);
							for (size_t i = 0; i < argc; i++)
								vec_node_destroy(args[i]);
							free(args);
							snprintf(err, errsz, "parse: out of memory");
							return NULL;
						}
						args = na;
						cap = ncap;
					}
					args[argc++] = ex;

					if (p->cur.kind == VEC_TOK_COMMA) {
						parser_next(p);
						continue;
					}
					break;
				}
			}
			if (p->cur.kind != VEC_TOK_RPAREN) {
				for (size_t i = 0; i < argc; i++)
					vec_node_destroy(args[i]);
				free(args);
				snprintf(err, errsz, "parse: expected ')'");
				return NULL;
			}
			parser_next(p);
			vec_node *n = vec_node_call_new(name, name_len, argc, args);
			if (!n) {
				snprintf(err, errsz, "parse: out of memory");
				for (size_t i = 0; i < argc; i++)
					vec_node_destroy(args[i]);
				free(args);
				return NULL;
			}
			return n;
		}
		{
			vec_node *n = vec_node_ident_new(name, name_len);
			if (!n)
				snprintf(err, errsz, "parse: out of memory");
			return n;
		}
	}
	case VEC_TOK_LPAREN: {
		parser_next(p);
		vec_node *ex = parse_expr(p, err, errsz);
		if (!ex)
			return NULL;
		if (p->cur.kind != VEC_TOK_RPAREN) {
			vec_node_destroy(ex);
			snprintf(err, errsz, "parse: expected ')'");
			return NULL;
		}
		parser_next(p);
		return ex;
	}
	default:
		snprintf(err, errsz, "parse: unexpected token");
		return NULL;
	}
}

