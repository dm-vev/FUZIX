#ifndef VEC_PARSER_H
#define VEC_PARSER_H

#include "vec_ast.h"

#include <stddef.h>

typedef enum {
	VEC_ACT_EVAL = 0,
	VEC_ACT_ASSIGN_VAR = 1,
	VEC_ACT_ASSIGN_FUNC = 2,
} vec_action_kind;

typedef struct {
	vec_action_kind kind;
	vec_node *expr;
	char *var_name;
	char *func_name;
	char *func_param;
} vec_action;

typedef struct {
	vec_action *items;
	size_t count;
	size_t cap;
} vec_actions;

void vec_actions_destroy(vec_actions *a);

int vec_parse_input(const char *s, vec_actions *out, char *err, size_t errsz);

#endif

