#include "vec_numeric.h"

#include "vec_eval.h"
#include "vec_number.h"
#include "vec_value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int eval_expr_x(vec_env *e, const vec_node *expr, double x, double *out_y, char *err, size_t errsz)
{
	if (!out_y) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}

	vec_value v;
	memset(&v, 0, sizeof(v));
	if (vec_eval_node_override(e, expr, "x", vec_value_number(vec_float(x)), &v, err, errsz) != 0)
		return -1;
	if (v.kind != VEC_VALUE_NUMBER) {
		vec_value_destroy(&v);
		snprintf(err, errsz, "eval: expected numeric expression");
		return -1;
	}
	*out_y = vec_number_float64(v.num);
	vec_value_destroy(&v);
	return 0;
}

int vec_numeric_solve1_newton(vec_env *e, const vec_node *expr, double x0, double tol, int max_iter,
			      double *out_root, char *err, size_t errsz)
{
	if (!out_root) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	double x = x0;
	for (int iter = 0; iter < max_iter; iter++) {
		double fx;
		if (eval_expr_x(e, expr, x, &fx, err, errsz) != 0)
			return -1;
		if (fabs(fx) <= tol) {
			*out_root = x;
			return 0;
		}
		double h = 1e-6 * (1 + fabs(x));
		double fph;
		if (eval_expr_x(e, expr, x + h, &fph, err, errsz) != 0)
			return -1;
		double df = (fph - fx) / h;
		if (df == 0 || isnan(df) || isinf(df)) {
			snprintf(err, errsz, "eval: solve1 derivative is zero/invalid");
			return -1;
		}
		double dx = -fx / df;
		double x_next = x + dx;
		if (fabs(dx) <= tol * (1 + fabs(x))) {
			*out_root = x_next;
			return 0;
		}
		x = x_next;
	}
	snprintf(err, errsz, "eval: solve1 did not converge");
	return -1;
}

int vec_numeric_bisection_root(vec_env *e, const vec_node *expr, double a, double b, double tol, int max_iter,
			       double *out_root, char *err, size_t errsz)
{
	if (!out_root) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	double fa;
	if (eval_expr_x(e, expr, a, &fa, err, errsz) != 0)
		return -1;
	double fb;
	if (eval_expr_x(e, expr, b, &fb, err, errsz) != 0)
		return -1;

	if (isnan(fa) || isnan(fb) || isinf(fa) || isinf(fb)) {
		snprintf(err, errsz, "eval: bisection invalid endpoint value");
		return -1;
	}
	if (fa == 0) {
		*out_root = a;
		return 0;
	}
	if (fb == 0) {
		*out_root = b;
		return 0;
	}
	if ((fa < 0) == (fb < 0)) {
		snprintf(err, errsz, "eval: bisection requires opposite signs at endpoints");
		return -1;
	}

	for (int iter = 0; iter < max_iter; iter++) {
		double m = (a + b) / 2;
		double fm;
		if (eval_expr_x(e, expr, m, &fm, err, errsz) != 0)
			return -1;
		if (fabs(fm) <= tol || fabs(b - a) <= tol * (1 + fabs(m))) {
			*out_root = m;
			return 0;
		}
		if ((fa < 0) != (fm < 0)) {
			b = m;
			fb = fm;
		} else {
			a = m;
			fa = fm;
		}
		(void)fb;
	}

	snprintf(err, errsz, "eval: bisection did not converge");
	return -1;
}

int vec_numeric_secant_root(vec_env *e, const vec_node *expr, double x0, double x1, double tol, int max_iter,
			    double *out_root, char *err, size_t errsz)
{
	if (!out_root) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	double f0;
	if (eval_expr_x(e, expr, x0, &f0, err, errsz) != 0)
		return -1;
	double f1;
	if (eval_expr_x(e, expr, x1, &f1, err, errsz) != 0)
		return -1;

	for (int iter = 0; iter < max_iter; iter++) {
		if (fabs(f1) <= tol) {
			*out_root = x1;
			return 0;
		}
		double den = f1 - f0;
		if (den == 0 || isnan(den) || isinf(den)) {
			snprintf(err, errsz, "eval: secant slope is zero/invalid");
			return -1;
		}
		double x2 = x1 - f1 * (x1 - x0) / den;
		if (fabs(x2 - x1) <= tol * (1 + fabs(x1))) {
			*out_root = x2;
			return 0;
		}
		x0 = x1;
		x1 = x2;
		f0 = f1;
		if (eval_expr_x(e, expr, x1, &f1, err, errsz) != 0)
			return -1;
	}

	snprintf(err, errsz, "eval: secant did not converge");
	return -1;
}

int vec_numeric_diff_central(vec_env *e, const vec_node *expr, double x, double h,
			     double *out_d, char *err, size_t errsz)
{
	if (!out_d) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	double fm;
	if (eval_expr_x(e, expr, x - h, &fm, err, errsz) != 0)
		return -1;
	double fp;
	if (eval_expr_x(e, expr, x + h, &fp, err, errsz) != 0)
		return -1;
	*out_d = (fp - fm) / (2 * h);
	return 0;
}

int vec_numeric_integrate_trapezoid(vec_env *e, const vec_node *expr, double a, double b, int n,
				    double *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	double h = (b - a) / (double)n;
	double fa;
	if (eval_expr_x(e, expr, a, &fa, err, errsz) != 0)
		return -1;
	double fb;
	if (eval_expr_x(e, expr, b, &fb, err, errsz) != 0)
		return -1;
	double sum = 0.5 * (fa + fb);
	for (int i = 1; i < n; i++) {
		double x = a + (double)i * h;
		double fx;
		if (eval_expr_x(e, expr, x, &fx, err, errsz) != 0)
			return -1;
		sum += fx;
	}
	*out = sum * h;
	return 0;
}

int vec_numeric_integrate_simpson(vec_env *e, const vec_node *expr, double a, double b, int n,
				  double *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	if (n & 1)
		n++;

	double h = (b - a) / (double)n;
	double sum_odd = 0;
	double sum_even = 0;

	for (int i = 1; i < n; i++) {
		double x = a + (double)i * h;
		double fx;
		if (eval_expr_x(e, expr, x, &fx, err, errsz) != 0)
			return -1;
		if (i & 1)
			sum_odd += fx;
		else
			sum_even += fx;
	}

	double fa;
	if (eval_expr_x(e, expr, a, &fa, err, errsz) != 0)
		return -1;
	double fb;
	if (eval_expr_x(e, expr, b, &fb, err, errsz) != 0)
		return -1;

	*out = (h / 3) * (fa + fb + 4 * sum_odd + 2 * sum_even);
	return 0;
}

int vec_numeric_roots_scan_bisection(vec_env *e, const vec_node *expr, double x_min, double x_max, int n,
				     double **out_roots, size_t *out_len, char *err, size_t errsz)
{
	if (!out_roots || !out_len) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	*out_roots = NULL;
	*out_len = 0;
	if (n < 2) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}

	double eps = 1e-10 * (1 + fabs(x_max - x_min));
	double last_root = NAN;

	size_t cap = 16;
	size_t count = 0;
	double *roots = malloc(sizeof(roots[0]) * cap);
	if (!roots) {
		snprintf(err, errsz, "eval: out of memory");
		return -1;
	}

	double prev_val = 0;
	double prev_t = 0;
	int prev_ok = 0;

	for (int i = 0; i < n; i++) {
		double t = x_min + (double)i * (x_max - x_min) / (double)(n - 1);
		double val;
		if (eval_expr_x(e, expr, t, &val, err, errsz) != 0) {
			free(roots);
			return -1;
		}
		if (isnan(val) || isinf(val)) {
			prev_ok = 0;
			continue;
		}

		if (fabs(val) <= eps) {
			if (isnan(last_root) || fabs(t - last_root) > 1e-6) {
				if (count == cap) {
					size_t ncap = cap * 2;
					double *nr = realloc(roots, sizeof(nr[0]) * ncap);
					if (!nr) {
						free(roots);
						snprintf(err, errsz, "eval: out of memory");
						return -1;
					}
					roots = nr;
					cap = ncap;
				}
				roots[count++] = t;
				last_root = t;
			}
			prev_ok = 0;
			continue;
		}

		if (prev_ok && ((prev_val < 0) != (val < 0))) {
			double a = prev_t;
			double b = t;
			double fa = prev_val;
			double fb = val;
			for (int iter = 0; iter < 64; iter++) {
				double m = (a + b) / 2;
				double fm;
				if (eval_expr_x(e, expr, m, &fm, err, errsz) != 0) {
					free(roots);
					return -1;
				}
				if (fabs(fm) <= eps || (b - a) <= eps) {
					if (isnan(last_root) || fabs(m - last_root) > 1e-6) {
						if (count == cap) {
							size_t ncap = cap * 2;
							double *nr = realloc(roots, sizeof(nr[0]) * ncap);
							if (!nr) {
								free(roots);
								snprintf(err, errsz, "eval: out of memory");
								return -1;
							}
							roots = nr;
							cap = ncap;
						}
						roots[count++] = m;
						last_root = m;
					}
					break;
				}
				if ((fa < 0) != (fm < 0)) {
					b = m;
					fb = fm;
				} else {
					a = m;
					fa = fm;
				}
				(void)fb;
			}
		}

		prev_ok = 1;
		prev_val = val;
		prev_t = t;
	}

	if (!count) {
		free(roots);
		return 0;
	}

	double *out = realloc(roots, sizeof(out[0]) * count);
	if (!out) {
		*out_roots = roots;
		*out_len = count;
		return 0;
	}
	*out_roots = out;
	*out_len = count;
	return 0;
}

int vec_numeric_interp1(const vec_value *data, double x, double *out_y, char *err, size_t errsz)
{
	if (!out_y) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	if (!data) {
		snprintf(err, errsz, "bad args");
		return -1;
	}

	if (data->kind == VEC_VALUE_ARRAY) {
		if (data->len == 0) {
			*out_y = NAN;
			return 0;
		}
		if (isnan(x)) {
			*out_y = NAN;
			return 0;
		}
		if (x <= 0) {
			*out_y = data->arr[0];
			return 0;
		}
		double last = (double)(data->len - 1);
		if (x >= last) {
			*out_y = data->arr[data->len - 1];
			return 0;
		}
		double f0 = floor(x);
		if (!isfinite(f0)) {
			*out_y = NAN;
			return 0;
		}
		int i0 = (int)f0;
		int i1 = i0 + 1;
		double t = x - (double)i0;
		*out_y = data->arr[(size_t)i0] + t * (data->arr[(size_t)i1] - data->arr[(size_t)i0]);
		return 0;
	}

	if (data->kind == VEC_VALUE_MATRIX) {
		if (data->cols != 2 || data->rows == 0) {
			snprintf(err, errsz, "expected Nx2 matrix");
			return -1;
		}
		if (x <= data->mat[0]) {
			*out_y = data->mat[1];
			return 0;
		}
		int last = data->rows - 1;
		if (x >= data->mat[(size_t)last * 2]) {
			*out_y = data->mat[(size_t)last * 2 + 1];
			return 0;
		}
		for (int i = 0; i < last; i++) {
			double x0 = data->mat[(size_t)i * 2];
			double y0 = data->mat[(size_t)i * 2 + 1];
			double x1 = data->mat[(size_t)(i + 1) * 2];
			double y1 = data->mat[(size_t)(i + 1) * 2 + 1];
			if ((x0 <= x && x <= x1) || (x1 <= x && x <= x0)) {
				if (x0 == x1) {
					*out_y = y0;
					return 0;
				}
				double t = (x - x0) / (x1 - x0);
				*out_y = y0 + t * (y1 - y0);
				return 0;
			}
		}
		*out_y = NAN;
		return 0;
	}

	snprintf(err, errsz, "expected array or matrix");
	return -1;
}
