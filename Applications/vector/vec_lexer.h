#ifndef VEC_LEXER_H
#define VEC_LEXER_H

#include "vec_number.h"

#include <stddef.h>

typedef enum {
	VEC_TOK_EOF = 0,
	VEC_TOK_NUMBER,
	VEC_TOK_IDENT,
	VEC_TOK_SEMI,
	VEC_TOK_PLUS,
	VEC_TOK_MINUS,
	VEC_TOK_STAR,
	VEC_TOK_SLASH,
	VEC_TOK_CARET,
	VEC_TOK_LPAREN,
	VEC_TOK_RPAREN,
	VEC_TOK_COMMA,
	VEC_TOK_ASSIGN,
	VEC_TOK_EQ,
	VEC_TOK_NE,
	VEC_TOK_LT,
	VEC_TOK_LE,
	VEC_TOK_GT,
	VEC_TOK_GE,
} vec_tok_kind;

typedef struct {
	vec_tok_kind kind;
	const char *start;
	size_t len;
	vec_number num;
} vec_token;

typedef struct {
	const char *s;
	size_t i;
} vec_lexer;

void vec_lexer_init(vec_lexer *l, const char *s);
vec_token vec_lexer_next(vec_lexer *l);

const char *vec_token_text(vec_tok_kind kind);

#endif

