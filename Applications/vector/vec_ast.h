#ifndef VEC_AST_H
#define VEC_AST_H

#include "vec_number.h"

#include <stddef.h>

typedef enum {
	VEC_NODE_NUMBER = 0,
	VEC_NODE_IDENT = 1,
	VEC_NODE_UNARY = 2,
	VEC_NODE_BINARY = 3,
	VEC_NODE_CALL = 4,
	VEC_NODE_COMPARE = 5,
} vec_node_kind;

typedef enum {
	VEC_CMP_EQ = 0,
	VEC_CMP_NE,
	VEC_CMP_LT,
	VEC_CMP_LE,
	VEC_CMP_GT,
	VEC_CMP_GE,
} vec_cmp_op;

typedef struct vec_node vec_node;

struct vec_node {
	vec_node_kind kind;
	union {
		vec_number number;
		struct {
			char *name;
		} ident;
		struct {
			char op;
			vec_node *x;
		} unary;
		struct {
			char op;
			vec_node *left;
			vec_node *right;
		} binary;
		struct {
			char *name;
			size_t argc;
			vec_node **args;
		} call;
		struct {
			vec_cmp_op op;
			vec_node *left;
			vec_node *right;
		} compare;
	} u;
};

vec_node *vec_node_number_new(vec_number n);
vec_node *vec_node_ident_new(const char *name, size_t len);
vec_node *vec_node_unary_new(char op, vec_node *x);
vec_node *vec_node_binary_new(char op, vec_node *left, vec_node *right);
vec_node *vec_node_call_new(const char *name, size_t len, size_t argc, vec_node **args);
vec_node *vec_node_compare_new(vec_cmp_op op, vec_node *left, vec_node *right);

void vec_node_destroy(vec_node *n);

int vec_node_has_ident(const vec_node *n, const char *name);
vec_node *vec_node_clone(const vec_node *n);

#endif
