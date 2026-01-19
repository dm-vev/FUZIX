#ifndef VEC_POLY_FACTOR_H
#define VEC_POLY_FACTOR_H

#include "vec_ast.h"
#include "vec_number.h"
#include "vec_poly_rat.h"

#include <stddef.h>

/* Returns: 0 ok, -1 error (err filled). */
int vec_poly_rat_resultant(const vec_poly_rat *a, const vec_poly_rat *b, vec_rat *out,
			   char *err, size_t errsz);

/* Returns: node (caller owns) or NULL on OOM/error (err filled). */
vec_node *vec_poly_rat_factor_integer(const vec_poly_rat *p, const char *var_name, char *err, size_t errsz);

#endif

