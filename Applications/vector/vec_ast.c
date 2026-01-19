#include "vec_ast.h"

#include <stdlib.h>
#include <string.h>

static char *vec_strndup(const char *s, size_t n)
{
	char *out;
	if (!s)
		return NULL;
	out = malloc(n + 1);
	if (!out)
		return NULL;
	memcpy(out, s, n);
	out[n] = 0;
	return out;
}

static vec_node *vec_node_alloc(vec_node_kind kind)
{
	vec_node *n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->kind = kind;
	return n;
}

vec_node *vec_node_number_new(vec_number v)
{
	vec_node *n = vec_node_alloc(VEC_NODE_NUMBER);
	if (!n)
		return NULL;
	n->u.number = v;
	return n;
}

vec_node *vec_node_ident_new(const char *name, size_t len)
{
	vec_node *n = vec_node_alloc(VEC_NODE_IDENT);
	if (!n)
		return NULL;
	n->u.ident.name = vec_strndup(name, len);
	if (!n->u.ident.name) {
		free(n);
		return NULL;
	}
	return n;
}

vec_node *vec_node_unary_new(char op, vec_node *x)
{
	vec_node *n = vec_node_alloc(VEC_NODE_UNARY);
	if (!n) {
		vec_node_destroy(x);
		return NULL;
	}
	n->u.unary.op = op;
	n->u.unary.x = x;
	return n;
}

vec_node *vec_node_binary_new(char op, vec_node *left, vec_node *right)
{
	vec_node *n = vec_node_alloc(VEC_NODE_BINARY);
	if (!n) {
		vec_node_destroy(left);
		vec_node_destroy(right);
		return NULL;
	}
	n->u.binary.op = op;
	n->u.binary.left = left;
	n->u.binary.right = right;
	return n;
}

vec_node *vec_node_call_new(const char *name, size_t len, size_t argc, vec_node **args)
{
	vec_node *n = vec_node_alloc(VEC_NODE_CALL);
	if (!n) {
		if (args) {
			for (size_t i = 0; i < argc; i++)
				vec_node_destroy(args[i]);
		}
		free(args);
		return NULL;
	}
	n->u.call.name = vec_strndup(name, len);
	if (!n->u.call.name) {
		free(n);
		if (args) {
			for (size_t i = 0; i < argc; i++)
				vec_node_destroy(args[i]);
		}
		free(args);
		return NULL;
	}
	n->u.call.argc = argc;
	n->u.call.args = args;
	return n;
}

vec_node *vec_node_compare_new(vec_cmp_op op, vec_node *left, vec_node *right)
{
	vec_node *n = vec_node_alloc(VEC_NODE_COMPARE);
	if (!n) {
		vec_node_destroy(left);
		vec_node_destroy(right);
		return NULL;
	}
	n->u.compare.op = op;
	n->u.compare.left = left;
	n->u.compare.right = right;
	return n;
}

void vec_node_destroy(vec_node *n)
{
	if (!n)
		return;
	switch (n->kind) {
	case VEC_NODE_NUMBER:
		break;
	case VEC_NODE_IDENT:
		free(n->u.ident.name);
		break;
	case VEC_NODE_UNARY:
		vec_node_destroy(n->u.unary.x);
		break;
	case VEC_NODE_BINARY:
		vec_node_destroy(n->u.binary.left);
		vec_node_destroy(n->u.binary.right);
		break;
	case VEC_NODE_CALL:
		for (size_t i = 0; i < n->u.call.argc; i++)
			vec_node_destroy(n->u.call.args[i]);
		free(n->u.call.args);
		free(n->u.call.name);
		break;
	case VEC_NODE_COMPARE:
		vec_node_destroy(n->u.compare.left);
		vec_node_destroy(n->u.compare.right);
		break;
	default:
		break;
	}
	free(n);
}

int vec_node_has_ident(const vec_node *n, const char *name)
{
	if (!n || !name)
		return 0;
	switch (n->kind) {
	case VEC_NODE_IDENT:
		return (n->u.ident.name && !strcmp(n->u.ident.name, name)) ? 1 : 0;
	case VEC_NODE_NUMBER:
		return 0;
	case VEC_NODE_UNARY:
		return vec_node_has_ident(n->u.unary.x, name);
	case VEC_NODE_BINARY:
		return vec_node_has_ident(n->u.binary.left, name) || vec_node_has_ident(n->u.binary.right, name);
	case VEC_NODE_CALL:
		for (size_t i = 0; i < n->u.call.argc; i++) {
			if (vec_node_has_ident(n->u.call.args[i], name))
				return 1;
		}
		return 0;
	case VEC_NODE_COMPARE:
		return vec_node_has_ident(n->u.compare.left, name) || vec_node_has_ident(n->u.compare.right, name);
	default:
		return 0;
	}
}

vec_node *vec_node_clone(const vec_node *n)
{
	if (!n)
		return NULL;
	switch (n->kind) {
	case VEC_NODE_NUMBER:
		return vec_node_number_new(n->u.number);
	case VEC_NODE_IDENT:
		if (!n->u.ident.name)
			return NULL;
		return vec_node_ident_new(n->u.ident.name, strlen(n->u.ident.name));
	case VEC_NODE_UNARY: {
		vec_node *x = vec_node_clone(n->u.unary.x);
		if (!x)
			return NULL;
		return vec_node_unary_new(n->u.unary.op, x);
	}
	case VEC_NODE_BINARY: {
		vec_node *l = vec_node_clone(n->u.binary.left);
		vec_node *r = vec_node_clone(n->u.binary.right);
		if (!l || !r) {
			vec_node_destroy(l);
			vec_node_destroy(r);
			return NULL;
		}
		return vec_node_binary_new(n->u.binary.op, l, r);
	}
	case VEC_NODE_CALL: {
		if (!n->u.call.name)
			return NULL;
		size_t argc = n->u.call.argc;
		vec_node **args = NULL;
		if (argc) {
			args = calloc(argc, sizeof(args[0]));
			if (!args)
				return NULL;
			for (size_t i = 0; i < argc; i++) {
				args[i] = vec_node_clone(n->u.call.args[i]);
				if (!args[i]) {
					for (size_t j = 0; j < argc; j++)
						vec_node_destroy(args[j]);
					free(args);
					return NULL;
				}
			}
		}
		return vec_node_call_new(n->u.call.name, strlen(n->u.call.name), argc, args);
	}
	case VEC_NODE_COMPARE: {
		vec_node *l = vec_node_clone(n->u.compare.left);
		vec_node *r = vec_node_clone(n->u.compare.right);
		if (!l || !r) {
			vec_node_destroy(l);
			vec_node_destroy(r);
			return NULL;
		}
		return vec_node_compare_new(n->u.compare.op, l, r);
	}
	default:
		return NULL;
	}
}
