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
				if (c_is_zero(c)) {
					snprintf(err, errsz, "eval: tan: division by zero");
					goto fail;
				}
				vec_complex w = c_div(s, c);
				*out = vec_value_complex(w.re, w.im);
				goto done;
			}
		}
	}

	/* Builtins (minimal subset). */
	if (!strcmp(name, "sin") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: sin(x) expects number");
			goto fail;
		}
		*out = vec_value_number(vec_float(sin(vec_number_float64(args[0].num))));
		goto done;
	}
	if (!strcmp(name, "cos") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: cos(x) expects number");
			goto fail;
		}
		*out = vec_value_number(vec_float(cos(vec_number_float64(args[0].num))));
		goto done;
	}
	if (!strcmp(name, "tan") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: tan(x) expects number");
			goto fail;
		}
		double x = vec_number_float64(args[0].num);
		double c = cos(x);
		if (c == 0) {
			snprintf(err, errsz, "eval: tan: division by zero");
			goto fail;
		}
		*out = vec_value_number(vec_float(sin(x) / c));
		goto done;
	}
	if ((!strcmp(name, "ln") || !strcmp(name, "log")) && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: ln(x) expects number");
			goto fail;
		}
		*out = vec_value_number(vec_float(log(vec_number_float64(args[0].num))));
		goto done;
	}
	if (!strcmp(name, "exp") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: exp(x) expects number");
			goto fail;
		}
		*out = vec_value_number(vec_float(exp(vec_number_float64(args[0].num))));
		goto done;
	}
	if (!strcmp(name, "sqrt") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: sqrt(x) expects number");
			goto fail;
		}
		*out = vec_value_number(vec_float(sqrt(vec_number_float64(args[0].num))));
		goto done;
	}
	if (!strcmp(name, "abs") && argc == 1) {
		if (args[0].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: abs(x) expects number");
			goto fail;
		}
		*out = vec_value_number(vec_float(fabs(vec_number_float64(args[0].num))));
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
