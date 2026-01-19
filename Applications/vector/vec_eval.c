#include "vec_eval.h"
#include "vec_cas.h"

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
		if (args[0].kind != VEC_VALUE_ARRAY || args[1].kind != VEC_VALUE_ARRAY || args[2].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: lerp(a, b, t)");
			goto fail;
		}
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
