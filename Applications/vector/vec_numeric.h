#ifndef VEC_NUMERIC_H
#define VEC_NUMERIC_H

#include "vec_ast.h"
#include "vec_env.h"

#include <stddef.h>

int vec_numeric_solve1_newton(vec_env *e, const vec_node *expr, double x0, double tol, int max_iter,
			      double *out_root, char *err, size_t errsz);
int vec_numeric_bisection_root(vec_env *e, const vec_node *expr, double a, double b, double tol, int max_iter,
			       double *out_root, char *err, size_t errsz);
int vec_numeric_secant_root(vec_env *e, const vec_node *expr, double x0, double x1, double tol, int max_iter,
			    double *out_root, char *err, size_t errsz);
int vec_numeric_diff_central(vec_env *e, const vec_node *expr, double x, double h,
			     double *out_d, char *err, size_t errsz);
int vec_numeric_integrate_trapezoid(vec_env *e, const vec_node *expr, double a, double b, int n,
				    double *out, char *err, size_t errsz);
int vec_numeric_integrate_simpson(vec_env *e, const vec_node *expr, double a, double b, int n,
				  double *out, char *err, size_t errsz);

int vec_numeric_interp1(const vec_value *data, double x, double *out_y, char *err, size_t errsz);

#endif

