#ifndef VEC_POLY_H
#define VEC_POLY_H

#include "vec_ast.h"
#include "vec_env.h"

#include <stddef.h>

typedef struct {
	double *coeffs;
	size_t len;
} vec_poly;

void vec_poly_destroy(vec_poly *p);
int vec_poly_degree(const vec_poly *p);

/* Returns: 0 ok, -1 error (err filled). */
int vec_poly_from_expr(vec_env *e, const vec_node *expr, const char *var_name,
		       vec_poly *out, char *err, size_t errsz);

/* Returns: 0 ok, -1 error (err filled). */
int vec_poly_from_coeffs(const double *coeffs, size_t len, vec_poly *out, char *err, size_t errsz);

/* Builds a Horner-form expression with Float coefficients. Returns NULL on OOM. */
vec_node *vec_poly_to_expr_horner(const vec_poly *p, const char *var_name);

/* Evaluate p(x) for real x. */
double vec_poly_eval(const vec_poly *p, double x);

#endif

