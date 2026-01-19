#include "vec_poly_rat.h"

#include "vec_cas.h"
#include "vec_value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double trunc_d(double x)
{
	if (x < 0)
		return ceil(x);
	return floor(x);
}

static int rat_is_zero(vec_rat a) { return a.num == 0; }
static vec_rat rat_zero(void) { return vec_rat_int(0); }
static vec_rat rat_one(void) { return vec_rat_int(1); }
static vec_rat rat_neg(vec_rat a) { vec_rat r = a; r.num = -r.num; return r; }

static int rat_from_number(vec_number n, vec_rat *out)
{
	if (!out)
		return 0;
	if (n.kind == VEC_NUM_RAT) {
		*out = n.r;
		return 1;
	}
	double f = n.f;
	if (isnan(f) || isinf(f) || f != trunc_d(f))
		return 0;
	if (f < (double)INT64_MIN || f > (double)INT64_MAX)
		return 0;
	*out = vec_rat_int((int64_t)f);
	return 1;
}

static int rat_err(vec_num_err e, char *err, size_t errsz)
{
	switch (e) {
	case VEC_NUM_DIV0:
		snprintf(err, errsz, "division by zero");
		return -1;
	case VEC_NUM_OVERFLOW:
		snprintf(err, errsz, "overflow");
		return -1;
	default:
		snprintf(err, errsz, "error");
		return -1;
	}
}

static int rat_add(vec_rat a, vec_rat b, vec_rat *out, char *err, size_t errsz)
{
	vec_num_err rc = vec_rat_add(a, b, out);
	if (rc != VEC_NUM_OK)
		return rat_err(rc, err, errsz);
	return 0;
}

static int rat_sub(vec_rat a, vec_rat b, vec_rat *out, char *err, size_t errsz)
{
	vec_num_err rc = vec_rat_sub(a, b, out);
	if (rc != VEC_NUM_OK)
		return rat_err(rc, err, errsz);
	return 0;
}

static int rat_mul(vec_rat a, vec_rat b, vec_rat *out, char *err, size_t errsz)
{
	vec_num_err rc = vec_rat_mul(a, b, out);
	if (rc != VEC_NUM_OK)
		return rat_err(rc, err, errsz);
	return 0;
}

static int rat_div(vec_rat a, vec_rat b, vec_rat *out, char *err, size_t errsz)
{
	vec_num_err rc = vec_rat_div(a, b, out);
	if (rc != VEC_NUM_OK)
		return rat_err(rc, err, errsz);
	return 0;
}

static void poly_trim(vec_poly_rat *p)
{
	if (!p || !p->coeffs)
		return;
	size_t n = p->len;
	while (n > 0 && rat_is_zero(p->coeffs[n - 1]))
		n--;
	if (n == p->len)
		return;
	if (n == 0) {
		free(p->coeffs);
		p->coeffs = NULL;
		p->len = 0;
		return;
	}
	vec_rat *nc = realloc(p->coeffs, sizeof(nc[0]) * n);
	if (nc) {
		p->coeffs = nc;
		p->len = n;
	} else {
		p->len = n;
	}
}

void vec_poly_rat_destroy(vec_poly_rat *p)
{
	if (!p)
		return;
	free(p->coeffs);
	p->coeffs = NULL;
	p->len = 0;
}

int vec_poly_rat_degree(const vec_poly_rat *p)
{
	if (!p || !p->coeffs || p->len == 0)
		return -1;
	for (size_t i = p->len; i > 0; i--) {
		if (!rat_is_zero(p->coeffs[i - 1]))
			return (int)(i - 1);
	}
	return -1;
}

static int poly_add(const vec_poly_rat *a, const vec_poly_rat *b, vec_poly_rat *out, char *err, size_t errsz)
{
	size_t n = a->len;
	if (b->len > n)
		n = b->len;
	vec_rat *c = n ? malloc(sizeof(c[0]) * n) : NULL;
	if (n && !c) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		vec_rat av = rat_zero();
		vec_rat bv = rat_zero();
		if (i < a->len)
			av = a->coeffs[i];
		if (i < b->len)
			bv = b->coeffs[i];
		vec_rat s;
		if (rat_add(av, bv, &s, err, errsz) != 0) {
			free(c);
			return -1;
		}
		c[i] = s;
	}
	out->coeffs = c;
	out->len = n;
	poly_trim(out);
	return 0;
}

static int poly_sub(const vec_poly_rat *a, const vec_poly_rat *b, vec_poly_rat *out, char *err, size_t errsz)
{
	size_t n = a->len;
	if (b->len > n)
		n = b->len;
	vec_rat *c = n ? malloc(sizeof(c[0]) * n) : NULL;
	if (n && !c) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		vec_rat av = rat_zero();
		vec_rat bv = rat_zero();
		if (i < a->len)
			av = a->coeffs[i];
		if (i < b->len)
			bv = b->coeffs[i];
		vec_rat s;
		if (rat_sub(av, bv, &s, err, errsz) != 0) {
			free(c);
			return -1;
		}
		c[i] = s;
	}
	out->coeffs = c;
	out->len = n;
	poly_trim(out);
	return 0;
}

int vec_poly_rat_mul(const vec_poly_rat *a, const vec_poly_rat *b, vec_poly_rat *out, char *err, size_t errsz)
{
	out->coeffs = NULL;
	out->len = 0;
	if (!a || !b) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	if (a->len == 0 || b->len == 0)
		return 0;
	size_t n = a->len + b->len - 1;
	vec_rat *c = malloc(sizeof(c[0]) * n);
	if (!c) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	for (size_t i = 0; i < n; i++)
		c[i] = rat_zero();

	for (size_t i = 0; i < a->len; i++) {
		vec_rat ca = a->coeffs[i];
		if (rat_is_zero(ca))
			continue;
		for (size_t j = 0; j < b->len; j++) {
			vec_rat cb = b->coeffs[j];
			if (rat_is_zero(cb))
				continue;
			vec_rat p;
			if (rat_mul(ca, cb, &p, err, errsz) != 0) {
				free(c);
				return -1;
			}
			vec_rat s;
			if (rat_add(c[i + j], p, &s, err, errsz) != 0) {
				free(c);
				return -1;
			}
			c[i + j] = s;
		}
	}
	out->coeffs = c;
	out->len = n;
	poly_trim(out);
	return 0;
}

static int poly_pow(const vec_poly_rat *p, int exp, vec_poly_rat *out, char *err, size_t errsz)
{
	if (exp == 0) {
		vec_rat *c = malloc(sizeof(c[0]));
		if (!c) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		c[0] = rat_one();
		out->coeffs = c;
		out->len = 1;
		return 0;
	}
	if (exp == 1) {
		vec_rat *c = p->len ? malloc(sizeof(c[0]) * p->len) : NULL;
		if (p->len && !c) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		if (p->len)
			memcpy(c, p->coeffs, sizeof(c[0]) * p->len);
		out->coeffs = c;
		out->len = p->len;
		poly_trim(out);
		return 0;
	}

	vec_poly_rat base = {0};
	if (poly_pow(p, 1, &base, err, errsz) != 0)
		return -1;
	vec_poly_rat acc = {0};
	if (poly_pow(p, 0, &acc, err, errsz) != 0) {
		vec_poly_rat_destroy(&base);
		return -1;
	}

	int e = exp;
	while (e > 0) {
		if (e & 1) {
			vec_poly_rat tmp = {0};
			if (vec_poly_rat_mul(&acc, &base, &tmp, err, errsz) != 0) {
				vec_poly_rat_destroy(&acc);
				vec_poly_rat_destroy(&base);
				return -1;
			}
			vec_poly_rat_destroy(&acc);
			acc = tmp;
		}
		e >>= 1;
		if (!e)
			break;
		{
			vec_poly_rat tmp = {0};
			if (vec_poly_rat_mul(&base, &base, &tmp, err, errsz) != 0) {
				vec_poly_rat_destroy(&acc);
				vec_poly_rat_destroy(&base);
				return -1;
			}
			vec_poly_rat_destroy(&base);
			base = tmp;
		}
	}

	vec_poly_rat_destroy(&base);
	*out = acc;
	poly_trim(out);
	return 0;
}

static int poly_from_node(vec_env *e, const vec_node *ex, const char *var_name,
			  vec_poly_rat *out, int *ok, char *err, size_t errsz)
{
	if (!out || !ok) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	out->coeffs = NULL;
	out->len = 0;
	*ok = 0;
	if (!e || !ex || !var_name) {
		snprintf(err, errsz, "bad args");
		return -1;
	}

	switch (ex->kind) {
	case VEC_NODE_NUMBER: {
		vec_rat r;
		if (!rat_from_number(ex->u.number, &r))
			return 0;
		vec_rat *c = malloc(sizeof(c[0]));
		if (!c) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		c[0] = r;
		out->coeffs = c;
		out->len = 1;
		*ok = 1;
		return 0;
	}
	case VEC_NODE_IDENT: {
		if (ex->u.ident.name && !strcmp(ex->u.ident.name, var_name)) {
			vec_rat *c = malloc(sizeof(c[0]) * 2);
			if (!c) {
				snprintf(err, errsz, "out of memory");
				return -1;
			}
			c[0] = rat_zero();
			c[1] = rat_one();
			out->coeffs = c;
			out->len = 2;
			*ok = 1;
			return 0;
		}
		if (!ex->u.ident.name)
			return 0;
		vec_value v;
		memset(&v, 0, sizeof(v));
		int rc = vec_env_get_var(e, ex->u.ident.name, &v);
		if (rc != 0 || v.kind != VEC_VALUE_NUMBER) {
			vec_value_destroy(&v);
			return 0;
		}
		vec_rat r;
		if (!rat_from_number(v.num, &r)) {
			vec_value_destroy(&v);
			return 0;
		}
		vec_value_destroy(&v);

		vec_rat *c = malloc(sizeof(c[0]));
		if (!c) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		c[0] = r;
		out->coeffs = c;
		out->len = 1;
		*ok = 1;
		return 0;
	}
	case VEC_NODE_UNARY: {
		vec_poly_rat p = {0};
		int ok_inner = 0;
		if (poly_from_node(e, ex->u.unary.x, var_name, &p, &ok_inner, err, errsz) != 0)
			return -1;
		if (!ok_inner) {
			vec_poly_rat_destroy(&p);
			return 0;
		}
		switch (ex->u.unary.op) {
		case '+':
			*out = p;
			*ok = 1;
			return 0;
		case '-':
			for (size_t i = 0; i < p.len; i++)
				p.coeffs[i] = rat_neg(p.coeffs[i]);
			*out = p;
			*ok = 1;
			return 0;
		default:
			vec_poly_rat_destroy(&p);
			return 0;
		}
	}
	case VEC_NODE_BINARY: {
		vec_poly_rat a = {0};
		vec_poly_rat b = {0};
		int ok_a = 0;
		int ok_b = 0;
		if (poly_from_node(e, ex->u.binary.left, var_name, &a, &ok_a, err, errsz) != 0)
			return -1;
		if (poly_from_node(e, ex->u.binary.right, var_name, &b, &ok_b, err, errsz) != 0) {
			vec_poly_rat_destroy(&a);
			return -1;
		}
		if (!ok_a || !ok_b) {
			vec_poly_rat_destroy(&a);
			vec_poly_rat_destroy(&b);
			return 0;
		}
		char op = ex->u.binary.op;
		vec_poly_rat res = {0};
		int rc = 0;
		switch (op) {
		case '+':
			rc = poly_add(&a, &b, &res, err, errsz);
			break;
		case '-':
			rc = poly_sub(&a, &b, &res, err, errsz);
			break;
		case '*':
			rc = vec_poly_rat_mul(&a, &b, &res, err, errsz);
			break;
		case '^': {
			if (!ex->u.binary.right || ex->u.binary.right->kind != VEC_NODE_NUMBER) {
				vec_poly_rat_destroy(&a);
				vec_poly_rat_destroy(&b);
				return 0;
			}
			double expf = vec_number_float64(ex->u.binary.right->u.number);
			if (isnan(expf) || isinf(expf) || expf != trunc_d(expf)) {
				vec_poly_rat_destroy(&a);
				vec_poly_rat_destroy(&b);
				return 0;
			}
			int exp = (int)expf;
			if (exp < 0 || exp > 64) {
				vec_poly_rat_destroy(&a);
				vec_poly_rat_destroy(&b);
				return 0;
			}
			rc = poly_pow(&a, exp, &res, err, errsz);
			break;
		}
		default:
			vec_poly_rat_destroy(&a);
			vec_poly_rat_destroy(&b);
			return 0;
		}
		vec_poly_rat_destroy(&a);
		vec_poly_rat_destroy(&b);
		if (rc != 0) {
			vec_poly_rat_destroy(&res);
			return -1;
		}
		*out = res;
		*ok = 1;
		return 0;
	}
	default:
		return 0;
	}
}

int vec_poly_rat_from_expr(vec_env *e, const vec_node *expr, const char *var_name,
			   vec_poly_rat *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	out->coeffs = NULL;
	out->len = 0;
	if (!e) {
		snprintf(err, errsz, "nil env");
		return -1;
	}
	int ok = 0;
	if (poly_from_node(e, expr, var_name, out, &ok, err, errsz) != 0) {
		vec_poly_rat_destroy(out);
		return -1;
	}
	if (!ok) {
		vec_poly_rat_destroy(out);
		snprintf(err, errsz, "not a rational polynomial");
		return -1;
	}
	poly_trim(out);
	return 0;
}

int vec_poly_rat_monic(vec_poly_rat *p, char *err, size_t errsz)
{
	if (!p) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	poly_trim(p);
	int d = vec_poly_rat_degree(p);
	if (d < 0)
		return 0;
	vec_rat lead = p->coeffs[(size_t)d];
	if (rat_is_zero(lead)) {
		snprintf(err, errsz, "leading coefficient is zero");
		return -1;
	}
	for (size_t i = 0; i < p->len; i++) {
		vec_rat v;
		if (rat_div(p->coeffs[i], lead, &v, err, errsz) != 0)
			return -1;
		p->coeffs[i] = v;
	}
	poly_trim(p);
	return 0;
}

int vec_poly_rat_divmod(const vec_poly_rat *a, const vec_poly_rat *b,
			vec_poly_rat *out_q, vec_poly_rat *out_r, char *err, size_t errsz)
{
	if (!out_q || !out_r) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	out_q->coeffs = NULL;
	out_q->len = 0;
	out_r->coeffs = NULL;
	out_r->len = 0;
	if (!a || !b) {
		snprintf(err, errsz, "bad args");
		return -1;
	}

	vec_poly_rat aa = {0};
	vec_poly_rat bb = {0};
	aa.len = a->len;
	bb.len = b->len;
	aa.coeffs = aa.len ? malloc(sizeof(aa.coeffs[0]) * aa.len) : NULL;
	bb.coeffs = bb.len ? malloc(sizeof(bb.coeffs[0]) * bb.len) : NULL;
	if ((aa.len && !aa.coeffs) || (bb.len && !bb.coeffs)) {
		vec_poly_rat_destroy(&aa);
		vec_poly_rat_destroy(&bb);
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	if (aa.len)
		memcpy(aa.coeffs, a->coeffs, sizeof(aa.coeffs[0]) * aa.len);
	if (bb.len)
		memcpy(bb.coeffs, b->coeffs, sizeof(bb.coeffs[0]) * bb.len);
	poly_trim(&aa);
	poly_trim(&bb);

	int da = vec_poly_rat_degree(&aa);
	int db = vec_poly_rat_degree(&bb);
	if (db < 0) {
		vec_poly_rat_destroy(&aa);
		vec_poly_rat_destroy(&bb);
		snprintf(err, errsz, "division by zero polynomial");
		return -1;
	}
	if (da < db) {
		*out_r = aa;
		vec_poly_rat_destroy(&bb);
		return 0;
	}

	vec_rat lead = bb.coeffs[(size_t)db];
	if (rat_is_zero(lead)) {
		vec_poly_rat_destroy(&aa);
		vec_poly_rat_destroy(&bb);
		snprintf(err, errsz, "zero leading coefficient");
		return -1;
	}

	vec_rat *rem = malloc(sizeof(rem[0]) * aa.len);
	size_t qlen = (size_t)(da - db + 1);
	vec_rat *quo = malloc(sizeof(quo[0]) * qlen);
	if (!rem || !quo) {
		vec_poly_rat_destroy(&aa);
		vec_poly_rat_destroy(&bb);
		free(rem);
		free(quo);
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	memcpy(rem, aa.coeffs, sizeof(rem[0]) * aa.len);
	for (size_t i = 0; i < qlen; i++)
		quo[i] = rat_zero();

	for (int k = da; k >= db; k--) {
		if (rat_is_zero(rem[(size_t)k]))
			continue;
		vec_rat t;
		if (rat_div(rem[(size_t)k], lead, &t, err, errsz) != 0) {
			free(rem);
			free(quo);
			vec_poly_rat_destroy(&aa);
			vec_poly_rat_destroy(&bb);
			return -1;
		}
		quo[(size_t)(k - db)] = t;
		for (int j = 0; j <= db; j++) {
			vec_rat p;
			if (rat_mul(t, bb.coeffs[(size_t)j], &p, err, errsz) != 0) {
				free(rem);
				free(quo);
				vec_poly_rat_destroy(&aa);
				vec_poly_rat_destroy(&bb);
				return -1;
			}
			vec_rat s;
			if (rat_sub(rem[(size_t)(k - db + j)], p, &s, err, errsz) != 0) {
				free(rem);
				free(quo);
				vec_poly_rat_destroy(&aa);
				vec_poly_rat_destroy(&bb);
				return -1;
			}
			rem[(size_t)(k - db + j)] = s;
		}
	}

	vec_poly_rat q = {.coeffs = quo, .len = qlen};
	vec_poly_rat r = {.coeffs = rem, .len = aa.len};
	poly_trim(&q);
	poly_trim(&r);
	*out_q = q;
	*out_r = r;

	vec_poly_rat_destroy(&aa);
	vec_poly_rat_destroy(&bb);
	return 0;
}

int vec_poly_rat_gcd(const vec_poly_rat *a, const vec_poly_rat *b, vec_poly_rat *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	out->coeffs = NULL;
	out->len = 0;
	if (!a || !b) {
		snprintf(err, errsz, "bad args");
		return -1;
	}

	vec_poly_rat aa = {0};
	vec_poly_rat bb = {0};
	aa.len = a->len;
	bb.len = b->len;
	aa.coeffs = aa.len ? malloc(sizeof(aa.coeffs[0]) * aa.len) : NULL;
	bb.coeffs = bb.len ? malloc(sizeof(bb.coeffs[0]) * bb.len) : NULL;
	if ((aa.len && !aa.coeffs) || (bb.len && !bb.coeffs)) {
		vec_poly_rat_destroy(&aa);
		vec_poly_rat_destroy(&bb);
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	if (aa.len)
		memcpy(aa.coeffs, a->coeffs, sizeof(aa.coeffs[0]) * aa.len);
	if (bb.len)
		memcpy(bb.coeffs, b->coeffs, sizeof(bb.coeffs[0]) * bb.len);
	poly_trim(&aa);
	poly_trim(&bb);

	if (vec_poly_rat_degree(&aa) < 0) {
		vec_poly_rat_destroy(&aa);
		if (vec_poly_rat_monic(&bb, err, errsz) != 0) {
			vec_poly_rat_destroy(&bb);
			return -1;
		}
		*out = bb;
		return 0;
	}
	if (vec_poly_rat_degree(&bb) < 0) {
		vec_poly_rat_destroy(&bb);
		if (vec_poly_rat_monic(&aa, err, errsz) != 0) {
			vec_poly_rat_destroy(&aa);
			return -1;
		}
		*out = aa;
		return 0;
	}

	while (vec_poly_rat_degree(&bb) >= 0) {
		vec_poly_rat q = {0};
		vec_poly_rat r = {0};
		if (vec_poly_rat_divmod(&aa, &bb, &q, &r, err, errsz) != 0) {
			vec_poly_rat_destroy(&aa);
			vec_poly_rat_destroy(&bb);
			return -1;
		}
		vec_poly_rat_destroy(&q);
		vec_poly_rat_destroy(&aa);
		aa = bb;
		bb = r;
	}
	vec_poly_rat_destroy(&bb);
	if (vec_poly_rat_monic(&aa, err, errsz) != 0) {
		vec_poly_rat_destroy(&aa);
		return -1;
	}
	*out = aa;
	return 0;
}

vec_node *vec_poly_rat_to_expr_horner(const vec_poly_rat *p, const char *var_name)
{
	if (!p || !var_name)
		return NULL;
	if (!p->coeffs || p->len == 0)
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));

	vec_node *x = vec_node_ident_new(var_name, strlen(var_name));
	if (!x)
		return NULL;
	vec_node *ex = vec_node_number_new(vec_rat_number(p->coeffs[p->len - 1]));
	if (!ex) {
		vec_node_destroy(x);
		return NULL;
	}
	for (size_t i = p->len - 1; i > 0; i--) {
		vec_node *coef = vec_node_number_new(vec_rat_number(p->coeffs[i - 1]));
		vec_node *mul = vec_node_binary_new('*', ex, vec_node_clone(x));
		vec_node *add = vec_node_binary_new('+', mul, coef);
		vec_node *simp = add ? vec_node_simplify_owned(add) : NULL;
		if (!simp) {
			vec_node_destroy(x);
			return NULL;
		}
		ex = simp;
	}
	vec_node_destroy(x);
	return ex;
}
