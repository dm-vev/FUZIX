#ifndef VEC_CAS_EXT_H
#define VEC_CAS_EXT_H

#include "vec_ast.h"
#include "vec_env.h"

#include <stddef.h>

/* Returns expanded node (caller owns) or NULL on OOM. */
vec_node *vec_node_expand(const vec_node *n);

/* Returns Taylor series node (caller owns) or NULL on error (err filled). */
vec_node *vec_node_taylor_series(vec_env *e, const vec_node *expr, const char *var_name,
				 double a, int n, char *err, size_t errsz);

#endif

