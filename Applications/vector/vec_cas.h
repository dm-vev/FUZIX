#ifndef VEC_CAS_H
#define VEC_CAS_H

#include "vec_ast.h"

#include <stddef.h>

vec_node *vec_node_simplify(const vec_node *n);
vec_node *vec_node_simplify_owned(vec_node *n);
vec_node *vec_node_deriv(const vec_node *n, const char *var_name);
size_t vec_node_size(const vec_node *n);

/* Writes a representation like Spark's NodeString. */
int vec_node_to_string(const vec_node *n, char *buf, size_t bufsz);

#endif
