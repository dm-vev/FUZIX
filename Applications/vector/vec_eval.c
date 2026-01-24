#include "vec_eval.h"
#ifndef VEC_LITE
#include "vec_cas.h"
#include "vec_cas_ext.h"
#include "vec_linalg.h"
#include "vec_matrix.h"
#include "vec_numeric.h"
#include "vec_poly.h"
#include "vec_poly_factor.h"
#include "vec_poly_rat.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int vec_eval_node_impl(vec_env *e, const vec_node *n, const char *ov_name, const vec_value *ov_value,
			      vec_value *out, char *err, size_t errsz);

static vec_number neg_number(vec_number n)
{
	if (n.kind == VEC_NUM_RAT) {
		vec_rat r = n.r;
		r.num = -r.num;
		return vec_rat_number(r);
	}
	return vec_float(-n.f);
}

static vec_number add_number(vec_env *e, vec_number a, vec_number b)
{
	if (e && e->mode == VEC_MODE_EXACT && a.kind == VEC_NUM_RAT && b.kind == VEC_NUM_RAT) {
		vec_rat r;
		if (vec_rat_add(a.r, b.r, &r) == VEC_NUM_OK)
			return vec_rat_number(r);
	}
	return vec_float(vec_number_float64(a) + vec_number_float64(b));
}

static vec_number sub_number(vec_env *e, vec_number a, vec_number b)
{
	if (e && e->mode == VEC_MODE_EXACT && a.kind == VEC_NUM_RAT && b.kind == VEC_NUM_RAT) {
		vec_rat r;
		if (vec_rat_sub(a.r, b.r, &r) == VEC_NUM_OK)
			return vec_rat_number(r);
	}
	return vec_float(vec_number_float64(a) - vec_number_float64(b));
}

static vec_number mul_number(vec_env *e, vec_number a, vec_number b)
{
	if (e && e->mode == VEC_MODE_EXACT && a.kind == VEC_NUM_RAT && b.kind == VEC_NUM_RAT) {
		vec_rat r;
		if (vec_rat_mul(a.r, b.r, &r) == VEC_NUM_OK)
			return vec_rat_number(r);
	}
	return vec_float(vec_number_float64(a) * vec_number_float64(b));
}

static int div_number(vec_env *e, vec_number a, vec_number b, vec_number *out, char *err, size_t errsz)
{
	double bf = vec_number_float64(b);
	if (bf == 0) {
		snprintf(err, errsz, "eval: division by zero");
		return -1;
	}
	if (e && e->mode == VEC_MODE_EXACT && a.kind == VEC_NUM_RAT && b.kind == VEC_NUM_RAT) {
		vec_rat r;
		if (vec_rat_div(a.r, b.r, &r) == VEC_NUM_OK) {
			*out = vec_rat_number(r);
			return 0;
		}
	}
	*out = vec_float(vec_number_float64(a) / bf);
	return 0;
}

static vec_number pow_number(vec_env *e, vec_number a, vec_number b)
{
	if (e && e->mode == VEC_MODE_EXACT && a.kind == VEC_NUM_RAT && b.kind == VEC_NUM_RAT && b.r.den == 1) {
		vec_rat r;
		if (vec_rat_pow_int(a.r, b.r.num, &r) == VEC_NUM_OK)
			return vec_rat_number(r);
	}
	return vec_float(pow(vec_number_float64(a), vec_number_float64(b)));
}

static vec_complex c_add(vec_complex a, vec_complex b)
{
	vec_complex z;
	z.re = a.re + b.re;
	z.im = a.im + b.im;
	return z;
}

static vec_complex c_sub(vec_complex a, vec_complex b)
{
	vec_complex z;
	z.re = a.re - b.re;
	z.im = a.im - b.im;
	return z;
}

static vec_complex c_mul(vec_complex a, vec_complex b)
{
	vec_complex z;
	z.re = a.re * b.re - a.im * b.im;
	z.im = a.re * b.im + a.im * b.re;
	return z;
}

static vec_complex c_div(vec_complex a, vec_complex b)
{
	double d = b.re * b.re + b.im * b.im;
	vec_complex z;
	z.re = (a.re * b.re + a.im * b.im) / d;
	z.im = (a.im * b.re - a.re * b.im) / d;
	return z;
}

static int c_is_zero(vec_complex z)
{
	return z.re == 0 && z.im == 0;
}

static double c_abs(vec_complex z)
{
	return hypot(z.re, z.im);
}

static double c_arg(vec_complex z)
{
	return atan2(z.im, z.re);
}

static vec_complex c_conj(vec_complex z)
{
	z.im = -z.im;
	return z;
}

static vec_complex poly_eval_complex(const double *coeffs, size_t len, vec_complex z)
{
	vec_complex out;
	out.re = 0;
	out.im = 0;
	if (!coeffs || len == 0)
		return out;
	out.re = coeffs[len - 1];
	for (size_t i = len - 1; i > 0; i--) {
		vec_complex c;
		c.re = coeffs[i - 1];
		c.im = 0;
		out = c_add(c_mul(out, z), c);
	}
	return out;
}

static vec_complex c_exp(vec_complex z)
{
	double ea = exp(z.re);
	vec_complex out;
	out.re = ea * cos(z.im);
	out.im = ea * sin(z.im);
	return out;
}

static vec_complex c_log(vec_complex z)
{
	vec_complex out;
	out.re = log(c_abs(z));
	out.im = c_arg(z);
	return out;
}

static vec_complex c_pow(vec_complex a, vec_complex b)
{
	if (c_is_zero(a)) {
		if (c_is_zero(b)) {
			vec_complex one = {1, 0};
			return one;
		}
		if (b.im == 0 && b.re > 0) {
			vec_complex zero = {0, 0};
			return zero;
		}
		vec_complex nan = {NAN, NAN};
		return nan;
	}

	vec_complex wlog = c_mul(b, c_log(a));
	return c_exp(wlog);
}

static vec_complex c_sqrt(vec_complex z)
{
	if (z.im == 0 && z.re >= 0) {
		vec_complex out = {sqrt(z.re), 0};
		return out;
	}
	double r = c_abs(z);
	double t = sqrt((r + z.re) * 0.5);
	double u = sqrt((r - z.re) * 0.5);
	if (z.im < 0)
		u = -u;
	vec_complex out = {t, u};
	return out;
}

static double sinh_d(double x)
{
	double ex = exp(x);
	double em = exp(-x);
	return (ex - em) * 0.5;
}

static double cosh_d(double x)
{
	double ex = exp(x);
	double em = exp(-x);
	return (ex + em) * 0.5;
}

static double tan_d(double x)
{
	return sin(x) / cos(x);
}

static double cot_d(double x)
{
	return 1 / tan_d(x);
}

static double sec_d(double x)
{
	return 1 / cos(x);
}

static double csc_d(double x)
{
	return 1 / sin(x);
}

static double tanh_d(double x)
{
	double ex2 = exp(2 * x);
	return (ex2 - 1) / (ex2 + 1);
}

static double asinh_d(double x)
{
	return log(x + sqrt(x * x + 1));
}

static double acosh_d(double x)
{
	return log(x + sqrt(x - 1) * sqrt(x + 1));
}

static double atanh_d(double x)
{
	return 0.5 * log((1 + x) / (1 - x));
}

static double sign_d(double x)
{
	if (isnan(x))
		return x;
	if (x > 0)
		return 1;
	if (x < 0)
		return -1;
	return 0;
}

static double trunc_d(double x)
{
	if (x < 0)
		return ceil(x);
	return floor(x);
}

static double round_d(double x)
{
	if (isnan(x) || isinf(x))
		return x;
	if (x < 0)
		return ceil(x - 0.5);
	return floor(x + 0.5);
}

static double cbrt_d(double x)
{
	if (x == 0 || isnan(x) || isinf(x))
		return x;
	if (x < 0)
		return -pow(-x, 1.0 / 3.0);
	return pow(x, 1.0 / 3.0);
}

static double exp2_d(double x)
{
	return exp(x * M_LN2);
}

static double log2_d(double x)
{
	return log(x) / M_LN2;
}

static double log10_d(double x)
{
	return log(x) / M_LN10;
}

static double expm1_d(double x)
{
	return exp(x) - 1;
}

static double log1p_d(double x)
{
	return log(1 + x);
}

static double rad_d(double x)
{
	return x * M_PI / 180.0;
}

static double deg_d(double x)
{
	return x * 180.0 / M_PI;
}

static double sq_d(double x)
{
	return x * x;
}

static double cube_d(double x)
{
	return x * x * x;
}

static double saturate_d(double x)
{
	if (x < 0)
		return 0;
	if (x > 1)
		return 1;
	return x;
}

typedef double (*unary_d_fn)(double);

struct unary_builtin {
	const char *name;
	unary_d_fn fn;
};

static const struct unary_builtin unary_array_builtins[] = {
	{"sin", sin},
	{"cos", cos},
	{"tan", tan_d},
	{"asin", asin},
	{"acos", acos},
	{"atan", atan},
	{"cot", cot_d},
	{"sec", sec_d},
	{"csc", csc_d},

	{"sinh", sinh_d},
	{"cosh", cosh_d},
	{"tanh", tanh_d},
	{"asinh", asinh_d},
	{"acosh", acosh_d},
	{"atanh", atanh_d},

	{"sqrt", sqrt},
	{"cbrt", cbrt_d},

	{"abs", fabs},
	{"sign", sign_d},

	{"exp", exp},
	{"expm1", expm1_d},
	{"exp2", exp2_d},
	{"ln", log},
	{"log", log},
	{"log10", log10_d},
	{"log2", log2_d},
	{"log1p", log1p_d},

	{"floor", floor},
	{"ceil", ceil},
	{"trunc", trunc_d},
	{"round", round_d},

	{"rad", rad_d},
	{"deg", deg_d},
	{"saturate", saturate_d},
	{"sq", sq_d},
	{"cube", cube_d},
};

static unary_d_fn unary_builtin_find(const char *name)
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < sizeof(unary_array_builtins) / sizeof(unary_array_builtins[0]); i++) {
		const struct unary_builtin *b = &unary_array_builtins[i];
		if (!strcmp(b->name, name))
			return b->fn;
	}
	return NULL;
}

typedef double (*agg_d_fn)(const double *xs, size_t n);

struct agg_builtin {
	const char *name;
	agg_d_fn fn;
};

static int dbl_cmp(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;
	return (da > db) - (da < db);
}

static double agg_len(const double *xs, size_t n)
{
	(void)xs;
	return (double)n;
}

static double agg_sum(const double *xs, size_t n)
{
	double total = 0;
	for (size_t i = 0; i < n; i++)
		total += xs[i];
	return total;
}

static double agg_avg(const double *xs, size_t n)
{
	if (!n)
		return NAN;
	return agg_sum(xs, n) / (double)n;
}

static double agg_min(const double *xs, size_t n)
{
	if (!n)
		return NAN;
	double m = xs[0];
	for (size_t i = 1; i < n; i++)
		if (xs[i] < m)
			m = xs[i];
	return m;
}

static double agg_max(const double *xs, size_t n)
{
	if (!n)
		return NAN;
	double m = xs[0];
	for (size_t i = 1; i < n; i++)
		if (xs[i] > m)
			m = xs[i];
	return m;
}

static double agg_median(const double *xs, size_t n)
{
	if (!n)
		return NAN;
	double *tmp = malloc(sizeof(tmp[0]) * n);
	if (!tmp)
		return NAN;
	memcpy(tmp, xs, sizeof(tmp[0]) * n);
	qsort(tmp, n, sizeof(tmp[0]), dbl_cmp);
	size_t mid = n / 2;
	double out;
	if (n & 1)
		out = tmp[mid];
	else
		out = 0.5 * (tmp[mid - 1] + tmp[mid]);
	free(tmp);
	return out;
}

static double agg_variance(const double *xs, size_t n)
{
	if (!n)
		return NAN;
	double mean = agg_avg(xs, n);
	double sum = 0;
	for (size_t i = 0; i < n; i++) {
		double d = xs[i] - mean;
		sum += d * d;
	}
	return sum / (double)n;
}

static double agg_std(const double *xs, size_t n)
{
	return sqrt(agg_variance(xs, n));
}

static const struct agg_builtin array_agg_builtins[] = {
	{"len", agg_len},
	{"sum", agg_sum},
	{"avg", agg_avg},
	{"mean", agg_avg},
	{"min", agg_min},
	{"max", agg_max},
	{"median", agg_median},
	{"variance", agg_variance},
	{"std", agg_std},
};

static agg_d_fn agg_builtin_find(const char *name)
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < sizeof(array_agg_builtins) / sizeof(array_agg_builtins[0]); i++) {
		const struct agg_builtin *b = &array_agg_builtins[i];
		if (!strcmp(b->name, name))
			return b->fn;
	}
	return NULL;
}

static int vector_index(double x, size_t size, size_t *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "eval: bad index args");
		return -1;
	}
	if (isnan(x) || isinf(x)) {
		snprintf(err, errsz, "eval: invalid index %g", x);
		return -1;
	}
	if (x != trunc_d(x)) {
		snprintf(err, errsz, "eval: index must be an integer: %g", x);
		return -1;
	}
	int i = (int)x;
	if (i < 1 || (size_t)i > size) {
		snprintf(err, errsz, "eval: index out of range: %d", i);
		return -1;
	}
	*out = (size_t)(i - 1);
	return 0;
}

static int require_int(double x, int min, int max, int *out, const char *what, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	if (isnan(x) || isinf(x)) {
		snprintf(err, errsz, "eval: invalid %s %g", what ? what : "value", x);
		return -1;
	}
	if (x != trunc_d(x)) {
		snprintf(err, errsz, "eval: %s must be an integer: %g", what ? what : "value", x);
		return -1;
	}
	int i = (int)x;
	if (i < min || i > max) {
		snprintf(err, errsz, "eval: %s must be %d..%d", what ? what : "value", min, max);
		return -1;
	}
	*out = i;
	return 0;
}

static vec_complex c_sin(vec_complex z)
{
	/* sin(a+ib) = sin a cosh b + i cos a sinh b */
	double a = z.re;
	double b = z.im;
	double sa = sin(a);
	double ca = cos(a);
	double cb = cosh_d(b);
	double sb = sinh_d(b);
	vec_complex out;
	out.re = sa * cb;
	out.im = ca * sb;
	return out;
}

static vec_complex c_cos(vec_complex z)
{
	/* cos(a+ib) = cos a cosh b - i sin a sinh b */
	double a = z.re;
	double b = z.im;
	double sa = sin(a);
	double ca = cos(a);
	double cb = cosh_d(b);
	double sb = sinh_d(b);
	vec_complex out;
	out.re = ca * cb;
	out.im = -sa * sb;
	return out;
}

static int value_to_complex(const vec_value *v, vec_complex *out)
{
	if (!v || !out)
		return -1;
	switch (v->kind) {
	case VEC_VALUE_COMPLEX:
		*out = v->c;
		return 0;
	case VEC_VALUE_NUMBER:
		out->re = vec_number_float64(v->num);
		out->im = 0;
		return 0;
	default:
		return -1;
	}
}

static int value_truthy(const vec_value *v, int *out)
{
	if (!v || !out)
		return -1;
	switch (v->kind) {
	case VEC_VALUE_NUMBER:
		*out = vec_number_float64(v->num) != 0 ? 1 : 0;
		return 0;
	case VEC_VALUE_COMPLEX:
		*out = !c_is_zero(v->c);
		return 0;
	default:
		return -1;
	}
}

struct env_saved_var {
	int had;
	vec_value v;
};

static int env_save_var(vec_env *e, const char *name, struct env_saved_var *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (!e || !name) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	int rc = vec_env_get_var(e, name, &out->v);
	if (rc == 0) {
		out->had = 1;
		return 0;
	}
	if (rc == 1) {
		out->had = 0;
		memset(&out->v, 0, sizeof(out->v));
		return 0;
	}
	snprintf(err, errsz, "eval: out of memory");
	return -1;
}

static void env_restore_var(vec_env *e, const char *name, struct env_saved_var *sv)
{
	if (!e || !name || !sv)
		return;
	if (sv->had) {
		(void)vec_env_set_var(e, name, sv->v);
		memset(&sv->v, 0, sizeof(sv->v));
	} else {
		(void)vec_env_unset_var(e, name);
	}
}

static int eval_float(vec_env *e, const vec_node *expr, double *out_y, char *err, size_t errsz)
{
	if (!out_y) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	vec_value v;
	memset(&v, 0, sizeof(v));
	if (vec_eval_node(e, expr, &v, err, errsz) != 0) {
		vec_value_destroy(&v);
		return -1;
	}
	if (v.kind != VEC_VALUE_NUMBER) {
		vec_value_destroy(&v);
		snprintf(err, errsz, "eval: expected numeric expression");
		return -1;
	}
	*out_y = vec_number_float64(v.num);
	vec_value_destroy(&v);
	return 0;
}

static int xy_append(double **data, size_t *len, size_t *cap, double x, double y, char *err, size_t errsz)
{
	if (!data || !len || !cap) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	if (*len + 2 > *cap) {
		size_t want = *len + 2;
		size_t ncap = *cap ? (*cap * 2) : 256;
		if (ncap < *cap)
			ncap = want;
		while (ncap < want) {
			size_t next = ncap * 2;
			if (next < ncap) {
				ncap = want;
				break;
			}
			ncap = next;
		}
		double *nd = realloc(*data, sizeof(nd[0]) * ncap);
		if (!nd) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*data = nd;
		*cap = ncap;
	}
	(*data)[*len + 0] = x;
	(*data)[*len + 1] = y;
	*len += 2;
	return 0;
}

static int xy_append_break(double **data, size_t *len, size_t *cap, char *err, size_t errsz)
{
	if (!data || !len || !cap) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	if (*len >= 2 && isnan((*data)[*len - 2]) && isnan((*data)[*len - 1]))
		return 0;
	return xy_append(data, len, cap, NAN, NAN, err, errsz);
}

static int xy_append_segment(double **data, size_t *len, size_t *cap,
			     double x0, double y0, double x1, double y1, char *err, size_t errsz)
{
	if (xy_append(data, len, cap, x0, y0, err, errsz) != 0)
		return -1;
	if (xy_append(data, len, cap, x1, y1, err, errsz) != 0)
		return -1;
	if (xy_append_break(data, len, cap, err, errsz) != 0)
		return -1;
	return 0;
}

static int plot_vectorfield_segments(vec_env *e, const vec_node *fx, const vec_node *fy,
				     double xmin, double xmax, double ymin, double ymax, int n,
				     double **out_data, int *out_rows, char *err, size_t errsz)
{
	if (!out_data || !out_rows) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	*out_data = NULL;
	*out_rows = 0;
	if (!e || !fx || !fy) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}

	struct env_saved_var prev_x = {0};
	struct env_saved_var prev_y = {0};
	if (env_save_var(e, "x", &prev_x, err, errsz) != 0)
		return -1;
	if (env_save_var(e, "y", &prev_y, err, errsz) != 0) {
		vec_value_destroy(&prev_x.v);
		return -1;
	}

	if (xmin >= xmax || ymin >= ymax) {
		env_restore_var(e, "y", &prev_y);
		env_restore_var(e, "x", &prev_x);
		snprintf(err, errsz, "eval: vectorfield expects min < max");
		return -1;
	}
	if (n < 2)
		n = 2;

	double dx = (xmax - xmin) / (double)(n - 1);
	double dy = (ymax - ymin) / (double)(n - 1);
	double scale = 0.35 * (dx < dy ? dx : dy);

	double *data = NULL;
	size_t len = 0;
	size_t cap = 0;

	for (int j = 0; j < n; j++) {
		double y = ymin + (double)j * dy;
		for (int i = 0; i < n; i++) {
			double x = xmin + (double)i * dx;
			if (vec_env_set_var(e, "x", vec_value_number(vec_float(x))) != 0 ||
			    vec_env_set_var(e, "y", vec_value_number(vec_float(y))) != 0) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			double vx, vy;
			if (eval_float(e, fx, &vx, err, errsz) != 0 ||
			    eval_float(e, fy, &vy, err, errsz) != 0)
				goto fail;
			if (isnan(vx) || isnan(vy) || isinf(vx) || isinf(vy))
				continue;
			double mag = hypot(vx, vy);
			if (mag == 0 || isnan(mag) || isinf(mag))
				continue;
			double ux = vx / mag;
			double uy = vy / mag;
			double x1 = x + ux * scale;
			double y1 = y + uy * scale;
			if (xy_append(&data, &len, &cap, x, y, err, errsz) != 0 ||
			    xy_append(&data, &len, &cap, x1, y1, err, errsz) != 0 ||
			    xy_append_break(&data, &len, &cap, err, errsz) != 0)
				goto fail;
		}
	}

	env_restore_var(e, "y", &prev_y);
	env_restore_var(e, "x", &prev_x);
	int rows = (int)(len / 2);
	if (rows == 0) {
		free(data);
		data = malloc(sizeof(data[0]) * 2);
		if (!data) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		data[0] = NAN;
		data[1] = NAN;
		rows = 1;
	}
	*out_data = data;
	*out_rows = rows;
	return 0;

fail:
	free(data);
	env_restore_var(e, "y", &prev_y);
	env_restore_var(e, "x", &prev_x);
	return -1;
}

static int ms_interp(double x0, double y0, double z0, double x1, double y1, double z1,
		     double level, double *out_x, double *out_y)
{
	if (!out_x || !out_y)
		return 0;
	if (isnan(z0) || isnan(z1) || isinf(z0) || isinf(z1))
		return 0;
	double dz = z1 - z0;
	double t = 0.5;
	if (dz != 0)
		t = (level - z0) / dz;
	if (t < 0)
		t = 0;
	else if (t > 1)
		t = 1;
	*out_x = x0 + t * (x1 - x0);
	*out_y = y0 + t * (y1 - y0);
	return 1;
}

static int plot_contour_marching_squares(vec_env *e, const vec_node *f, const double *levels, size_t nlevels,
					double xmin, double xmax, double ymin, double ymax, int n,
					double **out_data, int *out_rows, char *err, size_t errsz)
{
	if (!out_data || !out_rows) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}
	*out_data = NULL;
	*out_rows = 0;
	if (!e || !f || !levels || nlevels == 0) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}

	struct env_saved_var prev_x = {0};
	struct env_saved_var prev_y = {0};
	if (env_save_var(e, "x", &prev_x, err, errsz) != 0)
		return -1;
	if (env_save_var(e, "y", &prev_y, err, errsz) != 0) {
		vec_value_destroy(&prev_x.v);
		return -1;
	}

	if (xmin >= xmax || ymin >= ymax) {
		env_restore_var(e, "y", &prev_y);
		env_restore_var(e, "x", &prev_x);
		snprintf(err, errsz, "eval: contour expects min < max");
		return -1;
	}
	if (n < 8)
		n = 8;

	double *xs = malloc(sizeof(xs[0]) * (size_t)n);
	double *ys = malloc(sizeof(ys[0]) * (size_t)n);
	double *val = malloc(sizeof(val[0]) * (size_t)n * (size_t)n);
	if (!xs || !ys || !val) {
		free(xs);
		free(ys);
		free(val);
		env_restore_var(e, "y", &prev_y);
		env_restore_var(e, "x", &prev_x);
		snprintf(err, errsz, "eval: out of memory");
		return -1;
	}

	for (int i = 0; i < n; i++) {
		double t = (double)i / (double)(n - 1);
		xs[i] = xmin + t * (xmax - xmin);
		ys[i] = ymin + t * (ymax - ymin);
	}
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			if (vec_env_set_var(e, "x", vec_value_number(vec_float(xs[i]))) != 0 ||
			    vec_env_set_var(e, "y", vec_value_number(vec_float(ys[j]))) != 0) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			double v;
			if (eval_float(e, f, &v, err, errsz) != 0)
				goto fail;
			val[(size_t)j * (size_t)n + (size_t)i] = v;
		}
	}

	double *data = NULL;
	size_t len = 0;
	size_t cap = 0;

	for (size_t li = 0; li < nlevels; li++) {
		double level = levels[li];
		for (int j = 0; j < n - 1; j++) {
			double y0 = ys[j];
			double y1 = ys[j + 1];
			for (int i = 0; i < n - 1; i++) {
				double x0 = xs[i];
				double x1 = xs[i + 1];
				double z00 = val[(size_t)j * (size_t)n + (size_t)i];
				double z10 = val[(size_t)j * (size_t)n + (size_t)i + 1];
				double z01 = val[(size_t)(j + 1) * (size_t)n + (size_t)i];
				double z11 = val[(size_t)(j + 1) * (size_t)n + (size_t)i + 1];

				int c0 = z00 > level;
				int c1 = z10 > level;
				int c2 = z11 > level;
				int c3 = z01 > level;
				int idx = 0;
					if (c0)
						idx |= 1;
					if (c1)
						idx |= 2;
					if (c2)
						idx |= 4;
					if (c3)
						idx |= 8;
					if (idx == 0 || idx == 15)
						continue;

					double ex[4];
					double ey[4];
					int ok[4];
					ok[0] = ms_interp(x0, y0, z00, x1, y0, z10, level, &ex[0], &ey[0]);
					ok[1] = ms_interp(x1, y0, z10, x1, y1, z11, level, &ex[1], &ey[1]);
					ok[2] = ms_interp(x0, y1, z01, x1, y1, z11, level, &ex[2], &ey[2]);
					ok[3] = ms_interp(x0, y0, z00, x0, y1, z01, level, &ex[3], &ey[3]);

					switch (idx) {
					case 1:
					case 14:
						if (ok[3] && ok[0] &&
						    xy_append_segment(&data, &len, &cap, ex[3], ey[3], ex[0], ey[0], err, errsz) != 0)
							goto fail;
						break;
					case 2:
					case 13:
						if (ok[0] && ok[1] &&
						    xy_append_segment(&data, &len, &cap, ex[0], ey[0], ex[1], ey[1], err, errsz) != 0)
							goto fail;
						break;
					case 3:
					case 12:
						if (ok[3] && ok[1] &&
						    xy_append_segment(&data, &len, &cap, ex[3], ey[3], ex[1], ey[1], err, errsz) != 0)
							goto fail;
						break;
					case 4:
					case 11:
						if (ok[1] && ok[2] &&
						    xy_append_segment(&data, &len, &cap, ex[1], ey[1], ex[2], ey[2], err, errsz) != 0)
							goto fail;
						break;
					case 5:
						if (ok[3] && ok[2] &&
						    xy_append_segment(&data, &len, &cap, ex[3], ey[3], ex[2], ey[2], err, errsz) != 0)
							goto fail;
						if (ok[0] && ok[1] &&
						    xy_append_segment(&data, &len, &cap, ex[0], ey[0], ex[1], ey[1], err, errsz) != 0)
							goto fail;
						break;
					case 6:
					case 9:
						if (ok[0] && ok[2] &&
						    xy_append_segment(&data, &len, &cap, ex[0], ey[0], ex[2], ey[2], err, errsz) != 0)
							goto fail;
						break;
					case 7:
					case 8:
						if (ok[3] && ok[2] &&
						    xy_append_segment(&data, &len, &cap, ex[3], ey[3], ex[2], ey[2], err, errsz) != 0)
							goto fail;
						break;
					case 10:
						if (ok[3] && ok[0] &&
						    xy_append_segment(&data, &len, &cap, ex[3], ey[3], ex[0], ey[0], err, errsz) != 0)
							goto fail;
						if (ok[1] && ok[2] &&
						    xy_append_segment(&data, &len, &cap, ex[1], ey[1], ex[2], ey[2], err, errsz) != 0)
							goto fail;
						break;
					default:
						break;
					}
				}
			}
		}

	free(xs);
	free(ys);
	free(val);
	env_restore_var(e, "y", &prev_y);
	env_restore_var(e, "x", &prev_x);

	int rows = (int)(len / 2);
	if (rows == 0) {
		free(data);
		data = malloc(sizeof(data[0]) * 2);
		if (!data) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		data[0] = NAN;
		data[1] = NAN;
		rows = 1;
	}
	*out_data = data;
	*out_rows = rows;
	return 0;

fail:
	free(xs);
	free(ys);
	free(val);
	free(data);
	env_restore_var(e, "y", &prev_y);
	env_restore_var(e, "x", &prev_x);
	return -1;
}

static vec_node *value_to_node_take(vec_value *v)
{
	if (!v)
		return NULL;
	switch (v->kind) {
	case VEC_VALUE_NUMBER:
		return vec_node_number_new(v->num);
	case VEC_VALUE_EXPR: {
		vec_node *n = v->expr;
		v->expr = NULL;
		return n;
	}
	case VEC_VALUE_COMPLEX:
		return vec_node_ident_new("<complex>", 9);
	case VEC_VALUE_ARRAY:
		return vec_node_ident_new("<array>", 7);
	case VEC_VALUE_MATRIX:
		return vec_node_ident_new("<matrix>", 8);
	default:
		return NULL;
	}
}

#ifdef VEC_LITE
static int eval_call(vec_env *e, const vec_node *n, const char *ov_name, const vec_value *ov_value,
		     vec_value *out, char *err, size_t errsz)
{
	if (!n || n->kind != VEC_NODE_CALL || !n->u.call.name) {
		snprintf(err, errsz, "eval: bad call");
		return -1;
	}

	const char *name = n->u.call.name;
	size_t argc = n->u.call.argc;
	if (argc > 8) {
		snprintf(err, errsz, "eval: too many args");
		return -1;
	}

	/* Special forms (operate on raw nodes). */
	if (!strcmp(name, "expr") && argc == 1) {
		vec_node *simp = vec_node_simplify(n->u.call.args[0]);
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "simp") && argc == 1) {
		vec_node *simp = vec_node_simplify(n->u.call.args[0]);
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}

	vec_value args[8];
	memset(args, 0, sizeof(args));
	for (size_t i = 0; i < argc; i++) {
		if (vec_eval_node_impl(e, n->u.call.args[i], ov_name, ov_value, &args[i], err, errsz) != 0) {
			for (size_t j = 0; j < i; j++)
				vec_value_destroy(&args[j]);
			return -1;
		}
		if (args[i].kind != VEC_VALUE_NUMBER) {
			for (size_t j = 0; j <= i; j++)
				vec_value_destroy(&args[j]);
			snprintf(err, errsz, "eval: %s expects numbers", name);
			return -1;
		}
	}

	double a0 = (argc >= 1) ? vec_number_float64(args[0].num) : 0.0;
	double a1 = (argc >= 2) ? vec_number_float64(args[1].num) : 0.0;
	double a2 = (argc >= 3) ? vec_number_float64(args[2].num) : 0.0;

	if (!strcmp(name, "sin") && argc == 1)
		*out = vec_value_number(vec_float(sin(a0)));
	else if (!strcmp(name, "cos") && argc == 1)
		*out = vec_value_number(vec_float(cos(a0)));
	else if (!strcmp(name, "tan") && argc == 1)
		*out = vec_value_number(vec_float(tan(a0)));
	else if (!strcmp(name, "asin") && argc == 1)
		*out = vec_value_number(vec_float(asin(a0)));
	else if (!strcmp(name, "acos") && argc == 1)
		*out = vec_value_number(vec_float(acos(a0)));
	else if (!strcmp(name, "atan") && argc == 1)
		*out = vec_value_number(vec_float(atan(a0)));
	else if (!strcmp(name, "atan2") && argc == 2)
		*out = vec_value_number(vec_float(atan2(a0, a1)));
	else if (!strcmp(name, "sqrt") && argc == 1)
		*out = vec_value_number(vec_float(sqrt(a0)));
	else if (!strcmp(name, "abs") && argc == 1)
		*out = vec_value_number(vec_float(fabs(a0)));
	else if (!strcmp(name, "floor") && argc == 1)
		*out = vec_value_number(vec_float(floor(a0)));
	else if (!strcmp(name, "ceil") && argc == 1)
		*out = vec_value_number(vec_float(ceil(a0)));
	else if (!strcmp(name, "exp") && argc == 1)
		*out = vec_value_number(vec_float(exp(a0)));
	else if (!strcmp(name, "log") && argc == 1)
		*out = vec_value_number(vec_float(log(a0)));
	else if (!strcmp(name, "log10") && argc == 1)
		*out = vec_value_number(vec_float(log10(a0)));
	else if (!strcmp(name, "pow") && argc == 2)
		*out = vec_value_number(vec_float(pow(a0, a1)));
	else if (!strcmp(name, "min") && argc == 2)
		*out = vec_value_number(vec_float(a0 < a1 ? a0 : a1));
	else if (!strcmp(name, "max") && argc == 2)
		*out = vec_value_number(vec_float(a0 > a1 ? a0 : a1));
	else if (!strcmp(name, "clamp") && argc == 3) {
		double v = a0;
		if (v < a1)
			v = a1;
		if (v > a2)
			v = a2;
		*out = vec_value_number(vec_float(v));
	} else {
		/* User-defined single-arg functions: f(x)=... */
		const vec_userfunc *uf;
		if (vec_env_get_func(e, name, &uf) == 0 && uf && uf->body && uf->param) {
			if (argc != 1) {
				snprintf(err, errsz, "eval: %s expects 1 argument", name);
				for (size_t i = 0; i < argc; i++)
					vec_value_destroy(&args[i]);
				return -1;
			}
			int rc = vec_eval_node_impl(e, uf->body, uf->param, &args[0], out, err, errsz);
			for (size_t i = 0; i < argc; i++)
				vec_value_destroy(&args[i]);
			return rc;
		}

		snprintf(err, errsz, "eval: unknown function '%s'", name);
		for (size_t i = 0; i < argc; i++)
			vec_value_destroy(&args[i]);
		return -1;
	}

	for (size_t i = 0; i < argc; i++)
		vec_value_destroy(&args[i]);
	return 0;
}
#else
static int eval_call(vec_env *e, const vec_node *n, const char *ov_name, const vec_value *ov_value,
		     vec_value *out, char *err, size_t errsz)
{
	if (!n || n->kind != VEC_NODE_CALL || !n->u.call.name) {
		snprintf(err, errsz, "eval: bad call");
		return -1;
	}

	const char *name = n->u.call.name;
	size_t argc = n->u.call.argc;
	if (argc > 32) {
		snprintf(err, errsz, "eval: too many args");
		return -1;
	}

	/* Special forms (operate on raw nodes). */
	if (!strcmp(name, "expr") && argc == 1) {
		vec_node *simp = vec_node_simplify(n->u.call.args[0]);
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "simp") && argc == 1) {
		vec_node *simp = vec_node_simplify(n->u.call.args[0]);
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "param") && (argc == 4 || argc == 5)) {
		const vec_node *xexpr = n->u.call.args[0];
		const vec_node *yexpr = n->u.call.args[1];

		vec_value tminv;
		memset(&tminv, 0, sizeof(tminv));
		if (vec_eval_node_impl(e, n->u.call.args[2], ov_name, ov_value, &tminv, err, errsz) != 0)
			return -1;
		vec_value tmaxv;
		memset(&tmaxv, 0, sizeof(tmaxv));
		if (vec_eval_node_impl(e, n->u.call.args[3], ov_name, ov_value, &tmaxv, err, errsz) != 0) {
			vec_value_destroy(&tminv);
			return -1;
		}
		if (tminv.kind != VEC_VALUE_NUMBER || tmaxv.kind != VEC_VALUE_NUMBER) {
			vec_value_destroy(&tminv);
			vec_value_destroy(&tmaxv);
			snprintf(err, errsz, "eval: param expects numeric t bounds");
			return -1;
		}
		double t_min = vec_number_float64(tminv.num);
		double t_max = vec_number_float64(tmaxv.num);
		vec_value_destroy(&tminv);
		vec_value_destroy(&tmaxv);
		if (isnan(t_min) || isnan(t_max) || isinf(t_min) || isinf(t_max)) {
			snprintf(err, errsz, "eval: param invalid t bounds");
			return -1;
		}

		int npoints = 256;
		if (argc == 5) {
			vec_value nv;
			memset(&nv, 0, sizeof(nv));
			if (vec_eval_node_impl(e, n->u.call.args[4], ov_name, ov_value, &nv, err, errsz) != 0)
				return -1;
			if (nv.kind != VEC_VALUE_NUMBER) {
				vec_value_destroy(&nv);
				snprintf(err, errsz, "eval: param expects numeric point count");
				return -1;
			}
			double nf = vec_number_float64(nv.num);
			vec_value_destroy(&nv);
			if (nf < 2 || nf > 4096) {
				snprintf(err, errsz, "eval: param point count must be 2..4096");
				return -1;
			}
			npoints = (int)nf;
		}

		struct env_saved_var prev_t = {0};
		if (env_save_var(e, "t", &prev_t, err, errsz) != 0)
			return -1;

		double *data = NULL;
		if (npoints > 0) {
			size_t want = (size_t)npoints * 2;
			data = malloc(sizeof(data[0]) * want);
			if (!data) {
				env_restore_var(e, "t", &prev_t);
				snprintf(err, errsz, "eval: out of memory");
				return -1;
			}
		}

		for (int i = 0; i < npoints; i++) {
			double tt = t_min + (double)i * (t_max - t_min) / (double)(npoints - 1);
			if (vec_env_set_var(e, "t", vec_value_number(vec_float(tt))) != 0) {
				snprintf(err, errsz, "eval: out of memory");
				goto param_fail;
			}
			vec_value xv;
			memset(&xv, 0, sizeof(xv));
			if (vec_eval_node_impl(e, xexpr, ov_name, ov_value, &xv, err, errsz) != 0) {
				vec_value_destroy(&xv);
				goto param_fail;
			}
			vec_value yv;
			memset(&yv, 0, sizeof(yv));
			if (vec_eval_node_impl(e, yexpr, ov_name, ov_value, &yv, err, errsz) != 0) {
				vec_value_destroy(&xv);
				goto param_fail;
			}
			if (xv.kind != VEC_VALUE_NUMBER || yv.kind != VEC_VALUE_NUMBER) {
				vec_value_destroy(&xv);
				vec_value_destroy(&yv);
				snprintf(err, errsz, "eval: param expects x(t), y(t) to be numeric");
				goto param_fail;
			}
			data[i * 2 + 0] = vec_number_float64(xv.num);
			data[i * 2 + 1] = vec_number_float64(yv.num);
			vec_value_destroy(&xv);
			vec_value_destroy(&yv);
		}

		env_restore_var(e, "t", &prev_t);
		*out = vec_value_matrix(npoints, 2, data);
		return 0;

	param_fail:
		free(data);
		env_restore_var(e, "t", &prev_t);
		return -1;
	}
	if (!strcmp(name, "expand") && argc == 1) {
		vec_node *ex = vec_node_expand(n->u.call.args[0]);
		vec_node *simp = ex ? vec_node_simplify_owned(ex) : NULL;
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "horner") && argc == 2) {
		const vec_node *v = n->u.call.args[1];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: horner expects second arg as identifier");
			return -1;
		}
		char pbuf[96];
		pbuf[0] = 0;
		vec_poly p = {0};
		if (vec_poly_from_expr(e, n->u.call.args[0], v->u.ident.name, &p, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: horner: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_node *tmp = vec_poly_to_expr_horner(&p, v->u.ident.name);
		vec_poly_destroy(&p);
		vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "degree") && argc == 2) {
		const vec_node *v = n->u.call.args[1];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: degree expects second arg as identifier");
			return -1;
		}
		char pbuf[96];
		pbuf[0] = 0;
		vec_poly p = {0};
		if (vec_poly_from_expr(e, n->u.call.args[0], v->u.ident.name, &p, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: degree: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		int deg = vec_poly_degree(&p);
		vec_poly_destroy(&p);
		*out = vec_value_number(vec_rat_number(vec_rat_int((int64_t)deg)));
		return 0;
	}
	if (!strcmp(name, "coeff") && argc == 3) {
		const vec_node *v = n->u.call.args[1];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: coeff expects second arg as identifier");
			return -1;
		}
		vec_value nv;
		memset(&nv, 0, sizeof(nv));
		if (vec_eval_node_impl(e, n->u.call.args[2], ov_name, ov_value, &nv, err, errsz) != 0)
			return -1;
		if (nv.kind != VEC_VALUE_NUMBER) {
			vec_value_destroy(&nv);
			snprintf(err, errsz, "eval: coeff expects non-negative integer n");
			return -1;
		}
		double nn = vec_number_float64(nv.num);
		vec_value_destroy(&nv);
		if (isnan(nn) || isinf(nn) || nn != trunc_d(nn) || nn < 0) {
			snprintf(err, errsz, "eval: coeff expects non-negative integer n");
			return -1;
		}
		int ncoef = (int)nn;

		char pbuf[96];
		pbuf[0] = 0;
		vec_poly p = {0};
		if (vec_poly_from_expr(e, n->u.call.args[0], v->u.ident.name, &p, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: coeff: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		double c = 0;
		if ((size_t)ncoef < p.len)
			c = p.coeffs[ncoef];
		vec_poly_destroy(&p);
		*out = vec_value_number(vec_float(c));
		return 0;
	}
	if (!strcmp(name, "collect") && argc == 2) {
		const vec_node *v = n->u.call.args[1];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: collect expects second arg as identifier");
			return -1;
		}
		char pbuf[96];
		pbuf[0] = 0;
		vec_poly p = {0};
		if (vec_poly_from_expr(e, n->u.call.args[0], v->u.ident.name, &p, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: collect: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_node *tmp = vec_poly_to_expr_horner(&p, v->u.ident.name);
		vec_poly_destroy(&p);
		vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "gcd") && (argc == 2 || argc == 3)) {
		const char *var_name = "x";
		if (argc == 3) {
			const vec_node *v = n->u.call.args[2];
			if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
				snprintf(err, errsz, "eval: gcd expects third arg as identifier");
				return -1;
			}
			var_name = v->u.ident.name;
		}

		char pbuf[96];
		pbuf[0] = 0;
		vec_poly_rat a = {0};
		vec_poly_rat b = {0};
		vec_poly_rat g = {0};
		if (vec_poly_rat_from_expr(e, n->u.call.args[0], var_name, &a, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: gcd: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_from_expr(e, n->u.call.args[1], var_name, &b, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			snprintf(err, errsz, "eval: gcd: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_gcd(&a, &b, &g, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			vec_poly_rat_destroy(&b);
			snprintf(err, errsz, "eval: gcd: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_poly_rat_destroy(&a);
		vec_poly_rat_destroy(&b);
		vec_node *tmp = vec_poly_rat_to_expr_horner(&g, var_name);
		vec_poly_rat_destroy(&g);
		vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "lcm") && (argc == 2 || argc == 3)) {
		const char *var_name = "x";
		if (argc == 3) {
			const vec_node *v = n->u.call.args[2];
			if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
				snprintf(err, errsz, "eval: lcm expects third arg as identifier");
				return -1;
			}
			var_name = v->u.ident.name;
		}

		char pbuf[96];
		pbuf[0] = 0;
		vec_poly_rat a = {0};
		vec_poly_rat b = {0};
		vec_poly_rat g = {0};
		vec_poly_rat ab = {0};
		vec_poly_rat q = {0};
		vec_poly_rat r = {0};
		if (vec_poly_rat_from_expr(e, n->u.call.args[0], var_name, &a, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: lcm: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_from_expr(e, n->u.call.args[1], var_name, &b, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			snprintf(err, errsz, "eval: lcm: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_gcd(&a, &b, &g, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			vec_poly_rat_destroy(&b);
			snprintf(err, errsz, "eval: lcm: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_mul(&a, &b, &ab, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			vec_poly_rat_destroy(&b);
			vec_poly_rat_destroy(&g);
			snprintf(err, errsz, "eval: lcm: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_poly_rat_destroy(&a);
		vec_poly_rat_destroy(&b);
		pbuf[0] = 0;
		if (vec_poly_rat_divmod(&ab, &g, &q, &r, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&ab);
			vec_poly_rat_destroy(&g);
			snprintf(err, errsz, "eval: lcm: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_poly_rat_destroy(&ab);
		vec_poly_rat_destroy(&g);
		if (vec_poly_rat_degree(&r) >= 0) {
			vec_poly_rat_destroy(&q);
			vec_poly_rat_destroy(&r);
			snprintf(err, errsz, "eval: lcm: non-exact division");
			return -1;
		}
		vec_poly_rat_destroy(&r);
		pbuf[0] = 0;
		if (vec_poly_rat_monic(&q, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&q);
			snprintf(err, errsz, "eval: lcm: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_node *tmp = vec_poly_rat_to_expr_horner(&q, var_name);
		vec_poly_rat_destroy(&q);
		vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "resultant") && argc == 3) {
		const vec_node *v = n->u.call.args[2];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: resultant expects third arg as identifier");
			return -1;
		}

		char pbuf[96];
		pbuf[0] = 0;
		vec_poly_rat a = {0};
		vec_poly_rat b = {0};
		vec_rat res = vec_rat_int(0);
		if (vec_poly_rat_from_expr(e, n->u.call.args[0], v->u.ident.name, &a, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: resultant: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_from_expr(e, n->u.call.args[1], v->u.ident.name, &b, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			snprintf(err, errsz, "eval: resultant: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		if (vec_poly_rat_resultant(&a, &b, &res, pbuf, sizeof(pbuf)) != 0) {
			vec_poly_rat_destroy(&a);
			vec_poly_rat_destroy(&b);
			snprintf(err, errsz, "eval: resultant: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		vec_poly_rat_destroy(&a);
		vec_poly_rat_destroy(&b);
		if (e->mode == VEC_MODE_EXACT)
			*out = vec_value_number(vec_rat_number(res));
		else
			*out = vec_value_number(vec_float(vec_rat_float64(res)));
		return 0;
	}
	if (!strcmp(name, "factor") && (argc == 1 || argc == 2)) {
		const char *var_name = "x";
		if (argc == 2) {
			const vec_node *v = n->u.call.args[1];
			if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
				snprintf(err, errsz, "eval: factor expects second arg as identifier");
				return -1;
			}
			var_name = v->u.ident.name;
		}

		char pbuf[96];
		pbuf[0] = 0;
		vec_poly_rat p = {0};
		if (vec_poly_rat_from_expr(e, n->u.call.args[0], var_name, &p, pbuf, sizeof(pbuf)) != 0) {
			snprintf(err, errsz, "eval: factor: %s", pbuf[0] ? pbuf : "invalid polynomial");
			return -1;
		}
		pbuf[0] = 0;
		vec_node *tmp = vec_poly_rat_factor_integer(&p, var_name, pbuf, sizeof(pbuf));
		vec_poly_rat_destroy(&p);
		if (!tmp) {
			snprintf(err, errsz, "eval: factor: %s", pbuf[0] ? pbuf : "out of memory");
			return -1;
		}
		vec_node *simp = vec_node_simplify_owned(tmp);
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}
	if (!strcmp(name, "series") && argc == 4) {
		const vec_node *v = n->u.call.args[1];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: series expects second arg as identifier");
			return -1;
		}
		vec_value av;
		memset(&av, 0, sizeof(av));
		if (vec_eval_node_impl(e, n->u.call.args[2], ov_name, ov_value, &av, err, errsz) != 0)
			return -1;
		if (av.kind != VEC_VALUE_NUMBER) {
			vec_value_destroy(&av);
			snprintf(err, errsz, "eval: series expects numeric a");
			return -1;
		}
		double a = vec_number_float64(av.num);
		vec_value_destroy(&av);

		vec_value nv;
		memset(&nv, 0, sizeof(nv));
		if (vec_eval_node_impl(e, n->u.call.args[3], ov_name, ov_value, &nv, err, errsz) != 0)
			return -1;
		if (nv.kind != VEC_VALUE_NUMBER) {
			vec_value_destroy(&nv);
			snprintf(err, errsz, "eval: series expects n in 0..64");
			return -1;
		}
		double nn = vec_number_float64(nv.num);
		vec_value_destroy(&nv);
		if (isnan(nn) || isinf(nn) || nn != trunc_d(nn) || nn < 0 || nn > 64) {
			snprintf(err, errsz, "eval: series expects n in 0..64");
			return -1;
		}
		int nterms = (int)nn;

		vec_node *series = vec_node_taylor_series(e, n->u.call.args[0], v->u.ident.name, a, nterms, err, errsz);
		if (!series)
			return -1;
		*out = vec_value_expr(series);
		return 0;
	}
	if (!strcmp(name, "diff") && argc == 2) {
		const vec_node *v = n->u.call.args[1];
		if (!v || v->kind != VEC_NODE_IDENT || !v->u.ident.name) {
			snprintf(err, errsz, "eval: diff expects second arg as identifier");
			return -1;
		}
		vec_node *d = vec_node_deriv(n->u.call.args[0], v->u.ident.name);
		if (!d) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		vec_node *simp = vec_node_simplify_owned(d);
		if (!simp) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
		*out = vec_value_expr(simp);
		return 0;
	}

	vec_value *args = NULL;
	int rc = 0;
	if (argc) {
		args = calloc(argc, sizeof(args[0]));
		if (!args) {
			snprintf(err, errsz, "eval: out of memory");
			return -1;
		}
	}

	for (size_t i = 0; i < argc; i++) {
		if (vec_eval_node_impl(e, n->u.call.args[i], ov_name, ov_value, &args[i], err, errsz) != 0) {
			for (size_t j = 0; j < argc; j++)
				vec_value_destroy(&args[j]);
			free(args);
			return -1;
		}
	}

	/* Control builtins (operate on evaluated values). */
	if (!strcmp(name, "eval")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_EXPR) {
			snprintf(err, errsz, "eval: eval(expr)");
			goto fail;
		}
		rc = vec_eval_node_impl(e, args[0].expr, ov_name, ov_value, out, err, errsz);
		goto done_rc;
	}
	if (!strcmp(name, "if")) {
		if (argc != 3) {
			snprintf(err, errsz, "eval: if(cond, a, b)");
			goto fail;
		}
		int cond;
		if (value_truthy(&args[0], &cond) != 0) {
			snprintf(err, errsz, "eval: condition must be a number");
			goto fail;
		}
		if (cond) {
			*out = args[1];
			args[1] = vec_value_number(vec_float(0));
		} else {
			*out = args[2];
			args[2] = vec_value_number(vec_float(0));
		}
		goto done;
	}
	if (!strcmp(name, "size")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_EXPR) {
			snprintf(err, errsz, "eval: size(expr)");
			goto fail;
		}
		*out = vec_value_number(vec_rat_number(vec_rat_int((int64_t)vec_node_size(args[0].expr))));
		goto done;
	}
	if (!strcmp(name, "numeric")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_EXPR) {
			snprintf(err, errsz, "eval: numeric(expr)");
			goto fail;
		}
		vec_mode prev = e->mode;
		e->mode = VEC_MODE_FLOAT;
		rc = vec_eval_node_impl(e, args[0].expr, ov_name, ov_value, out, err, errsz);
		e->mode = prev;
		goto done_rc;
	}
	if (!strcmp(name, "exact")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_EXPR) {
			snprintf(err, errsz, "eval: exact(expr)");
			goto fail;
		}
		vec_mode prev = e->mode;
		e->mode = VEC_MODE_EXACT;
		rc = vec_eval_node_impl(e, args[0].expr, ov_name, ov_value, out, err, errsz);
		e->mode = prev;
		goto done_rc;
	}
	if (!strcmp(name, "time")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_EXPR) {
			snprintf(err, errsz, "eval: time(expr)");
			goto fail;
		}
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		rc = vec_eval_node_impl(e, args[0].expr, ov_name, ov_value, out, err, errsz);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0;
		ms += (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
		(void)vec_env_set_var(e, "_time_ms", vec_value_number(vec_float(ms)));
		goto done_rc;
	}
	if (!strcmp(name, "where")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: where(cond, value)");
			goto fail;
		}
		int cond;
		if (value_truthy(&args[0], &cond) != 0) {
			snprintf(err, errsz, "eval: condition must be a number");
			goto fail;
		}
		if (cond) {
			*out = args[1];
			args[1] = vec_value_number(vec_float(0));
		} else {
			*out = vec_value_number(vec_float(NAN));
		}
		goto done;
	}
	if (!strcmp(name, "and") && argc == 2) {
		int a, b;
		if (value_truthy(&args[0], &a) != 0 || value_truthy(&args[1], &b) != 0) {
			snprintf(err, errsz, "eval: condition must be a number");
			goto fail;
		}
		*out = vec_value_number(vec_rat_number(vec_rat_int((a && b) ? 1 : 0)));
		goto done;
	}
	if (!strcmp(name, "or") && argc == 2) {
		int a, b;
		if (value_truthy(&args[0], &a) != 0 || value_truthy(&args[1], &b) != 0) {
			snprintf(err, errsz, "eval: condition must be a number");
			goto fail;
		}
		*out = vec_value_number(vec_rat_number(vec_rat_int((a || b) ? 1 : 0)));
		goto done;
	}
		if (!strcmp(name, "not") && argc == 1) {
			int a;
			if (value_truthy(&args[0], &a) != 0) {
				snprintf(err, errsz, "eval: condition must be a number");
			goto fail;
		}
		*out = vec_value_number(vec_rat_number(vec_rat_int(a ? 0 : 1)));
			goto done;
		}

		/* Numeric analysis builtins. */
			if (!strcmp(name, "newton")) {
				if (argc < 2 || argc > 4) {
					snprintf(err, errsz, "eval: newton(expr, x0[, tol[, maxIter]])");
					goto fail;
				}
			if (args[0].kind != VEC_VALUE_EXPR || args[1].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: newton(expr, x0[, tol[, maxIter]])");
				goto fail;
			}
			double x0 = vec_number_float64(args[1].num);
			double tol = 1e-9;
			if (argc >= 3) {
				if (args[2].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: newton(expr, x0[, tol[, maxIter]])");
					goto fail;
				}
				double v = vec_number_float64(args[2].num);
				if (v > 0 && !isinf(v) && !isnan(v))
					tol = v;
			}
			int max_iter = 32;
			if (argc == 4) {
				if (args[3].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: newton(expr, x0[, tol[, maxIter]])");
					goto fail;
				}
				double it = vec_number_float64(args[3].num);
				if (isnan(it) || isinf(it) || it != trunc_d(it)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (it >= 1 && it <= 512)
					max_iter = (int)it;
			}

				double root;
				if (vec_numeric_solve1_newton(e, args[0].expr, x0, tol, max_iter, &root, err, errsz) != 0)
					goto fail;
				*out = vec_value_number(vec_float(root));
				goto done;
			}

			if (!strcmp(name, "solve1")) {
					if (argc < 2 || argc > 4) {
						snprintf(err, errsz, "eval: solve1(expr, x0[, tol[, maxIter]])");
						goto fail;
					}
				if (args[0].kind != VEC_VALUE_EXPR || args[1].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: solve1(expr, x0[, tol[, maxIter]])");
					goto fail;
				}
				double x0 = vec_number_float64(args[1].num);
				double tol = 1e-9;
				if (argc >= 3) {
					if (args[2].kind != VEC_VALUE_NUMBER) {
						snprintf(err, errsz, "eval: solve1(expr, x0[, tol[, maxIter]])");
						goto fail;
					}
					double v = vec_number_float64(args[2].num);
					if (v > 0 && !isinf(v) && !isnan(v))
						tol = v;
				}
				int max_iter = 32;
				if (argc == 4) {
					if (args[3].kind != VEC_VALUE_NUMBER) {
						snprintf(err, errsz, "eval: solve1(expr, x0[, tol[, maxIter]])");
						goto fail;
					}
					double it = vec_number_float64(args[3].num);
					if (isnan(it) || isinf(it) || it != trunc_d(it)) {
						snprintf(err, errsz, "eval: expected integer");
						goto fail;
					}
					if (it >= 1 && it <= 512)
						max_iter = (int)it;
				}
				double root;
				if (vec_numeric_solve1_newton(e, args[0].expr, x0, tol, max_iter, &root, err, errsz) != 0)
					goto fail;
				*out = vec_value_number(vec_float(root));
				goto done;
			}

			if (!strcmp(name, "solve2")) {
				if (argc < 4 || argc > 6) {
					snprintf(err, errsz, "eval: solve2(f, g, x0, y0[, tol[, maxIter]])");
					goto fail;
				}
				if (args[0].kind != VEC_VALUE_EXPR ||
				    args[1].kind != VEC_VALUE_EXPR ||
				    args[2].kind != VEC_VALUE_NUMBER ||
				    args[3].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: solve2(f, g, x0, y0[, tol[, maxIter]])");
					goto fail;
				}
				double x0 = vec_number_float64(args[2].num);
				double y0 = vec_number_float64(args[3].num);
				double tol = 1e-9;
				if (argc >= 5) {
					if (args[4].kind != VEC_VALUE_NUMBER) {
						snprintf(err, errsz, "eval: solve2(f, g, x0, y0[, tol[, maxIter]])");
						goto fail;
					}
					double v = vec_number_float64(args[4].num);
					if (v > 0 && !isinf(v) && !isnan(v))
						tol = v;
				}
				int max_iter = 32;
				if (argc == 6) {
					if (args[5].kind != VEC_VALUE_NUMBER) {
						snprintf(err, errsz, "eval: solve2(f, g, x0, y0[, tol[, maxIter]])");
						goto fail;
					}
					double it = vec_number_float64(args[5].num);
					if (isnan(it) || isinf(it) || it != trunc_d(it)) {
						snprintf(err, errsz, "eval: expected integer");
						goto fail;
					}
					if (it >= 1 && it <= 512)
						max_iter = (int)it;
				}

				struct env_saved_var prev_x = {0};
				struct env_saved_var prev_y = {0};
				if (env_save_var(e, "x", &prev_x, err, errsz) != 0)
					goto fail;
				if (env_save_var(e, "y", &prev_y, err, errsz) != 0) {
					vec_value_destroy(&prev_x.v);
					goto fail;
				}

				double x = x0;
				double y = y0;
				int converged = 0;

				for (int iter = 0; iter < max_iter; iter++) {
					if (vec_env_set_var(e, "x", vec_value_number(vec_float(x))) != 0 ||
					    vec_env_set_var(e, "y", vec_value_number(vec_float(y))) != 0) {
						snprintf(err, errsz, "eval: out of memory");
						goto solve2_fail;
					}

					double fv, gv;
					if (eval_float(e, args[0].expr, &fv, err, errsz) != 0 ||
					    eval_float(e, args[1].expr, &gv, err, errsz) != 0)
						goto solve2_fail;
					if (fabs(fv) <= tol && fabs(gv) <= tol) {
						converged = 1;
						break;
					}

					double hx = 1e-6 * (1 + fabs(x));
					double hy = 1e-6 * (1 + fabs(y));

					if (vec_env_set_var(e, "x", vec_value_number(vec_float(x + hx))) != 0 ||
					    vec_env_set_var(e, "y", vec_value_number(vec_float(y))) != 0) {
						snprintf(err, errsz, "eval: out of memory");
						goto solve2_fail;
					}
					double fxph, gxph;
					if (eval_float(e, args[0].expr, &fxph, err, errsz) != 0 ||
					    eval_float(e, args[1].expr, &gxph, err, errsz) != 0)
						goto solve2_fail;
					double dfdx = (fxph - fv) / hx;
					double dgdx = (gxph - gv) / hx;

					if (vec_env_set_var(e, "x", vec_value_number(vec_float(x))) != 0 ||
					    vec_env_set_var(e, "y", vec_value_number(vec_float(y + hy))) != 0) {
						snprintf(err, errsz, "eval: out of memory");
						goto solve2_fail;
					}
					double fyph, gyph;
					if (eval_float(e, args[0].expr, &fyph, err, errsz) != 0 ||
					    eval_float(e, args[1].expr, &gyph, err, errsz) != 0)
						goto solve2_fail;
					double dfdy = (fyph - fv) / hy;
					double dgdy = (gyph - gv) / hy;

					double det = dfdx * dgdy - dfdy * dgdx;
					if (det == 0 || isnan(det) || isinf(det)) {
						snprintf(err, errsz, "eval: solve2 singular Jacobian");
						goto solve2_fail;
					}

					double dx = (-fv * dgdy + gv * dfdy) / det;
					double dy = (dgdx * fv - dfdx * gv) / det;

					double x_next = x + dx;
					double y_next = y + dy;
					if (fabs(dx) <= tol * (1 + fabs(x)) && fabs(dy) <= tol * (1 + fabs(y))) {
						x = x_next;
						y = y_next;
						converged = 1;
						break;
					}
					x = x_next;
					y = y_next;
				}

				if (!converged) {
					snprintf(err, errsz, "eval: solve2 did not converge");
					goto solve2_fail;
				}

				double *xy = malloc(sizeof(xy[0]) * 2);
				if (!xy) {
					snprintf(err, errsz, "eval: out of memory");
					goto solve2_fail;
				}
				xy[0] = x;
				xy[1] = y;
				env_restore_var(e, "y", &prev_y);
				env_restore_var(e, "x", &prev_x);
				*out = vec_value_array(xy, 2);
				goto done;

			solve2_fail:
				env_restore_var(e, "y", &prev_y);
				env_restore_var(e, "x", &prev_x);
				goto fail;
			}

			if (!strcmp(name, "bisection")) {
				if (argc < 3 || argc > 5) {
					snprintf(err, errsz, "eval: bisection(expr, a, b[, tol[, maxIter]])");
					goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_NUMBER ||
			    args[2].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: bisection(expr, a, b[, tol[, maxIter]])");
				goto fail;
			}
			double a = vec_number_float64(args[1].num);
			double b = vec_number_float64(args[2].num);
			if (a >= b) {
				snprintf(err, errsz, "eval: bisection expects a < b");
				goto fail;
			}
			double tol = 1e-9;
			if (argc >= 4) {
				if (args[3].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: bisection(expr, a, b[, tol[, maxIter]])");
					goto fail;
				}
				double v = vec_number_float64(args[3].num);
				if (v > 0 && !isinf(v) && !isnan(v))
					tol = v;
			}
			int max_iter = 64;
			if (argc == 5) {
				if (args[4].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: bisection(expr, a, b[, tol[, maxIter]])");
					goto fail;
				}
				double it = vec_number_float64(args[4].num);
				if (isnan(it) || isinf(it) || it != trunc_d(it)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (it >= 1 && it <= 2048)
					max_iter = (int)it;
			}

			double root;
			if (vec_numeric_bisection_root(e, args[0].expr, a, b, tol, max_iter, &root, err, errsz) != 0)
				goto fail;
			*out = vec_value_number(vec_float(root));
			goto done;
		}

		if (!strcmp(name, "secant")) {
			if (argc < 3 || argc > 5) {
				snprintf(err, errsz, "eval: secant(expr, x0, x1[, tol[, maxIter]])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_NUMBER ||
			    args[2].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: secant(expr, x0, x1[, tol[, maxIter]])");
				goto fail;
			}
			double x0 = vec_number_float64(args[1].num);
			double x1 = vec_number_float64(args[2].num);
			double tol = 1e-9;
			if (argc >= 4) {
				if (args[3].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: secant(expr, x0, x1[, tol[, maxIter]])");
					goto fail;
				}
				double v = vec_number_float64(args[3].num);
				if (v > 0 && !isinf(v) && !isnan(v))
					tol = v;
			}
			int max_iter = 64;
			if (argc == 5) {
				if (args[4].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: secant(expr, x0, x1[, tol[, maxIter]])");
					goto fail;
				}
				double it = vec_number_float64(args[4].num);
				if (isnan(it) || isinf(it) || it != trunc_d(it)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (it >= 1 && it <= 2048)
					max_iter = (int)it;
			}

			double root;
			if (vec_numeric_secant_root(e, args[0].expr, x0, x1, tol, max_iter, &root, err, errsz) != 0)
				goto fail;
			*out = vec_value_number(vec_float(root));
			goto done;
		}

		if (!strcmp(name, "diff_num")) {
			if (argc < 2 || argc > 3) {
				snprintf(err, errsz, "eval: diff_num(expr, x[, h])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR || args[1].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: diff_num(expr, x[, h])");
				goto fail;
			}
			double x = vec_number_float64(args[1].num);
			double h = 1e-6 * (1 + fabs(x));
			if (argc == 3) {
				if (args[2].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: diff_num(expr, x[, h])");
					goto fail;
				}
				double v = vec_number_float64(args[2].num);
				if (v > 0 && !isinf(v) && !isnan(v))
					h = v;
			}
			double d;
			if (vec_numeric_diff_central(e, args[0].expr, x, h, &d, err, errsz) != 0)
				goto fail;
			*out = vec_value_number(vec_float(d));
			goto done;
		}

		if (!strcmp(name, "integrate_num")) {
			if (argc < 3 || argc > 5) {
				snprintf(err, errsz, "eval: integrate_num(expr, a, b[, method[, n]])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_NUMBER ||
			    args[2].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: integrate_num(expr, a, b[, method[, n]])");
				goto fail;
			}
			double a = vec_number_float64(args[1].num);
			double b = vec_number_float64(args[2].num);
			if (a == b) {
				*out = vec_value_number(vec_float(0));
				goto done;
			}

			int method = 0;
			int n = 1024;
			if (argc >= 4) {
				if (args[3].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: integrate_num(expr, a, b[, method[, n]])");
					goto fail;
				}
				double v = vec_number_float64(args[3].num);
				if (isnan(v) || isinf(v) || v != trunc_d(v)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (v > 1) {
					if (v < 2 || v > 1000000) {
						snprintf(err, errsz, "eval: integrate_num n must be 2..1000000");
						goto fail;
					}
					n = (int)v;
				} else {
					method = (int)v;
				}
			}
			if (argc == 5) {
				if (args[4].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: integrate_num(expr, a, b[, method[, n]])");
					goto fail;
				}
				double v = vec_number_float64(args[4].num);
				if (isnan(v) || isinf(v) || v != trunc_d(v)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (v < 2 || v > 1000000) {
					snprintf(err, errsz, "eval: integrate_num n must be 2..1000000");
					goto fail;
				}
				n = (int)v;
			}
			if (method < 0 || method > 1) {
				snprintf(err, errsz, "eval: integrate_num method must be 0..1");
				goto fail;
			}
			if (n < 2 || n > 1000000) {
				snprintf(err, errsz, "eval: integrate_num n must be 2..1000000");
				goto fail;
			}

			double result;
			switch (method) {
			case 0:
				if (vec_numeric_integrate_trapezoid(e, args[0].expr, a, b, n, &result, err, errsz) != 0)
					goto fail;
				break;
			case 1:
				if (vec_numeric_integrate_simpson(e, args[0].expr, a, b, n, &result, err, errsz) != 0)
					goto fail;
				break;
			default:
				snprintf(err, errsz, "eval: integrate_num method must be 0..1");
				goto fail;
			}
			*out = vec_value_number(vec_float(result));
			goto done;
		}

		if (!strcmp(name, "interp")) {
			if (argc != 2) {
				snprintf(err, errsz, "eval: interp(data, x)");
				goto fail;
			}
			if (args[1].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: interp(data, x)");
				goto fail;
			}
			double x = vec_number_float64(args[1].num);
			double y;
			char ibuf[96];
			ibuf[0] = 0;
			if (vec_numeric_interp1(&args[0], x, &y, ibuf, sizeof(ibuf)) != 0) {
				snprintf(err, errsz, "eval: interp: %s", ibuf[0] ? ibuf : "invalid data");
				goto fail;
			}
			*out = vec_value_number(vec_float(y));
			goto done;
		}

		if (!strcmp(name, "roots") && !(argc == 1 && args[0].kind == VEC_VALUE_ARRAY)) {
			if (argc < 3 || argc > 4) {
				snprintf(err, errsz, "eval: roots(expr, xmin, xmax[, n])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_NUMBER ||
			    args[2].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: roots(expr, xmin, xmax[, n])");
				goto fail;
			}
			double x_min = vec_number_float64(args[1].num);
			double x_max = vec_number_float64(args[2].num);
			if (x_min >= x_max) {
				snprintf(err, errsz, "eval: roots expects xmin < xmax");
				goto fail;
			}
			int n = 256;
			if (argc == 4) {
				if (args[3].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: roots(expr, xmin, xmax[, n])");
					goto fail;
				}
				double nn = vec_number_float64(args[3].num);
				if (isnan(nn) || isinf(nn) || nn != trunc_d(nn)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (nn < 8 || nn > 4096) {
					snprintf(err, errsz, "eval: roots n must be 8..4096");
					goto fail;
				}
				n = (int)nn;
			}

			double *roots = NULL;
			size_t nroots = 0;
			if (vec_numeric_roots_scan_bisection(e, args[0].expr, x_min, x_max, n, &roots, &nroots, err, errsz) != 0)
				goto fail;
			*out = vec_value_array(roots, nroots);
			goto done;
		}

		if (!strcmp(name, "region")) {
			if (argc < 5 || argc > 6) {
				snprintf(err, errsz, "eval: region(cond, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_NUMBER ||
			    args[2].kind != VEC_VALUE_NUMBER ||
			    args[3].kind != VEC_VALUE_NUMBER ||
			    args[4].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: region(cond, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			double x_min = vec_number_float64(args[1].num);
			double x_max = vec_number_float64(args[2].num);
			double y_min = vec_number_float64(args[3].num);
			double y_max = vec_number_float64(args[4].num);
			if (x_min >= x_max || y_min >= y_max) {
				snprintf(err, errsz, "eval: region expects min < max");
				goto fail;
			}
			int n = 128;
			if (argc == 6) {
				if (args[5].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: region(cond, xmin, xmax, ymin, ymax[, n])");
					goto fail;
				}
				double nn = vec_number_float64(args[5].num);
				if (isnan(nn) || isinf(nn) || nn != trunc_d(nn)) {
					snprintf(err, errsz, "eval: expected integer");
					goto fail;
				}
				if (nn < 8 || nn > 1024) {
					snprintf(err, errsz, "eval: region n must be 8..1024");
					goto fail;
				}
				n = (int)nn;
			}

			struct env_saved_var prev_x = {0};
			struct env_saved_var prev_y = {0};
			if (env_save_var(e, "x", &prev_x, err, errsz) != 0)
				goto fail;
			if (env_save_var(e, "y", &prev_y, err, errsz) != 0) {
				vec_value_destroy(&prev_x.v);
				goto fail;
			}

			double *data = NULL;
			size_t len = 0;
			size_t cap = 0;

			for (int yi = 0; yi < n; yi++) {
				double ty = (double)yi / (double)(n - 1);
				double y = y_min + ty * (y_max - y_min);
				int run = 0;
				for (int xi = 0; xi < n; xi++) {
					double tx = (double)xi / (double)(n - 1);
					double x = x_min + tx * (x_max - x_min);

					if (vec_env_set_var(e, "x", vec_value_number(vec_float(x))) != 0 ||
					    vec_env_set_var(e, "y", vec_value_number(vec_float(y))) != 0) {
						snprintf(err, errsz, "eval: out of memory");
						goto region_fail;
					}
					vec_value v;
					memset(&v, 0, sizeof(v));
					if (vec_eval_node(e, args[0].expr, &v, err, errsz) != 0) {
						vec_value_destroy(&v);
						goto region_fail;
					}

					int ok;
					if (value_truthy(&v, &ok) != 0) {
						vec_value_destroy(&v);
						snprintf(err, errsz, "eval: condition must be a number");
						goto region_fail;
					}
					vec_value_destroy(&v);

					if (ok) {
						if (xy_append(&data, &len, &cap, x, y, err, errsz) != 0)
							goto region_fail;
						run = 1;
						continue;
					}
					if (run) {
						if (xy_append_break(&data, &len, &cap, err, errsz) != 0)
							goto region_fail;
						run = 0;
					}
				}
				if (xy_append_break(&data, &len, &cap, err, errsz) != 0)
					goto region_fail;
			}

			env_restore_var(e, "y", &prev_y);
			env_restore_var(e, "x", &prev_x);

			int rows = (int)(len / 2);
			if (rows == 0) {
				free(data);
				data = malloc(sizeof(data[0]) * 2);
				if (!data) {
					snprintf(err, errsz, "eval: out of memory");
					goto fail;
				}
				data[0] = NAN;
				data[1] = NAN;
				rows = 1;
			}
			*out = vec_value_matrix(rows, 2, data);
			goto done;

		region_fail:
			free(data);
			env_restore_var(e, "y", &prev_y);
			env_restore_var(e, "x", &prev_x);
			goto fail;
		}

		/* Plot builtins. */
		if (!strcmp(name, "implicit")) {
			if (argc < 5 || argc > 6) {
				snprintf(err, errsz, "eval: implicit(expr, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_NUMBER ||
			    args[2].kind != VEC_VALUE_NUMBER ||
			    args[3].kind != VEC_VALUE_NUMBER ||
			    args[4].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: implicit(expr, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			double xmin = vec_number_float64(args[1].num);
			double xmax = vec_number_float64(args[2].num);
			double ymin = vec_number_float64(args[3].num);
			double ymax = vec_number_float64(args[4].num);
			int n = 96;
			if (argc == 6) {
				if (args[5].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: implicit n must be 8..512");
					goto fail;
				}
				double nn = vec_number_float64(args[5].num);
				if (isnan(nn) || isinf(nn) || nn != trunc_d(nn) || nn < 8 || nn > 512) {
					snprintf(err, errsz, "eval: implicit n must be 8..512");
					goto fail;
				}
				n = (int)nn;
			}

			double level0 = 0;
			double *data = NULL;
			int rows = 0;
			if (plot_contour_marching_squares(e, args[0].expr, &level0, 1, xmin, xmax, ymin, ymax, n,
							 &data, &rows, err, errsz) != 0)
				goto fail;
			*out = vec_value_matrix(rows, 2, data);
			goto done;
		}

		if (!strcmp(name, "contour")) {
			if (argc < 6 || argc > 7) {
				snprintf(err, errsz, "eval: contour(expr, levels, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[2].kind != VEC_VALUE_NUMBER ||
			    args[3].kind != VEC_VALUE_NUMBER ||
			    args[4].kind != VEC_VALUE_NUMBER ||
			    args[5].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: contour(expr, levels, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			const double *levels = NULL;
			size_t nlevels = 0;
			double single = 0;
			if (args[1].kind == VEC_VALUE_NUMBER) {
				single = vec_number_float64(args[1].num);
				levels = &single;
				nlevels = 1;
			} else if (args[1].kind == VEC_VALUE_ARRAY) {
				if (args[1].len == 0) {
					snprintf(err, errsz, "eval: contour: empty levels");
					goto fail;
				}
				levels = args[1].arr;
				nlevels = args[1].len;
			} else {
				snprintf(err, errsz, "eval: contour: expected number or array");
				goto fail;
			}

			double xmin = vec_number_float64(args[2].num);
			double xmax = vec_number_float64(args[3].num);
			double ymin = vec_number_float64(args[4].num);
			double ymax = vec_number_float64(args[5].num);
			int n = 96;
			if (argc == 7) {
				if (args[6].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: contour n must be 8..512");
					goto fail;
				}
				double nn = vec_number_float64(args[6].num);
				if (isnan(nn) || isinf(nn) || nn != trunc_d(nn) || nn < 8 || nn > 512) {
					snprintf(err, errsz, "eval: contour n must be 8..512");
					goto fail;
				}
				n = (int)nn;
			}

			double *data = NULL;
			int rows = 0;
			if (plot_contour_marching_squares(e, args[0].expr, levels, nlevels, xmin, xmax, ymin, ymax, n,
							 &data, &rows, err, errsz) != 0)
				goto fail;
			*out = vec_value_matrix(rows, 2, data);
			goto done;
		}

			if (!strcmp(name, "vectorfield")) {
				if (argc < 6 || argc > 7) {
					snprintf(err, errsz, "eval: vectorfield(f, g, xmin, xmax, ymin, ymax[, n])");
					goto fail;
				}
			if (args[0].kind != VEC_VALUE_EXPR ||
			    args[1].kind != VEC_VALUE_EXPR ||
			    args[2].kind != VEC_VALUE_NUMBER ||
			    args[3].kind != VEC_VALUE_NUMBER ||
			    args[4].kind != VEC_VALUE_NUMBER ||
			    args[5].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: vectorfield(f, g, xmin, xmax, ymin, ymax[, n])");
				goto fail;
			}
			double xmin = vec_number_float64(args[2].num);
			double xmax = vec_number_float64(args[3].num);
			double ymin = vec_number_float64(args[4].num);
			double ymax = vec_number_float64(args[5].num);
			int n = 16;
			if (argc == 7) {
				if (args[6].kind != VEC_VALUE_NUMBER) {
					snprintf(err, errsz, "eval: vectorfield n must be 4..128");
					goto fail;
				}
				double nn = vec_number_float64(args[6].num);
				if (isnan(nn) || isinf(nn) || nn != trunc_d(nn) || nn < 4 || nn > 128) {
					snprintf(err, errsz, "eval: vectorfield n must be 4..128");
					goto fail;
				}
				n = (int)nn;
			}

				double *data = NULL;
				int rows = 0;
				if (plot_vectorfield_segments(e, args[0].expr, args[1].expr, xmin, xmax, ymin, ymax, n,
						      &data, &rows, err, errsz) != 0)
					goto fail;
				*out = vec_value_matrix(rows, 2, data);
				goto done;
			}

			if (!strcmp(name, "plane")) {
					if (argc == 2 && args[0].kind == VEC_VALUE_ARRAY && args[0].len == 3 && args[1].kind == VEC_VALUE_NUMBER) {
						double nx = args[0].arr[0];
						double ny = args[0].arr[1];
						double nz = args[0].arr[2];
					double d = vec_number_float64(args[1].num);
					if (nz == 0) {
						snprintf(err, errsz, "eval: plane: n.z must be non-zero (not a function z(x,y))");
						goto fail;
					}
					vec_node *termx = vec_node_binary_new('*', vec_node_number_new(vec_float(nx)), vec_node_ident_new("x", 1));
					vec_node *termy = vec_node_binary_new('*', vec_node_number_new(vec_float(ny)), vec_node_ident_new("y", 1));
					vec_node *sum = vec_node_binary_new('+', termx, termy);
					vec_node *num = vec_node_binary_new('+', sum, vec_node_number_new(vec_float(d)));
					vec_node *neg = vec_node_unary_new('-', num);
					vec_node *div = vec_node_binary_new('/', neg, vec_node_number_new(vec_float(nz)));
					vec_node *simp = div ? vec_node_simplify_owned(div) : NULL;
					if (!simp) {
						snprintf(err, errsz, "eval: out of memory");
						goto fail;
					}
					*out = vec_value_expr(simp);
					goto done;
				}

				if (argc == 3 &&
				    args[0].kind == VEC_VALUE_ARRAY && args[0].len == 3 &&
				    args[1].kind == VEC_VALUE_ARRAY && args[1].len == 3 &&
				    args[2].kind == VEC_VALUE_ARRAY && args[2].len == 3) {
					const double *p0 = args[0].arr;
					const double *p1 = args[1].arr;
					const double *p2 = args[2].arr;
					double u0 = p1[0] - p0[0];
					double u1 = p1[1] - p0[1];
					double u2 = p1[2] - p0[2];
					double v0 = p2[0] - p0[0];
					double v1 = p2[1] - p0[1];
					double v2 = p2[2] - p0[2];

					double nx = u1 * v2 - u2 * v1;
					double ny = u2 * v0 - u0 * v2;
					double nz = u0 * v1 - u1 * v0;
					if (nz == 0) {
						snprintf(err, errsz, "eval: plane: points form vertical plane (not a function z(x,y))");
						goto fail;
					}
					double d = -(nx * p0[0] + ny * p0[1] + nz * p0[2]);

					vec_node *termx = vec_node_binary_new('*', vec_node_number_new(vec_float(nx)), vec_node_ident_new("x", 1));
					vec_node *termy = vec_node_binary_new('*', vec_node_number_new(vec_float(ny)), vec_node_ident_new("y", 1));
					vec_node *sum = vec_node_binary_new('+', termx, termy);
					vec_node *num = vec_node_binary_new('+', sum, vec_node_number_new(vec_float(d)));
					vec_node *neg = vec_node_unary_new('-', num);
					vec_node *div = vec_node_binary_new('/', neg, vec_node_number_new(vec_float(nz)));
					vec_node *simp = div ? vec_node_simplify_owned(div) : NULL;
					if (!simp) {
						snprintf(err, errsz, "eval: out of memory");
						goto fail;
					}
					*out = vec_value_expr(simp);
					goto done;
				}

				snprintf(err, errsz, "eval: plane(n, d) or plane(p0, p1, p2)");
				goto fail;
			}

			/* Polynomial builtins. */
			if (!strcmp(name, "polyval")) {
				if (argc != 2 || args[0].kind != VEC_VALUE_ARRAY) {
					snprintf(err, errsz, "eval: polyval(coeffs, x)");
					goto fail;
			}
			char pbuf[96];
			pbuf[0] = 0;
			vec_poly p = {0};
			if (vec_poly_from_coeffs(args[0].arr, args[0].len, &p, pbuf, sizeof(pbuf)) != 0) {
				snprintf(err, errsz, "eval: polyval: %s", pbuf[0] ? pbuf : "out of memory");
				goto fail;
			}
			if (args[1].kind == VEC_VALUE_NUMBER) {
				double x = vec_number_float64(args[1].num);
				double y = vec_poly_eval(&p, x);
				vec_poly_destroy(&p);
				*out = vec_value_number(vec_float(y));
				goto done;
			}
			if (args[1].kind == VEC_VALUE_COMPLEX) {
				vec_complex y = poly_eval_complex(p.coeffs, p.len, args[1].c);
				vec_poly_destroy(&p);
				*out = vec_value_complex(y.re, y.im);
				goto done;
			}
			vec_poly_destroy(&p);
			snprintf(err, errsz, "eval: polyval expects numeric x");
			goto fail;
		}

		if (!strcmp(name, "polyfit")) {
			if (argc != 2 && argc != 3) {
				snprintf(err, errsz, "eval: polyfit(data, n) or polyfit(x, y, n)");
				goto fail;
			}

			const double *xs = NULL;
			const double *ys = NULL;
			size_t npts = 0;
			const vec_value *deg_v = NULL;

			if (argc == 2) {
				if (args[0].kind != VEC_VALUE_MATRIX || args[0].cols != 2) {
					snprintf(err, errsz, "eval: polyfit expects Nx2 matrix");
					goto fail;
				}
				npts = (size_t)args[0].rows;
				deg_v = &args[1];
			} else {
				if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
					snprintf(err, errsz, "eval: polyfit expects x and y as arrays");
					goto fail;
				}
				if (args[0].len != args[1].len) {
					snprintf(err, errsz, "eval: polyfit x/y length mismatch");
					goto fail;
				}
				xs = args[0].arr;
				ys = args[1].arr;
				npts = args[0].len;
				deg_v = &args[2];
			}

			if (!deg_v || deg_v->kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: polyfit degree must be 0..32");
				goto fail;
			}
			double degf = vec_number_float64(deg_v->num);
			if (isnan(degf) || isinf(degf) || degf != trunc_d(degf) || degf < 0 || degf > 32) {
				snprintf(err, errsz, "eval: polyfit degree must be 0..32");
				goto fail;
			}
			if (npts == 0) {
				snprintf(err, errsz, "eval: polyfit: empty data");
				goto fail;
			}

			int deg = (int)degf;
			int m = deg + 1;
			size_t mm = (size_t)m * (size_t)m;
			double *ata = calloc(mm, sizeof(ata[0]));
			double *atb = calloc((size_t)m, sizeof(atb[0]));
			if (!ata || !atb) {
				free(ata);
				free(atb);
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}

			double pows[33];
			for (size_t i = 0; i < npts; i++) {
				double x, y;
				if (argc == 2) {
					x = args[0].mat[i * 2 + 0];
					y = args[0].mat[i * 2 + 1];
				} else {
					x = xs[i];
					y = ys[i];
				}
				if (isnan(x) || isnan(y) || isinf(x) || isinf(y))
					continue;
				pows[0] = 1;
				for (int j = 1; j < m; j++)
					pows[j] = pows[j - 1] * x;
				for (int r = 0; r < m; r++) {
					atb[r] += pows[r] * y;
					for (int c = 0; c < m; c++)
						ata[(size_t)r * (size_t)m + (size_t)c] += pows[r] * pows[c];
				}
			}

			double *coeffs = NULL;
			vec_mat_err prc = vec_solve_linear_system(ata, atb, m, &coeffs);
			free(ata);
			free(atb);
			if (prc == VEC_MAT_ERR_NOMEM) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			if (prc != VEC_MAT_OK) {
				snprintf(err, errsz, "eval: polyfit: singular system");
				goto fail;
			}
			*out = vec_value_array(coeffs, (size_t)m);
			goto done;
		}

		if (!strcmp(name, "roots") && argc == 1 && args[0].kind == VEC_VALUE_ARRAY) {
			char pbuf[96];
			pbuf[0] = 0;
			vec_poly p = {0};
			if (vec_poly_from_coeffs(args[0].arr, args[0].len, &p, pbuf, sizeof(pbuf)) != 0) {
				snprintf(err, errsz, "eval: roots: %s", pbuf[0] ? pbuf : "out of memory");
				goto fail;
			}
			int deg = vec_poly_degree(&p);
			if (deg <= 0) {
				vec_poly_destroy(&p);
				snprintf(err, errsz, "eval: roots: degree must be >= 1");
				goto fail;
			}
			double cn = p.coeffs[(size_t)deg];
			if (cn == 0) {
				vec_poly_destroy(&p);
				snprintf(err, errsz, "eval: roots: leading coefficient is zero");
				goto fail;
			}

			double *coeffs = malloc(sizeof(coeffs[0]) * (size_t)(deg + 1));
			if (!coeffs) {
				vec_poly_destroy(&p);
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			for (int i = 0; i <= deg; i++)
				coeffs[i] = p.coeffs[i] / cn;
			vec_poly_destroy(&p);

			if (deg == 1) {
				double *outm = malloc(sizeof(outm[0]) * 2);
				if (!outm) {
					free(coeffs);
					snprintf(err, errsz, "eval: out of memory");
					goto fail;
				}
				outm[0] = -coeffs[0];
				outm[1] = 0;
				free(coeffs);
				*out = vec_value_matrix(1, 2, outm);
				goto done;
			}

			double max_abs = 0;
			for (int i = 0; i < deg; i++) {
				double v = fabs(coeffs[i]);
				if (v > max_abs)
					max_abs = v;
			}
			double radius = 1 + max_abs;

			vec_complex *roots = malloc(sizeof(roots[0]) * (size_t)deg);
			if (!roots) {
				free(coeffs);
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			for (int k = 0; k < deg; k++) {
				double theta = 2 * M_PI * (double)k / (double)deg;
				roots[k].re = radius * cos(theta);
				roots[k].im = radius * sin(theta);
			}

			const double tol = 1e-12;
			const int max_iter = 256;
			for (int iter = 0; iter < max_iter; iter++) {
				double max_delta = 0;
				for (int i = 0; i < deg; i++) {
					vec_complex zi = roots[i];
					vec_complex den;
					den.re = 1;
					den.im = 0;
					for (int j = 0; j < deg; j++) {
						if (i == j)
							continue;
						vec_complex d = c_sub(zi, roots[j]);
						if (c_is_zero(d)) {
							d.re = 1e-9;
							d.im = 1e-9;
						}
						den = c_mul(den, d);
					}
					if (c_is_zero(den))
						continue;
					vec_complex pz = poly_eval_complex(coeffs, (size_t)(deg + 1), zi);
					vec_complex dz = c_div(pz, den);
					roots[i] = c_sub(zi, dz);
					double dabs = c_abs(dz);
					if (dabs > max_delta)
						max_delta = dabs;
				}
				if (max_delta <= tol)
					break;
			}

			double *outm = malloc(sizeof(outm[0]) * (size_t)deg * 2);
			if (!outm) {
				free(coeffs);
				free(roots);
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			for (int i = 0; i < deg; i++) {
				outm[i * 2 + 0] = roots[i].re;
				outm[i * 2 + 1] = roots[i].im;
			}
			free(coeffs);
			free(roots);
			*out = vec_value_matrix(deg, 2, outm);
			goto done;
		}

			/* Complex helpers. */
			if (!strcmp(name, "rect") && argc == 2 && args[0].kind == VEC_VALUE_NUMBER && args[1].kind == VEC_VALUE_NUMBER) {
				double r = vec_number_float64(args[0].num);
				double phi = vec_number_float64(args[1].num);
		*out = vec_value_complex(r * cos(phi), r * sin(phi));
		goto done;
	}
	if (!strcmp(name, "re") && argc == 1) {
		if (args[0].kind != VEC_VALUE_COMPLEX) {
			snprintf(err, errsz, "eval: re(z)");
			goto fail;
		}
		*out = vec_value_number(vec_float(args[0].c.re));
		goto done;
	}
	if (!strcmp(name, "im") && argc == 1) {
		if (args[0].kind != VEC_VALUE_COMPLEX) {
			snprintf(err, errsz, "eval: im(z)");
			goto fail;
		}
		*out = vec_value_number(vec_float(args[0].c.im));
		goto done;
	}
	if (!strcmp(name, "conj") && argc == 1) {
		if (args[0].kind != VEC_VALUE_COMPLEX) {
			snprintf(err, errsz, "eval: conj(z)");
			goto fail;
		}
		vec_complex z = c_conj(args[0].c);
		*out = vec_value_complex(z.re, z.im);
		goto done;
	}
	if (!strcmp(name, "arg") && argc == 1) {
		if (args[0].kind != VEC_VALUE_COMPLEX) {
			snprintf(err, errsz, "eval: arg(z)");
			goto fail;
		}
		*out = vec_value_number(vec_float(c_arg(args[0].c)));
		goto done;
	}
	if (!strcmp(name, "polar") && argc == 1) {
		if (args[0].kind != VEC_VALUE_COMPLEX) {
			snprintf(err, errsz, "eval: polar(z)");
			goto fail;
		}
		double *xs = malloc(sizeof(xs[0]) * 2);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		xs[0] = c_abs(args[0].c);
		xs[1] = c_arg(args[0].c);
		*out = vec_value_array(xs, 2);
		goto done;
	}
	/* Array builtins. */
	if (!strcmp(name, "range")) {
		if (argc < 2 || argc > 3) {
			snprintf(err, errsz, "eval: range expects 2 or 3 arguments");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: range bounds must be numbers");
			goto fail;
		}
		int npts = 256;
		if (argc == 3) {
			if (args[2].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: range count must be a number");
				goto fail;
			}
			double nf = vec_number_float64(args[2].num);
			if (nf < 2 || nf > 4096) {
				snprintf(err, errsz, "eval: range count must be 2..4096");
				goto fail;
			}
			npts = (int)nf;
		}
		double a = vec_number_float64(args[0].num);
		double b = vec_number_float64(args[1].num);
		double *xs = malloc(sizeof(xs[0]) * (size_t)npts);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (npts == 1) {
			xs[0] = a;
		} else {
			for (int i = 0; i < npts; i++) {
				double t = (double)i / (double)(npts - 1);
				xs[i] = a + t * (b - a);
			}
		}
		*out = vec_value_array(xs, (size_t)npts);
		goto done;
	}

	if (!strcmp(name, "clamp") && argc == 3 && args[0].kind == VEC_VALUE_ARRAY &&
	    args[1].kind == VEC_VALUE_NUMBER && args[2].kind == VEC_VALUE_NUMBER) {
		double lo = vec_number_float64(args[1].num);
		double hi = vec_number_float64(args[2].num);
		if (lo > hi) {
			double tmp = lo;
			lo = hi;
			hi = tmp;
		}
		size_t n = args[0].len;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		for (size_t i = 0; i < n; i++) {
			double x = args[0].arr[i];
			if (x < lo)
				x = lo;
			else if (x > hi)
				x = hi;
			xs[i] = x;
		}
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (argc == 1 && args[0].kind == VEC_VALUE_ARRAY) {
		unary_d_fn fn = unary_builtin_find(name);
		if (fn) {
			size_t n = args[0].len;
			double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
			if (n && !xs) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			for (size_t i = 0; i < n; i++) {
				xs[i] = fn(args[0].arr[i]);
			}
			*out = vec_value_array(xs, n);
			goto done;
		}

		agg_d_fn agg = agg_builtin_find(name);
		if (agg) {
			*out = vec_value_number(vec_float(agg(args[0].arr, args[0].len)));
			goto done;
		}
	}
	if (argc == 1) {
		vec_complex z;
		if (value_to_complex(&args[0], &z) == 0 && args[0].kind == VEC_VALUE_COMPLEX) {
			if (!strcmp(name, "abs")) {
				*out = vec_value_number(vec_float(c_abs(z)));
				goto done;
			}
			if (!strcmp(name, "sqrt")) {
				vec_complex w = c_sqrt(z);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
			if (!strcmp(name, "exp")) {
				vec_complex w = c_exp(z);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
			if (!strcmp(name, "ln") || !strcmp(name, "log")) {
				vec_complex w = c_log(z);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
			if (!strcmp(name, "sin")) {
				vec_complex w = c_sin(z);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
			if (!strcmp(name, "cos")) {
				vec_complex w = c_cos(z);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
			if (!strcmp(name, "tan")) {
				vec_complex s = c_sin(z);
				vec_complex c = c_cos(z);
				vec_complex w = c_div(s, c);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
		}
	}

	/* Linear algebra builtins. */
	if (!strcmp(name, "solve")) {
		if (argc != 2 || args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: solve(A, b)");
			goto fail;
		}
		const vec_value *A = &args[0];
		if (A->rows != A->cols) {
			snprintf(err, errsz, "eval: solve expects square matrix");
			goto fail;
		}
		int n = A->rows;

		if (args[1].kind == VEC_VALUE_ARRAY) {
			if ((int)args[1].len != n) {
				snprintf(err, errsz, "eval: solve expects len(b)==rows(A)");
				goto fail;
			}
			double *x = NULL;
			vec_mat_err rc = vec_solve_linear_system(A->mat, args[1].arr, n, &x);
			if (rc == VEC_MAT_ERR_NOMEM) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			if (rc != VEC_MAT_OK) {
				snprintf(err, errsz, "eval: solve: singular system");
				goto fail;
			}
			*out = vec_value_array(x, (size_t)n);
			goto done;
		}

		if (args[1].kind == VEC_VALUE_MATRIX) {
			const vec_value *B = &args[1];
			if (B->rows != n) {
				snprintf(err, errsz, "eval: solve expects rows(b)==rows(A)");
				goto fail;
			}
			double *x = NULL;
			vec_mat_err rc = vec_solve_linear_system_multi(A->mat, B->mat, n, B->cols, &x);
			if (rc == VEC_MAT_ERR_NOMEM) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			if (rc != VEC_MAT_OK) {
				snprintf(err, errsz, "eval: solve: singular system");
				goto fail;
			}
			*out = vec_value_matrix(n, B->cols, x);
			goto done;
		}

		snprintf(err, errsz, "eval: solve expects b as array or matrix");
		goto fail;
	}

	if (!strcmp(name, "qr")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: qr(A)");
			goto fail;
		}
		const vec_value *A = &args[0];

		double *q = NULL;
		double *r = NULL;
		vec_mat_err rc = vec_qr_decompose(A->rows, A->cols, A->mat, &q, &r);
		if (rc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (rc == VEC_MAT_ERR_SINGULAR) {
			snprintf(err, errsz, "eval: qr: rank deficient");
			goto fail;
		}
		if (rc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix");
			goto fail;
		}

		size_t qn = (size_t)A->rows * (size_t)A->cols;
		double *qret = qn ? malloc(sizeof(qret[0]) * qn) : NULL;
		if (qn && !qret) {
			free(q);
			free(r);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (qn)
			memcpy(qret, q, sizeof(qret[0]) * qn);

		if (vec_env_set_var(e, "_R", vec_value_matrix(A->cols, A->cols, r)) != 0) {
			free(q);
			free(qret);
			free(r);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (vec_env_set_var(e, "_Q", vec_value_matrix(A->rows, A->cols, q)) != 0) {
			free(q);
			free(qret);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		*out = vec_value_matrix(A->rows, A->cols, qret);
		goto done;
	}

	if (!strcmp(name, "svd")) {
		if (argc != 1 || args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: svd(A)");
			goto fail;
		}
		const vec_value *A = &args[0];

		if (A->cols > 64) {
			snprintf(err, errsz, "eval: svd supports n<=64");
			goto fail;
		}

		double *u = NULL;
		double *s = NULL;
		double *v = NULL;
		vec_mat_err rc = vec_svd_thin(A->rows, A->cols, A->mat, &u, &s, &v);
		if (rc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (rc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: svd: invalid matrix");
			goto fail;
		}

		size_t sn = (size_t)A->cols;
		double *scopy = sn ? malloc(sizeof(scopy[0]) * sn) : NULL;
		if (sn && !scopy) {
			free(u);
			free(v);
			free(s);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (sn)
			memcpy(scopy, s, sizeof(scopy[0]) * sn);

		if (vec_env_set_var(e, "_U", vec_value_matrix(A->rows, A->cols, u)) != 0) {
			free(u);
			free(v);
			free(s);
			free(scopy);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (vec_env_set_var(e, "_V", vec_value_matrix(A->cols, A->cols, v)) != 0) {
			free(v);
			free(s);
			free(scopy);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (vec_env_set_var(e, "_S", vec_value_array(scopy, sn)) != 0) {
			free(s);
			free(scopy);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}

		*out = vec_value_array(s, sn);
		goto done;
	}

	/* Vector builtins (Array helpers). */
	if (!strcmp(name, "get")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: get(v, i)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: get(v, i)");
			goto fail;
		}
		size_t idx;
		if (vector_index(vec_number_float64(args[1].num), args[0].len, &idx, err, errsz) != 0)
			goto fail;
		*out = vec_value_number(vec_float(args[0].arr[idx]));
		goto done;
	}
	if (!strcmp(name, "set")) {
		if (argc != 3) {
			snprintf(err, errsz, "eval: set(v, i, value)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_NUMBER || args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: set(v, i, value)");
			goto fail;
		}
		size_t idx;
		if (vector_index(vec_number_float64(args[1].num), args[0].len, &idx, err, errsz) != 0)
			goto fail;
		size_t n = args[0].len;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		memcpy(xs, args[0].arr, sizeof(xs[0]) * n);
		xs[idx] = vec_number_float64(args[2].num);
		*out = vec_value_array(xs, n);
		goto done;
	}

	if ((!strcmp(name, "x") || !strcmp(name, "y") || !strcmp(name, "z") || !strcmp(name, "w")) && argc == 1) {
		if (args[0].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: %s(v)", name);
			goto fail;
		}
		size_t idx = 0;
		if (!strcmp(name, "y"))
			idx = 1;
		else if (!strcmp(name, "z"))
			idx = 2;
		else if (!strcmp(name, "w"))
			idx = 3;
		if (idx >= args[0].len) {
			snprintf(err, errsz, "eval: %s expects vector length >= %lu", name, (unsigned long)(idx + 1));
			goto fail;
		}
		*out = vec_value_number(vec_float(args[0].arr[idx]));
		goto done;
	}

	if (!strcmp(name, "vec2")) {
		if (argc != 2 || args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: vec2(x, y)");
			goto fail;
		}
		double *xs = malloc(sizeof(xs[0]) * 2);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		xs[0] = vec_number_float64(args[0].num);
		xs[1] = vec_number_float64(args[1].num);
		*out = vec_value_array(xs, 2);
		goto done;
	}
	if (!strcmp(name, "vec3")) {
		if (argc != 3 || args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER ||
		    args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: vec3(x, y, z)");
			goto fail;
		}
		double *xs = malloc(sizeof(xs[0]) * 3);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		xs[0] = vec_number_float64(args[0].num);
		xs[1] = vec_number_float64(args[1].num);
		xs[2] = vec_number_float64(args[2].num);
		*out = vec_value_array(xs, 3);
		goto done;
	}
	if (!strcmp(name, "vec4")) {
		if (argc != 4 || args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER ||
		    args[2].kind != VEC_VALUE_NUMBER || args[3].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: vec4(x, y, z, w)");
			goto fail;
		}
		double *xs = malloc(sizeof(xs[0]) * 4);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		xs[0] = vec_number_float64(args[0].num);
		xs[1] = vec_number_float64(args[1].num);
		xs[2] = vec_number_float64(args[2].num);
		xs[3] = vec_number_float64(args[3].num);
		*out = vec_value_array(xs, 4);
		goto done;
	}

	if (!strcmp(name, "dot")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: dot(a, b)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: dot(a, b)");
			goto fail;
		}
		if (args[0].len != args[1].len) {
			snprintf(err, errsz, "eval: array length mismatch");
			goto fail;
		}
		double sum = 0;
		for (size_t i = 0; i < args[0].len; i++)
			sum += args[0].arr[i] * args[1].arr[i];
		*out = vec_value_number(vec_float(sum));
		goto done;
	}
	if (!strcmp(name, "cross")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: cross(a, b)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: cross(a, b)");
			goto fail;
		}
		if (args[0].len != 3 || args[1].len != 3) {
			snprintf(err, errsz, "eval: cross expects 3D vectors");
			goto fail;
		}
		double *xs = malloc(sizeof(xs[0]) * 3);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		const double *a3 = args[0].arr;
		const double *b3 = args[1].arr;
		xs[0] = a3[1] * b3[2] - a3[2] * b3[1];
		xs[1] = a3[2] * b3[0] - a3[0] * b3[2];
		xs[2] = a3[0] * b3[1] - a3[1] * b3[0];
		*out = vec_value_array(xs, 3);
		goto done;
	}

	if (!strcmp(name, "norm") || !strcmp(name, "mag")) {
		if (argc != 1) {
			snprintf(err, errsz, "eval: norm(v)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: norm(v)");
			goto fail;
		}
		double ss = 0;
		for (size_t i = 0; i < args[0].len; i++)
			ss += args[0].arr[i] * args[0].arr[i];
		*out = vec_value_number(vec_float(sqrt(ss)));
		goto done;
	}

	if (!strcmp(name, "unit") || !strcmp(name, "normalize")) {
		if (argc != 1) {
			snprintf(err, errsz, "eval: unit(v)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: unit(v)");
			goto fail;
		}
		double ss = 0;
		for (size_t i = 0; i < args[0].len; i++)
			ss += args[0].arr[i] * args[0].arr[i];
		if (ss == 0) {
			snprintf(err, errsz, "eval: zero-length vector");
			goto fail;
		}
		double inv = 1.0 / sqrt(ss);
		size_t n = args[0].len;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		for (size_t i = 0; i < n; i++)
			xs[i] = args[0].arr[i] * inv;
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (!strcmp(name, "dist")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: dist(a, b)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: dist(a, b)");
			goto fail;
		}
		if (args[0].len != args[1].len) {
			snprintf(err, errsz, "eval: array length mismatch");
			goto fail;
		}
		double ss = 0;
		for (size_t i = 0; i < args[0].len; i++) {
			double d = args[0].arr[i] - args[1].arr[i];
			ss += d * d;
		}
		*out = vec_value_number(vec_float(sqrt(ss)));
		goto done;
	}

	if (!strcmp(name, "angle")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: angle(a, b)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: angle(a, b)");
			goto fail;
		}
		if (args[0].len != args[1].len) {
			snprintf(err, errsz, "eval: array length mismatch");
			goto fail;
		}
		double dot = 0;
		double aa = 0;
		double bb = 0;
		for (size_t i = 0; i < args[0].len; i++) {
			double av = args[0].arr[i];
			double bv = args[1].arr[i];
			dot += av * bv;
			aa += av * av;
			bb += bv * bv;
		}
		if (aa == 0 || bb == 0) {
			snprintf(err, errsz, "eval: angle undefined for zero vector");
			goto fail;
		}
		double c = dot / sqrt(aa * bb);
		if (c < -1)
			c = -1;
		if (c > 1)
			c = 1;
		*out = vec_value_number(vec_float(acos(c)));
		goto done;
	}

	if (!strcmp(name, "proj")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: proj(a, b)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: proj(a, b)");
			goto fail;
		}
		if (args[0].len != args[1].len) {
			snprintf(err, errsz, "eval: array length mismatch");
			goto fail;
		}
		double dot = 0;
		double bb = 0;
		for (size_t i = 0; i < args[0].len; i++) {
			double av = args[0].arr[i];
			double bv = args[1].arr[i];
			dot += av * bv;
			bb += bv * bv;
		}
		if (bb == 0) {
			snprintf(err, errsz, "eval: projection onto zero vector");
			goto fail;
		}
		double k = dot / bb;
		size_t n = args[1].len;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		for (size_t i = 0; i < n; i++)
			xs[i] = args[1].arr[i] * k;
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (!strcmp(name, "outer")) {
		if (argc != 2) {
			snprintf(err, errsz, "eval: outer(a, b)");
			goto fail;
		}
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: outer(a, b)");
			goto fail;
		}
		int r = (int)args[0].len;
		int c = (int)args[1].len;
		if (r <= 0 || c <= 0) {
			snprintf(err, errsz, "eval: outer expects non-empty vectors");
			goto fail;
		}
		size_t n = (size_t)r * (size_t)c;
		double *m = malloc(sizeof(m[0]) * n);
		if (!m) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		for (int i = 0; i < r; i++) {
			for (int j = 0; j < c; j++) {
				m[(size_t)i * (size_t)c + (size_t)j] = args[0].arr[i] * args[1].arr[j];
			}
		}
		*out = vec_value_matrix(r, c, m);
		goto done;
	}

	if (!strcmp(name, "lerp")) {
		if (argc != 3) {
			snprintf(err, errsz, "eval: lerp(a, b, t)");
			goto fail;
		}
		if (args[0].kind == VEC_VALUE_ARRAY && args[1].kind == VEC_VALUE_ARRAY && args[2].kind == VEC_VALUE_NUMBER) {
			if (args[0].len != args[1].len) {
				snprintf(err, errsz, "eval: array length mismatch");
				goto fail;
			}
			double t = vec_number_float64(args[2].num);
			size_t n = args[0].len;
			double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
			if (n && !xs) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			for (size_t i = 0; i < n; i++) {
				double a0 = args[0].arr[i];
				xs[i] = a0 + (args[1].arr[i] - a0) * t;
			}
			*out = vec_value_array(xs, n);
			goto done;
		}
		if (args[0].kind == VEC_VALUE_NUMBER && args[1].kind == VEC_VALUE_NUMBER && args[2].kind == VEC_VALUE_NUMBER) {
			double a = vec_number_float64(args[0].num);
			double b = vec_number_float64(args[1].num);
			double t = vec_number_float64(args[2].num);
			*out = vec_value_number(vec_float(a + t * (b - a)));
			goto done;
		}
		snprintf(err, errsz, "eval: lerp(a, b, t)");
		goto fail;
	}

	/* Matrix builtins. */
	if (argc == 1 && args[0].kind == VEC_VALUE_MATRIX) {
		size_t n = 0;
		if (args[0].rows > 0 && args[0].cols > 0)
			n = (size_t)args[0].rows * (size_t)args[0].cols;

		unary_d_fn fn = unary_builtin_find(name);
		if (fn) {
			double *m = n ? malloc(sizeof(m[0]) * n) : NULL;
			if (n && !m) {
				snprintf(err, errsz, "eval: out of memory");
				goto fail;
			}
			for (size_t i = 0; i < n; i++)
				m[i] = fn(args[0].mat[i]);
			*out = vec_value_matrix(args[0].rows, args[0].cols, m);
			goto done;
		}
		agg_d_fn agg = agg_builtin_find(name);
		if (agg) {
			*out = vec_value_number(vec_float(agg(args[0].mat, n)));
			goto done;
		}
	}

	if (!strcmp(name, "zeros") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: zeros(rows, cols)");
			goto fail;
		}
		int r = (int)vec_number_float64(args[0].num);
		int c = (int)vec_number_float64(args[1].num);
		double *m;
		vec_mat_err mrc = vec_matrix_zeros(r, c, &m);
		if (mrc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (mrc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix shape");
			goto fail;
		}
		*out = vec_value_matrix(r, c, m);
		goto done;
	}

	if (!strcmp(name, "ones") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: ones(rows, cols)");
			goto fail;
		}
		int r = (int)vec_number_float64(args[0].num);
		int c = (int)vec_number_float64(args[1].num);
		double *m;
		vec_mat_err mrc = vec_matrix_ones(r, c, &m);
		if (mrc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (mrc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix shape");
			goto fail;
		}
		*out = vec_value_matrix(r, c, m);
		goto done;
	}

	if (!strcmp(name, "eye") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: eye(n)");
			goto fail;
		}
		int n = (int)vec_number_float64(args[0].num);
		double *m;
		vec_mat_err mrc = vec_matrix_eye(n, &m);
		if (mrc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (mrc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix shape");
			goto fail;
		}
		*out = vec_value_matrix(n, n, m);
		goto done;
	}

	if (!strcmp(name, "reshape") && argc == 3) {
		if ((args[0].kind != VEC_VALUE_ARRAY && args[0].kind != VEC_VALUE_MATRIX) || args[1].kind != VEC_VALUE_NUMBER ||
		    args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: reshape(xs, rows, cols)");
			goto fail;
		}
		int r = (int)vec_number_float64(args[1].num);
		int c = (int)vec_number_float64(args[2].num);
		size_t want = 0;
		if (r > 0 && c > 0)
			want = (size_t)r * (size_t)c;
		const double *xs = NULL;
		size_t have = 0;
		if (args[0].kind == VEC_VALUE_ARRAY) {
			xs = args[0].arr;
			have = args[0].len;
		} else {
			xs = args[0].mat;
			if (args[0].rows > 0 && args[0].cols > 0)
				have = (size_t)args[0].rows * (size_t)args[0].cols;
		}
		if (r <= 0 || c <= 0 || have != want) {
			snprintf(err, errsz, "eval: reshape: need len(xs)==rows*cols");
			goto fail;
		}
		double *m = want ? malloc(sizeof(m[0]) * want) : NULL;
		if (want && !m) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		memcpy(m, xs, sizeof(m[0]) * want);
		*out = vec_value_matrix(r, c, m);
		goto done;
	}

	if ((!strcmp(name, "T") || !strcmp(name, "transpose")) && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: T(A)");
			goto fail;
		}
		double *m;
		vec_mat_err mrc = vec_matrix_transpose(args[0].rows, args[0].cols, args[0].mat, &m);
		if (mrc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (mrc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix");
			goto fail;
		}
		*out = vec_value_matrix(args[0].cols, args[0].rows, m);
		goto done;
	}

	if (!strcmp(name, "det") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: det(A)");
			goto fail;
		}
		double d = 0;
		vec_mat_err mrc = vec_matrix_det(args[0].rows, args[0].cols, args[0].mat, &d);
		if (mrc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (mrc == VEC_MAT_ERR_SHAPE) {
			snprintf(err, errsz, "eval: matrix shape mismatch");
			goto fail;
		}
		if (mrc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix");
			goto fail;
		}
		*out = vec_value_number(vec_float(d));
		goto done;
	}

	if (!strcmp(name, "inv") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: inv(A)");
			goto fail;
		}
		double *m;
		vec_mat_err mrc = vec_matrix_inv(args[0].rows, args[0].cols, args[0].mat, &m);
		if (mrc == VEC_MAT_ERR_NOMEM) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		if (mrc == VEC_MAT_ERR_SHAPE) {
			snprintf(err, errsz, "eval: matrix shape mismatch");
			goto fail;
		}
		if (mrc == VEC_MAT_ERR_SINGULAR) {
			snprintf(err, errsz, "eval: singular matrix");
			goto fail;
		}
		if (mrc != VEC_MAT_OK) {
			snprintf(err, errsz, "eval: invalid matrix");
			goto fail;
		}
		*out = vec_value_matrix(args[0].rows, args[0].cols, m);
		goto done;
	}

	if (!strcmp(name, "shape") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: shape(A)");
			goto fail;
		}
		double *xs = malloc(sizeof(xs[0]) * 2);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		xs[0] = (double)args[0].rows;
		xs[1] = (double)args[0].cols;
		*out = vec_value_array(xs, 2);
		goto done;
	}

	if (!strcmp(name, "flatten") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: flatten(A)");
			goto fail;
		}
		size_t n = (size_t)args[0].rows * (size_t)args[0].cols;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		memcpy(xs, args[0].mat, sizeof(xs[0]) * n);
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (!strcmp(name, "get") && argc == 3) {
		if (args[0].kind != VEC_VALUE_MATRIX || args[1].kind != VEC_VALUE_NUMBER || args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: get(A, row, col)");
			goto fail;
		}
		size_t r, c;
		if (vector_index(vec_number_float64(args[1].num), (size_t)args[0].rows, &r, err, errsz) != 0)
			goto fail;
		if (vector_index(vec_number_float64(args[2].num), (size_t)args[0].cols, &c, err, errsz) != 0)
			goto fail;
		*out = vec_value_number(vec_float(args[0].mat[r * (size_t)args[0].cols + c]));
		goto done;
	}

	if (!strcmp(name, "set") && argc == 4) {
		if (args[0].kind != VEC_VALUE_MATRIX || args[1].kind != VEC_VALUE_NUMBER || args[2].kind != VEC_VALUE_NUMBER ||
		    args[3].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: set(A, row, col, value)");
			goto fail;
		}
		size_t r, c;
		if (vector_index(vec_number_float64(args[1].num), (size_t)args[0].rows, &r, err, errsz) != 0)
			goto fail;
		if (vector_index(vec_number_float64(args[2].num), (size_t)args[0].cols, &c, err, errsz) != 0)
			goto fail;
		size_t n = (size_t)args[0].rows * (size_t)args[0].cols;
		double *m = n ? malloc(sizeof(m[0]) * n) : NULL;
		if (n && !m) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		memcpy(m, args[0].mat, sizeof(m[0]) * n);
		m[r * (size_t)args[0].cols + c] = vec_number_float64(args[3].num);
		*out = vec_value_matrix(args[0].rows, args[0].cols, m);
		goto done;
	}

	if (!strcmp(name, "row") && argc == 2) {
		if (args[0].kind != VEC_VALUE_MATRIX || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: row(A, row)");
			goto fail;
		}
		size_t r;
		if (vector_index(vec_number_float64(args[1].num), (size_t)args[0].rows, &r, err, errsz) != 0)
			goto fail;
		size_t n = (size_t)args[0].cols;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		memcpy(xs, &args[0].mat[r * (size_t)args[0].cols], sizeof(xs[0]) * n);
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (!strcmp(name, "col") && argc == 2) {
		if (args[0].kind != VEC_VALUE_MATRIX || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: col(A, col)");
			goto fail;
		}
		size_t c;
		if (vector_index(vec_number_float64(args[1].num), (size_t)args[0].cols, &c, err, errsz) != 0)
			goto fail;
		size_t n = (size_t)args[0].rows;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		for (size_t r = 0; r < n; r++)
			xs[r] = args[0].mat[r * (size_t)args[0].cols + c];
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (!strcmp(name, "diag") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: diag(A)");
			goto fail;
		}
		if (args[0].rows != args[0].cols) {
			snprintf(err, errsz, "eval: matrix shape mismatch");
			goto fail;
		}
		size_t n = (size_t)args[0].rows;
		double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
		if (n && !xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		for (size_t i = 0; i < n; i++)
			xs[i] = args[0].mat[i * n + i];
		*out = vec_value_array(xs, n);
		goto done;
	}

	if (!strcmp(name, "trace") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: trace(A)");
			goto fail;
		}
		if (args[0].rows != args[0].cols) {
			snprintf(err, errsz, "eval: matrix shape mismatch");
			goto fail;
		}
		size_t n = (size_t)args[0].rows;
		double s = 0;
		for (size_t i = 0; i < n; i++)
			s += args[0].mat[i * n + i];
		*out = vec_value_number(vec_float(s));
		goto done;
	}

	if (!strcmp(name, "norm") && argc == 1) {
		if (args[0].kind != VEC_VALUE_MATRIX) {
			snprintf(err, errsz, "eval: norm(A)");
			goto fail;
		}
		size_t n = (size_t)args[0].rows * (size_t)args[0].cols;
		double ss = 0;
		for (size_t i = 0; i < n; i++) {
			double x = args[0].mat[i];
			ss += x * x;
		}
		*out = vec_value_number(vec_float(sqrt(ss)));
		goto done;
	}

	/* Stats builtins. */
	if (!strcmp(name, "cov")) {
		if (argc != 2 || args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: cov(x, y)");
			goto fail;
		}
		if (args[0].len != args[1].len) {
			snprintf(err, errsz, "eval: cov: length mismatch");
			goto fail;
		}
		size_t n = args[0].len;
		if (!n) {
			*out = vec_value_number(vec_float(NAN));
			goto done;
		}
		double mx = agg_avg(args[0].arr, n);
		double my = agg_avg(args[1].arr, n);
		double sum = 0;
		for (size_t i = 0; i < n; i++)
			sum += (args[0].arr[i] - mx) * (args[1].arr[i] - my);
		*out = vec_value_number(vec_float(sum / (double)n));
		goto done;
	}

	if (!strcmp(name, "corr")) {
		if (argc != 2 || args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: corr(x, y)");
			goto fail;
		}
		if (args[0].len != args[1].len) {
			snprintf(err, errsz, "eval: corr: length mismatch");
			goto fail;
		}
		size_t n = args[0].len;
		if (!n) {
			*out = vec_value_number(vec_float(NAN));
			goto done;
		}
		double mx = agg_avg(args[0].arr, n);
		double my = agg_avg(args[1].arr, n);
		double sum = 0;
		for (size_t i = 0; i < n; i++)
			sum += (args[0].arr[i] - mx) * (args[1].arr[i] - my);
		double c = sum / (double)n;
		double sx = agg_std(args[0].arr, n);
		double sy = agg_std(args[1].arr, n);
		if (sx == 0 || sy == 0 || isnan(sx) || isnan(sy)) {
			*out = vec_value_number(vec_float(NAN));
			goto done;
		}
		*out = vec_value_number(vec_float(c / (sx * sy)));
		goto done;
	}

	if (!strcmp(name, "hist")) {
		if (argc != 2 || args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: hist(data, bins)");
			goto fail;
		}
		int bins;
		if (require_int(vec_number_float64(args[1].num), 1, 8192, &bins, "hist bins", err, errsz) != 0)
			goto fail;

		size_t ndata = args[0].len;
		size_t outn = (size_t)bins * 2;
		double *outm = malloc(sizeof(outm[0]) * outn);
		if (!outm) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}

		if (!ndata) {
			for (int i = 0; i < bins; i++) {
				outm[(size_t)i * 2 + 0] = (double)i;
				outm[(size_t)i * 2 + 1] = 0;
			}
			*out = vec_value_matrix(bins, 2, outm);
			goto done;
		}

		double min = args[0].arr[0];
		double max = args[0].arr[0];
		for (size_t i = 1; i < ndata; i++) {
			double x = args[0].arr[i];
			if (x < min)
				min = x;
			if (x > max)
				max = x;
		}
		if (isnan(min) || isnan(max) || isinf(min) || isinf(max)) {
			free(outm);
			snprintf(err, errsz, "eval: hist: invalid data range");
			goto fail;
		}

		if (min == max) {
			for (int i = 0; i < bins; i++) {
				outm[(size_t)i * 2 + 0] = min;
				outm[(size_t)i * 2 + 1] = 0;
			}
			outm[1] = (double)ndata;
			*out = vec_value_matrix(bins, 2, outm);
			goto done;
		}

		double width = (max - min) / (double)bins;
		double *counts = calloc((size_t)bins, sizeof(counts[0]));
		if (!counts) {
			free(outm);
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}

		for (size_t k = 0; k < ndata; k++) {
			double x = args[0].arr[k];
			if (isnan(x) || isinf(x))
				continue;
			int i = (int)((x - min) / width);
			if (i < 0)
				i = 0;
			else if (i >= bins)
				i = bins - 1;
			counts[i] += 1;
		}

		for (int i = 0; i < bins; i++) {
			double center = min + ((double)i + 0.5) * width;
			outm[(size_t)i * 2 + 0] = center;
			outm[(size_t)i * 2 + 1] = counts[i];
		}
		free(counts);
		*out = vec_value_matrix(bins, 2, outm);
		goto done;
	}

	if (!strcmp(name, "convolve")) {
		if (argc != 2 || args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY) {
			snprintf(err, errsz, "eval: convolve(a, b)");
			goto fail;
		}
		size_t na = args[0].len;
		size_t nb = args[1].len;
		if (!na || !nb) {
			*out = vec_value_array(NULL, 0);
			goto done;
		}
		size_t n = na + nb - 1;
		double *xs = malloc(sizeof(xs[0]) * n);
		if (!xs) {
			snprintf(err, errsz, "eval: out of memory");
			goto fail;
		}
		memset(xs, 0, sizeof(xs[0]) * n);
		for (size_t i = 0; i < na; i++) {
			double av = args[0].arr[i];
			for (size_t j = 0; j < nb; j++) {
				xs[i + j] += av * args[1].arr[j];
			}
		}
		*out = vec_value_array(xs, n);
		goto done;
	}

	/* Unary scalar builtins. */
	if (argc == 1 && args[0].kind == VEC_VALUE_NUMBER) {
		unary_d_fn fn = unary_builtin_find(name);
		if (fn) {
			*out = vec_value_number(vec_float(fn(vec_number_float64(args[0].num))));
			goto done;
		}
	}

	/* Scalar builtins. */
	if (!strcmp(name, "pow") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: pow(a, b) expects numbers");
			goto fail;
		}
		double a = vec_number_float64(args[0].num);
		double b = vec_number_float64(args[1].num);
		*out = vec_value_number(vec_float(pow(a, b)));
		goto done;
	}
	if (!strcmp(name, "hypot") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: hypot(a, b) expects numbers");
			goto fail;
		}
		double a = vec_number_float64(args[0].num);
		double b = vec_number_float64(args[1].num);
		*out = vec_value_number(vec_float(sqrt(a * a + b * b)));
		goto done;
	}
	if (!strcmp(name, "copysign") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: copysign(mag, sign)");
			goto fail;
		}
		double mag = fabs(vec_number_float64(args[0].num));
		double sign_source = vec_number_float64(args[1].num);
		if (isnan(sign_source)) {
			*out = vec_value_number(vec_float(NAN));
			goto done;
		}
		if (sign_source < 0)
			mag = -mag;
		*out = vec_value_number(vec_float(mag));
		goto done;
	}
	if (!strcmp(name, "mod") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: mod(a, b) expects numbers");
			goto fail;
		}
		double a = vec_number_float64(args[0].num);
		double b = vec_number_float64(args[1].num);
		if (b == 0) {
			snprintf(err, errsz, "eval: mod: division by zero");
			goto fail;
		}
		*out = vec_value_number(vec_float(a - b * floor(a / b)));
		goto done;
	}
	if (!strcmp(name, "clamp") && argc == 3) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER || args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: clamp(x, lo, hi)");
			goto fail;
		}
		double x = vec_number_float64(args[0].num);
		double lo = vec_number_float64(args[1].num);
		double hi = vec_number_float64(args[2].num);
		if (lo > hi) {
			double tmp = lo;
			lo = hi;
			hi = tmp;
		}
		if (x < lo)
			x = lo;
		else if (x > hi)
			x = hi;
		*out = vec_value_number(vec_float(x));
		goto done;
	}
	if (!strcmp(name, "step") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: step(edge, x)");
			goto fail;
		}
		double edge = vec_number_float64(args[0].num);
		double x = vec_number_float64(args[1].num);
		*out = vec_value_number(vec_float(x < edge ? 0 : 1));
		goto done;
	}
	if (!strcmp(name, "smoothstep") && argc == 3) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER || args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: smoothstep(edge0, edge1, x)");
			goto fail;
		}
		double edge0 = vec_number_float64(args[0].num);
		double edge1 = vec_number_float64(args[1].num);
		double x = vec_number_float64(args[2].num);
		double t;
		if (edge0 == edge1) {
			t = x < edge0 ? 0 : 1;
		} else {
			t = (x - edge0) / (edge1 - edge0);
			if (t < 0)
				t = 0;
			else if (t > 1)
				t = 1;
			t = t * t * (3 - 2 * t);
		}
		*out = vec_value_number(vec_float(t));
		goto done;
	}
	if (!strcmp(name, "sum") && argc >= 1) {
		double total = 0;
		for (size_t i = 0; i < argc; i++) {
			if (args[i].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: sum expects numbers");
				goto fail;
			}
			total += vec_number_float64(args[i].num);
		}
		*out = vec_value_number(vec_float(total));
		goto done;
	}
	if ((!strcmp(name, "avg") || !strcmp(name, "mean")) && argc >= 1) {
		double total = 0;
		for (size_t i = 0; i < argc; i++) {
			if (args[i].kind != VEC_VALUE_NUMBER) {
				snprintf(err, errsz, "eval: avg expects numbers");
				goto fail;
			}
			total += vec_number_float64(args[i].num);
		}
		*out = vec_value_number(vec_float(total / (double)argc));
		goto done;
	}
	if (!strcmp(name, "min") && argc >= 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: min expects numbers");
			goto fail;
		}
		double m = vec_number_float64(args[0].num);
		for (size_t i = 1; i < argc; i++)
			if (args[i].kind == VEC_VALUE_NUMBER) {
				double x = vec_number_float64(args[i].num);
				if (x < m)
					m = x;
			} else {
				snprintf(err, errsz, "eval: min expects numbers");
				goto fail;
			}
		*out = vec_value_number(vec_float(m));
		goto done;
	}
	if (!strcmp(name, "max") && argc >= 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: max expects numbers");
			goto fail;
		}
		double m = vec_number_float64(args[0].num);
		for (size_t i = 1; i < argc; i++)
			if (args[i].kind == VEC_VALUE_NUMBER) {
				double x = vec_number_float64(args[i].num);
				if (x > m)
					m = x;
			} else {
				snprintf(err, errsz, "eval: max expects numbers");
				goto fail;
			}
		*out = vec_value_number(vec_float(m));
		goto done;
	}
	if (!strcmp(name, "atan2") && argc == 2) {
		if (args[0].kind != VEC_VALUE_NUMBER || args[1].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: atan2(y,x) expects numbers");
			goto fail;
		}
		*out = vec_value_number(vec_float(atan2(vec_number_float64(args[0].num), vec_number_float64(args[1].num))));
		goto done;
	}

	/* User-defined single-arg functions: f(x)=... */
	const vec_userfunc *uf;
	if (vec_env_get_func(e, name, &uf) == 0 && uf && uf->body && uf->param) {
		if (argc != 1) {
			snprintf(err, errsz, "eval: %s expects 1 argument", name);
			goto fail;
		}
		rc = vec_eval_node_impl(e, uf->body, uf->param, &args[0], out, err, errsz);
		goto done_rc;
	}

	snprintf(err, errsz, "eval: unknown function '%s'", name);
	goto fail;

done:
	for (size_t i = 0; i < argc; i++)
		vec_value_destroy(&args[i]);
	free(args);
	return 0;

done_rc: {
		for (size_t i = 0; i < argc; i++)
			vec_value_destroy(&args[i]);
		free(args);
		return rc;
	}

fail:
	for (size_t i = 0; i < argc; i++)
		vec_value_destroy(&args[i]);
	free(args);
	return -1;
}
#endif

static int eval_compare(vec_env *e, vec_cmp_op op, const vec_value *a, const vec_value *b,
			vec_value *out, char *err, size_t errsz)
{
	(void)e;
	if (!a || !b || !out) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}

	int ok = 0;
	if (a->kind == VEC_VALUE_COMPLEX || b->kind == VEC_VALUE_COMPLEX) {
		vec_complex za, zb;
		if (value_to_complex(a, &za) != 0 || value_to_complex(b, &zb) != 0) {
			snprintf(err, errsz, "eval: unsupported complex comparison");
			return -1;
		}
		switch (op) {
		case VEC_CMP_EQ: ok = (za.re == zb.re && za.im == zb.im); break;
		case VEC_CMP_NE: ok = !(za.re == zb.re && za.im == zb.im); break;
		default:
			snprintf(err, errsz, "eval: unsupported complex comparison");
			return -1;
		}
	} else if (a->kind == VEC_VALUE_NUMBER && b->kind == VEC_VALUE_NUMBER) {
		double af = vec_number_float64(a->num);
		double bf = vec_number_float64(b->num);
		switch (op) {
		case VEC_CMP_EQ: ok = (af == bf); break;
		case VEC_CMP_NE: ok = (af != bf); break;
		case VEC_CMP_LT: ok = (af < bf); break;
		case VEC_CMP_LE: ok = (af <= bf); break;
		case VEC_CMP_GT: ok = (af > bf); break;
		case VEC_CMP_GE: ok = (af >= bf); break;
		default:
			snprintf(err, errsz, "eval: bad compare op");
			return -1;
		}
	} else {
		snprintf(err, errsz, "eval: unsupported comparison");
		return -1;
	}

	*out = vec_value_number(vec_rat_number(vec_rat_int(ok ? 1 : 0)));
	return 0;
}

static int vec_eval_node_impl(vec_env *e, const vec_node *n, const char *ov_name, const vec_value *ov_value,
			      vec_value *out, char *err, size_t errsz)
{
	if (!e || !n || !out) {
		snprintf(err, errsz, "eval: bad args");
		return -1;
	}

	switch (n->kind) {
	case VEC_NODE_NUMBER:
		*out = vec_value_number(n->u.number);
		return 0;
	case VEC_NODE_IDENT:
		if (ov_name && n->u.ident.name && !strcmp(n->u.ident.name, ov_name)) {
			if (!ov_value || vec_value_clone(out, ov_value) != 0) {
				snprintf(err, errsz, "eval: out of memory");
				return -1;
			}
			return 0;
		}
		{
			int rc = vec_env_get_var(e, n->u.ident.name, out);
			if (rc == 0)
				return 0;
			if (rc < 0) {
				snprintf(err, errsz, "eval: out of memory");
				return -1;
			}
		}
		snprintf(err, errsz, "eval: unknown variable '%s'", n->u.ident.name ? n->u.ident.name : "?");
		return -1;
	case VEC_NODE_UNARY: {
		vec_value x;
		memset(&x, 0, sizeof(x));
		if (vec_eval_node_impl(e, n->u.unary.x, ov_name, ov_value, &x, err, errsz) != 0)
			return -1;
		switch (x.kind) {
		case VEC_VALUE_COMPLEX:
			switch (n->u.unary.op) {
			case '+':
				*out = x;
				x = vec_value_number(vec_float(0));
				vec_value_destroy(&x);
				return 0;
			case '-':
				*out = vec_value_complex(-x.c.re, -x.c.im);
				vec_value_destroy(&x);
				return 0;
			default:
				snprintf(err, errsz, "eval: unary %q", n->u.unary.op);
				vec_value_destroy(&x);
				return -1;
			}
		case VEC_VALUE_EXPR: {
			vec_node *child = value_to_node_take(&x);
			if (!child) {
				snprintf(err, errsz, "eval: out of memory");
				vec_value_destroy(&x);
				return -1;
			}
			vec_node *tmp = vec_node_unary_new(n->u.unary.op, child);
			vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
			if (!simp) {
				snprintf(err, errsz, "eval: out of memory");
				vec_value_destroy(&x);
				return -1;
			}
			*out = vec_value_expr(simp);
			vec_value_destroy(&x);
			return 0;
		}
		case VEC_VALUE_ARRAY:
			switch (n->u.unary.op) {
			case '+':
				*out = x;
				x = vec_value_number(vec_float(0));
				vec_value_destroy(&x);
				return 0;
			case '-': {
				size_t n = x.len;
				double *xs = n ? malloc(sizeof(xs[0]) * n) : NULL;
				if (n && !xs) {
					snprintf(err, errsz, "eval: out of memory");
					vec_value_destroy(&x);
					return -1;
				}
				for (size_t i = 0; i < n; i++)
					xs[i] = -x.arr[i];
				*out = vec_value_array(xs, n);
				vec_value_destroy(&x);
				return 0;
			}
			default:
				snprintf(err, errsz, "eval: unary %q", n->u.unary.op);
				vec_value_destroy(&x);
				return -1;
			}
		case VEC_VALUE_MATRIX:
			switch (n->u.unary.op) {
			case '+':
				*out = x;
				x = vec_value_number(vec_float(0));
				vec_value_destroy(&x);
				return 0;
			case '-': {
				size_t n = (size_t)x.rows * (size_t)x.cols;
				double *m = n ? malloc(sizeof(m[0]) * n) : NULL;
				if (n && !m) {
					snprintf(err, errsz, "eval: out of memory");
					vec_value_destroy(&x);
					return -1;
				}
				for (size_t i = 0; i < n; i++)
					m[i] = -x.mat[i];
				*out = vec_value_matrix(x.rows, x.cols, m);
				vec_value_destroy(&x);
				return 0;
			}
			default:
				snprintf(err, errsz, "eval: unary %q", n->u.unary.op);
				vec_value_destroy(&x);
				return -1;
			}
		case VEC_VALUE_NUMBER:
			switch (n->u.unary.op) {
			case '+':
				*out = x;
				x = vec_value_number(vec_float(0));
				vec_value_destroy(&x);
				return 0;
			case '-':
				*out = vec_value_number(neg_number(x.num));
				vec_value_destroy(&x);
				return 0;
			default:
				snprintf(err, errsz, "eval: unary %q", n->u.unary.op);
				vec_value_destroy(&x);
				return -1;
			}
		default:
			snprintf(err, errsz, "eval: unsupported unary");
			vec_value_destroy(&x);
			return -1;
		}
	}
	case VEC_NODE_BINARY: {
		vec_value a, b;
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		if (vec_eval_node_impl(e, n->u.binary.left, ov_name, ov_value, &a, err, errsz) != 0)
			return -1;
		if (vec_eval_node_impl(e, n->u.binary.right, ov_name, ov_value, &b, err, errsz) != 0) {
			vec_value_destroy(&a);
			return -1;
		}
		if (a.kind == VEC_VALUE_COMPLEX || b.kind == VEC_VALUE_COMPLEX) {
			vec_complex za, zb;
			if (value_to_complex(&a, &za) != 0 || value_to_complex(&b, &zb) != 0) {
				snprintf(err, errsz, "eval: unsupported complex operand");
				goto bin_fail;
			}
			switch (n->u.binary.op) {
			case '+': {
				vec_complex z = c_add(za, zb);
				*out = vec_value_complex(z.re, z.im);
				goto bin_ok;
			}
			case '-': {
				vec_complex z = c_sub(za, zb);
				*out = vec_value_complex(z.re, z.im);
				goto bin_ok;
			}
			case '*': {
				vec_complex z = c_mul(za, zb);
				*out = vec_value_complex(z.re, z.im);
				goto bin_ok;
			}
			case '/':
				if (c_is_zero(zb)) {
					snprintf(err, errsz, "eval: division by zero");
					goto bin_fail;
				}
				{
					vec_complex z = c_div(za, zb);
					*out = vec_value_complex(z.re, z.im);
				}
				goto bin_ok;
			case '^': {
				vec_complex z = c_pow(za, zb);
				*out = vec_value_complex(z.re, z.im);
				goto bin_ok;
			}
			default:
				snprintf(err, errsz, "eval: binary %q", n->u.binary.op);
				goto bin_fail;
			}
		}

		if (a.kind == VEC_VALUE_MATRIX || b.kind == VEC_VALUE_MATRIX) {
			if (a.kind == VEC_VALUE_MATRIX && b.kind == VEC_VALUE_MATRIX) {
				vec_mat_err mrc;
				double *m = NULL;
				switch (n->u.binary.op) {
				case '+':
					if (a.rows != b.rows || a.cols != b.cols) {
						snprintf(err, errsz, "eval: matrix shape mismatch");
						goto bin_fail;
					}
					mrc = vec_matrix_add(a.rows, a.cols, a.mat, b.mat, &m);
					break;
				case '-':
					if (a.rows != b.rows || a.cols != b.cols) {
						snprintf(err, errsz, "eval: matrix shape mismatch");
						goto bin_fail;
					}
					mrc = vec_matrix_sub(a.rows, a.cols, a.mat, b.mat, &m);
					break;
				case '*':
					mrc = vec_matrix_mul(a.rows, a.cols, a.mat, b.rows, b.cols, b.mat, &m);
					break;
				default:
					snprintf(err, errsz, "eval: unsupported matrix operation");
					goto bin_fail;
				}
				if (mrc == VEC_MAT_ERR_NOMEM) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				if (mrc == VEC_MAT_ERR_SHAPE) {
					snprintf(err, errsz, "eval: matrix shape mismatch");
					goto bin_fail;
				}
				if (mrc != VEC_MAT_OK) {
					snprintf(err, errsz, "eval: invalid matrix");
					goto bin_fail;
				}
				if (n->u.binary.op == '*')
					*out = vec_value_matrix(a.rows, b.cols, m);
				else
					*out = vec_value_matrix(a.rows, a.cols, m);
				goto bin_ok;
			}
			if (a.kind == VEC_VALUE_MATRIX && b.kind == VEC_VALUE_NUMBER) {
				double bf = vec_number_float64(b.num);
				double *m = NULL;
				vec_mat_err mrc;
				switch (n->u.binary.op) {
				case '*':
					mrc = vec_matrix_scale(a.rows, a.cols, a.mat, bf, &m);
					break;
				case '/':
					if (bf == 0) {
						snprintf(err, errsz, "eval: division by zero");
						goto bin_fail;
					}
					mrc = vec_matrix_scale(a.rows, a.cols, a.mat, 1 / bf, &m);
					break;
				case '+':
				case '-': {
					size_t elem_count = (size_t)a.rows * (size_t)a.cols;
					m = elem_count ? malloc(sizeof(m[0]) * elem_count) : NULL;
					if (elem_count && !m) {
						snprintf(err, errsz, "eval: out of memory");
						goto bin_fail;
					}
					vec_number bn = vec_float(bf);
					for (size_t i = 0; i < elem_count; i++) {
						vec_number r;
						vec_number an = vec_float(a.mat[i]);
						if (n->u.binary.op == '+')
							r = add_number(e, an, bn);
						else
							r = sub_number(e, an, bn);
						m[i] = vec_number_float64(r);
					}
					*out = vec_value_matrix(a.rows, a.cols, m);
					goto bin_ok;
				}
				default:
					snprintf(err, errsz, "eval: unsupported matrix operation");
					goto bin_fail;
				}
				if (mrc == VEC_MAT_ERR_NOMEM) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				if (mrc != VEC_MAT_OK) {
					snprintf(err, errsz, "eval: invalid matrix");
					goto bin_fail;
				}
				*out = vec_value_matrix(a.rows, a.cols, m);
				goto bin_ok;
			}
			if (a.kind == VEC_VALUE_NUMBER && b.kind == VEC_VALUE_MATRIX) {
				double af = vec_number_float64(a.num);
				double *m = NULL;
				vec_mat_err mrc;
				switch (n->u.binary.op) {
				case '*':
					mrc = vec_matrix_scale(b.rows, b.cols, b.mat, af, &m);
					break;
				case '+':
				case '-': {
					size_t elem_count = (size_t)b.rows * (size_t)b.cols;
					m = elem_count ? malloc(sizeof(m[0]) * elem_count) : NULL;
					if (elem_count && !m) {
						snprintf(err, errsz, "eval: out of memory");
						goto bin_fail;
					}
					vec_number an = vec_float(af);
					for (size_t i = 0; i < elem_count; i++) {
						vec_number r;
						vec_number bn = vec_float(b.mat[i]);
						if (n->u.binary.op == '+')
							r = add_number(e, an, bn);
						else
							r = sub_number(e, an, bn);
						m[i] = vec_number_float64(r);
					}
					*out = vec_value_matrix(b.rows, b.cols, m);
					goto bin_ok;
				}
				default:
					snprintf(err, errsz, "eval: unsupported matrix operation");
					goto bin_fail;
				}
				if (mrc == VEC_MAT_ERR_NOMEM) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				if (mrc != VEC_MAT_OK) {
					snprintf(err, errsz, "eval: invalid matrix");
					goto bin_fail;
				}
				*out = vec_value_matrix(b.rows, b.cols, m);
				goto bin_ok;
			}
			if (a.kind == VEC_VALUE_MATRIX && b.kind == VEC_VALUE_ARRAY && n->u.binary.op == '*') {
				double *xs = NULL;
				vec_mat_err mrc = vec_matrix_mul_vec(a.rows, a.cols, a.mat, b.arr, b.len, &xs);
				if (mrc == VEC_MAT_ERR_NOMEM) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				if (mrc == VEC_MAT_ERR_SHAPE) {
					snprintf(err, errsz, "eval: matrix shape mismatch");
					goto bin_fail;
				}
				if (mrc != VEC_MAT_OK) {
					snprintf(err, errsz, "eval: invalid matrix");
					goto bin_fail;
				}
				*out = vec_value_array(xs, (size_t)a.rows);
				goto bin_ok;
			}
			snprintf(err, errsz, "eval: unsupported matrix operation");
			goto bin_fail;
		}

		if (a.kind == VEC_VALUE_ARRAY || b.kind == VEC_VALUE_ARRAY) {
			const vec_value *aa = &a;
			const vec_value *bb = &b;
			if (aa->kind == VEC_VALUE_ARRAY && bb->kind == VEC_VALUE_ARRAY) {
				if (aa->len != bb->len) {
					snprintf(err, errsz, "eval: array length mismatch");
					goto bin_fail;
				}
				size_t len = aa->len;
				double *xs = len ? malloc(sizeof(xs[0]) * len) : NULL;
				if (len && !xs) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				for (size_t i = 0; i < len; i++) {
					vec_number r;
					vec_number an = vec_float(aa->arr[i]);
					vec_number bn = vec_float(bb->arr[i]);
					switch (n->u.binary.op) {
					case '+':
						r = add_number(e, an, bn);
						break;
					case '-':
						r = sub_number(e, an, bn);
						break;
					case '*':
						r = mul_number(e, an, bn);
						break;
					case '/':
						if (div_number(e, an, bn, &r, err, errsz) != 0) {
							free(xs);
							goto bin_fail;
						}
						break;
					case '^':
						r = pow_number(e, an, bn);
						break;
					default:
						snprintf(err, errsz, "eval: unsupported array operation");
						free(xs);
						goto bin_fail;
					}
					xs[i] = vec_number_float64(r);
				}
				*out = vec_value_array(xs, len);
				goto bin_ok;
			}
			if (aa->kind == VEC_VALUE_ARRAY && bb->kind == VEC_VALUE_NUMBER) {
				size_t len = aa->len;
				double *xs = len ? malloc(sizeof(xs[0]) * len) : NULL;
				if (len && !xs) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				double bf = vec_number_float64(bb->num);
				for (size_t i = 0; i < len; i++) {
					vec_number r;
					vec_number an = vec_float(aa->arr[i]);
					vec_number bn = vec_float(bf);
					switch (n->u.binary.op) {
					case '+':
						r = add_number(e, an, bn);
						break;
					case '-':
						r = sub_number(e, an, bn);
						break;
					case '*':
						r = mul_number(e, an, bn);
						break;
					case '/':
						if (div_number(e, an, bn, &r, err, errsz) != 0) {
							free(xs);
							goto bin_fail;
						}
						break;
					case '^':
						r = pow_number(e, an, bn);
						break;
					default:
						snprintf(err, errsz, "eval: unsupported array operation");
						free(xs);
						goto bin_fail;
					}
					xs[i] = vec_number_float64(r);
				}
				*out = vec_value_array(xs, len);
				goto bin_ok;
			}
			if (aa->kind == VEC_VALUE_NUMBER && bb->kind == VEC_VALUE_ARRAY) {
				size_t len = bb->len;
				double *xs = len ? malloc(sizeof(xs[0]) * len) : NULL;
				if (len && !xs) {
					snprintf(err, errsz, "eval: out of memory");
					goto bin_fail;
				}
				double af = vec_number_float64(aa->num);
				for (size_t i = 0; i < len; i++) {
					vec_number r;
					vec_number an = vec_float(af);
					vec_number bn = vec_float(bb->arr[i]);
					switch (n->u.binary.op) {
					case '+':
						r = add_number(e, an, bn);
						break;
					case '-':
						r = sub_number(e, an, bn);
						break;
					case '*':
						r = mul_number(e, an, bn);
						break;
					case '/':
						if (div_number(e, an, bn, &r, err, errsz) != 0) {
							free(xs);
							goto bin_fail;
						}
						break;
					case '^':
						r = pow_number(e, an, bn);
						break;
					default:
						snprintf(err, errsz, "eval: unsupported array operation");
						free(xs);
						goto bin_fail;
					}
					xs[i] = vec_number_float64(r);
				}
				*out = vec_value_array(xs, len);
				goto bin_ok;
			}
			snprintf(err, errsz, "eval: unsupported array operation");
			goto bin_fail;
		}

		if (a.kind == VEC_VALUE_EXPR || b.kind == VEC_VALUE_EXPR) {
			vec_node *ln = value_to_node_take(&a);
			vec_node *rn = value_to_node_take(&b);
			if (!ln || !rn) {
				vec_node_destroy(ln);
				vec_node_destroy(rn);
				snprintf(err, errsz, "eval: out of memory");
				goto bin_fail;
			}
			vec_node *tmp = vec_node_binary_new(n->u.binary.op, ln, rn);
			vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
			if (!simp) {
				snprintf(err, errsz, "eval: out of memory");
				goto bin_fail;
			}
			*out = vec_value_expr(simp);
			goto bin_ok;
		}

		if (a.kind != VEC_VALUE_NUMBER || b.kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: unsupported binary");
			goto bin_fail;
		}

		switch (n->u.binary.op) {
		case '+':
			*out = vec_value_number(add_number(e, a.num, b.num));
			goto bin_ok;
		case '-':
			*out = vec_value_number(sub_number(e, a.num, b.num));
			goto bin_ok;
		case '*':
			*out = vec_value_number(mul_number(e, a.num, b.num));
			goto bin_ok;
		case '/': {
			vec_number r;
			if (div_number(e, a.num, b.num, &r, err, errsz) != 0)
				goto bin_fail;
			*out = vec_value_number(r);
			goto bin_ok;
		}
		case '^':
			*out = vec_value_number(pow_number(e, a.num, b.num));
			goto bin_ok;
		default:
			snprintf(err, errsz, "eval: bad binary op");
			goto bin_fail;
		}

bin_ok:
		vec_value_destroy(&a);
		vec_value_destroy(&b);
		return 0;
bin_fail:
		vec_value_destroy(&a);
		vec_value_destroy(&b);
		return -1;
	}
	case VEC_NODE_CALL:
		return eval_call(e, n, ov_name, ov_value, out, err, errsz);
	case VEC_NODE_COMPARE: {
		vec_value a, b;
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		if (vec_eval_node_impl(e, n->u.compare.left, ov_name, ov_value, &a, err, errsz) != 0)
			return -1;
		if (vec_eval_node_impl(e, n->u.compare.right, ov_name, ov_value, &b, err, errsz) != 0) {
			vec_value_destroy(&a);
			return -1;
		}
		if (a.kind == VEC_VALUE_EXPR || b.kind == VEC_VALUE_EXPR) {
			vec_node *ln = value_to_node_take(&a);
			vec_node *rn = value_to_node_take(&b);
			if (!ln || !rn) {
				vec_node_destroy(ln);
				vec_node_destroy(rn);
				snprintf(err, errsz, "eval: out of memory");
				vec_value_destroy(&a);
				vec_value_destroy(&b);
				return -1;
			}
			vec_node *tmp = vec_node_compare_new(n->u.compare.op, ln, rn);
			vec_node *simp = tmp ? vec_node_simplify_owned(tmp) : NULL;
			if (!simp) {
				snprintf(err, errsz, "eval: out of memory");
				vec_value_destroy(&a);
				vec_value_destroy(&b);
				return -1;
			}
			*out = vec_value_expr(simp);
			vec_value_destroy(&a);
			vec_value_destroy(&b);
			return 0;
		}
		int rc = eval_compare(e, n->u.compare.op, &a, &b, out, err, errsz);
		vec_value_destroy(&a);
		vec_value_destroy(&b);
		return rc;
	}
	default:
		snprintf(err, errsz, "eval: unknown node");
		return -1;
	}
}

int vec_eval_node(vec_env *e, const vec_node *n, vec_value *out, char *err, size_t errsz)
{
	vec_value dummy;
	memset(&dummy, 0, sizeof(dummy));
	return vec_eval_node_impl(e, n, NULL, &dummy, out, err, errsz);
}

int vec_eval_node_override(vec_env *e, const vec_node *n, const char *ov_name, vec_value ov_value,
			   vec_value *out, char *err, size_t errsz)
{
	return vec_eval_node_impl(e, n, ov_name, &ov_value, out, err, errsz);
}
