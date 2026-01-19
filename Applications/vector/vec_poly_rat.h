#ifndef VEC_POLY_RAT_H
#define VEC_POLY_RAT_H

#include "vec_ast.h"
#include "vec_env.h"
#include "vec_number.h"

#include <stddef.h>

typedef struct {
	vec_rat *coeffs;
	size_t len;
} vec_poly_rat;

void vec_poly_rat_destroy(vec_poly_rat *p);
int vec_poly_rat_degree(const vec_poly_rat *p);

/* Returns: 0 ok, -1 error (err filled). */
int vec_poly_rat_from_expr(vec_env *e, const vec_node *expr, const char *var_name,
			   vec_poly_rat *out, char *err, size_t errsz);

/* p <- monic(p). Returns 0 ok, -1 error (err filled). */
int vec_poly_rat_monic(vec_poly_rat *p, char *err, size_t errsz);

/* q,r are allocated and returned. Returns 0 ok, -1 error (err filled). */
int vec_poly_rat_divmod(const vec_poly_rat *a, const vec_poly_rat *b,
			vec_poly_rat *out_q, vec_poly_rat *out_r, char *err, size_t errsz);

/* out <- gcd(a,b) (monic). Returns 0 ok, -1 error (err filled). */
int vec_poly_rat_gcd(const vec_poly_rat *a, const vec_poly_rat *b, vec_poly_rat *out, char *err, size_t errsz);

/* out <- a*b. Returns 0 ok, -1 error (err filled). */
int vec_poly_rat_mul(const vec_poly_rat *a, const vec_poly_rat *b, vec_poly_rat *out, char *err, size_t errsz);

/* Builds a Horner-form expression with Rational coefficients. Returns NULL on OOM. */
vec_node *vec_poly_rat_to_expr_horner(const vec_poly_rat *p, const char *var_name);

#endif

