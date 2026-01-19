#include "vec_poly.h"

#include "vec_cas.h"
#include "vec_number.h"
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

static int mul_size(size_t a, size_t b, size_t *out)
{
	if (!out)
		return -1;
	if (a && b > SIZE_MAX / a)
		return -1;
	*out = a * b;
	return 0;
}

static void poly_trim(vec_poly *p)
{
	if (!p || !p->coeffs)
		return;
	size_t n = p->len;
	while (n > 0 && p->coeffs[n - 1] == 0)
		n--;
	if (n == p->len)
		return;
	if (n == 0) {
		free(p->coeffs);
		p->coeffs = NULL;
		p->len = 0;
		return;
	}
	double *nc = realloc(p->coeffs, sizeof(nc[0]) * n);
	if (nc) {
		p->coeffs = nc;
		p->len = n;
	} else {
		p->len = n;
	}
}

void vec_poly_destroy(vec_poly *p)
{
	if (!p)
		return;
	free(p->coeffs);
	p->coeffs = NULL;
	p->len = 0;
}

int vec_poly_degree(const vec_poly *p)
{
	if (!p || !p->coeffs || p->len == 0)
		return -1;
	for (size_t i = p->len; i > 0; i--) {
		if (p->coeffs[i - 1] != 0)
			return (int)(i - 1);
	}
	return -1;
}

double vec_poly_eval(const vec_poly *p, double x)
{
	if (!p || !p->coeffs || p->len == 0)
		return 0;
	double v = p->coeffs[p->len - 1];
	for (size_t i = p->len - 1; i > 0; i--)
		v = v * x + p->coeffs[i - 1];
	return v;
}

static int poly_add(const vec_poly *a, const vec_poly *b, vec_poly *out)
{
	size_t n = a->len;
	if (b->len > n)
		n = b->len;
	double *c = n ? malloc(sizeof(c[0]) * n) : NULL;
	if (n && !c)
		return -1;
	for (size_t i = 0; i < n; i++) {
		double v = 0;
		if (i < a->len)
			v += a->coeffs[i];
		if (i < b->len)
			v += b->coeffs[i];
		c[i] = v;
	}
	out->coeffs = c;
	out->len = n;
	poly_trim(out);
	return 0;
}

static int poly_sub(const vec_poly *a, const vec_poly *b, vec_poly *out)
{
	size_t n = a->len;
	if (b->len > n)
		n = b->len;
	double *c = n ? malloc(sizeof(c[0]) * n) : NULL;
	if (n && !c)
		return -1;
	for (size_t i = 0; i < n; i++) {
		double v = 0;
		if (i < a->len)
			v += a->coeffs[i];
		if (i < b->len)
			v -= b->coeffs[i];
		c[i] = v;
	}
	out->coeffs = c;
	out->len = n;
	poly_trim(out);
	return 0;
}

static int poly_mul(const vec_poly *a, const vec_poly *b, vec_poly *out)
{
	if (a->len == 0 || b->len == 0) {
		out->coeffs = NULL;
		out->len = 0;
		return 0;
	}
	size_t n;
	if (mul_size(a->len + b->len, 1, &n) != 0)
		return -1;
	if (n == 0)
		return -1;
	n = a->len + b->len - 1;
	double *c = malloc(sizeof(c[0]) * n);
	if (!c)
		return -1;
	for (size_t i = 0; i < n; i++)
		c[i] = 0;
	for (size_t i = 0; i < a->len; i++) {
		double ca = a->coeffs[i];
		if (ca == 0)
			continue;
		for (size_t j = 0; j < b->len; j++) {
			double cb = b->coeffs[j];
			c[i + j] += ca * cb;
		}
	}
	out->coeffs = c;
	out->len = n;
	poly_trim(out);
	return 0;
}

static int poly_pow(const vec_poly *p, int exp, vec_poly *out)
{
	if (exp == 0) {
		double *c = malloc(sizeof(c[0]));
		if (!c)
			return -1;
		c[0] = 1;
		out->coeffs = c;
		out->len = 1;
		return 0;
	}
	if (exp == 1) {
		double *c = p->len ? malloc(sizeof(c[0]) * p->len) : NULL;
		if (p->len && !c)
			return -1;
		if (p->len)
			memcpy(c, p->coeffs, sizeof(c[0]) * p->len);
		out->coeffs = c;
		out->len = p->len;
		poly_trim(out);
		return 0;
	}

	vec_poly base = {0};
	if (poly_pow(p, 1, &base) != 0)
		return -1;

	vec_poly acc = {0};
	if (poly_pow(p, 0, &acc) != 0) {
		vec_poly_destroy(&base);
		return -1;
	}

	int e = exp;
	while (e > 0) {
		if (e & 1) {
			vec_poly tmp = {0};
			if (poly_mul(&acc, &base, &tmp) != 0) {
				vec_poly_destroy(&acc);
				vec_poly_destroy(&base);
				return -1;
			}
			vec_poly_destroy(&acc);
			acc = tmp;
		}
		e >>= 1;
		if (!e)
			break;
		{
			vec_poly tmp = {0};
			if (poly_mul(&base, &base, &tmp) != 0) {
				vec_poly_destroy(&acc);
				vec_poly_destroy(&base);
				return -1;
			}
			vec_poly_destroy(&base);
			base = tmp;
		}
	}

	vec_poly_destroy(&base);
	*out = acc;
	poly_trim(out);
	return 0;
}

static int poly_from_node(vec_env *e, const vec_node *ex, const char *var_name,
			  vec_poly *out, int *ok, char *err, size_t errsz)
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
		double *c = malloc(sizeof(c[0]));
		if (!c) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		c[0] = vec_number_float64(ex->u.number);
		out->coeffs = c;
		out->len = 1;
		*ok = 1;
		return 0;
	}
	case VEC_NODE_IDENT: {
		if (ex->u.ident.name && !strcmp(ex->u.ident.name, var_name)) {
			double *c = malloc(sizeof(c[0]) * 2);
			if (!c) {
				snprintf(err, errsz, "out of memory");
				return -1;
			}
			c[0] = 0;
			c[1] = 1;
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
		double *c = malloc(sizeof(c[0]));
		if (!c) {
			vec_value_destroy(&v);
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		c[0] = vec_number_float64(v.num);
		vec_value_destroy(&v);
		out->coeffs = c;
		out->len = 1;
		*ok = 1;
		return 0;
	}
	case VEC_NODE_UNARY: {
		vec_poly p = {0};
		int ok_inner = 0;
		if (poly_from_node(e, ex->u.unary.x, var_name, &p, &ok_inner, err, errsz) != 0)
			return -1;
		if (!ok_inner) {
			vec_poly_destroy(&p);
			return 0;
		}
		switch (ex->u.unary.op) {
		case '+':
			*out = p;
			*ok = 1;
			return 0;
		case '-':
			for (size_t i = 0; i < p.len; i++)
				p.coeffs[i] = -p.coeffs[i];
			*out = p;
			*ok = 1;
			return 0;
		default:
			vec_poly_destroy(&p);
			return 0;
		}
	}
	case VEC_NODE_BINARY: {
		vec_poly a = {0};
		vec_poly b = {0};
		int ok_a = 0;
		int ok_b = 0;
		if (poly_from_node(e, ex->u.binary.left, var_name, &a, &ok_a, err, errsz) != 0)
			return -1;
		if (poly_from_node(e, ex->u.binary.right, var_name, &b, &ok_b, err, errsz) != 0) {
			vec_poly_destroy(&a);
			return -1;
		}
		if (!ok_a || !ok_b) {
			vec_poly_destroy(&a);
			vec_poly_destroy(&b);
			return 0;
		}
		char op = ex->u.binary.op;
		vec_poly res = {0};
		int rc = 0;
		switch (op) {
		case '+':
			rc = poly_add(&a, &b, &res);
			break;
		case '-':
			rc = poly_sub(&a, &b, &res);
			break;
		case '*':
			rc = poly_mul(&a, &b, &res);
			break;
		case '^': {
			if (!ex->u.binary.right || ex->u.binary.right->kind != VEC_NODE_NUMBER) {
				vec_poly_destroy(&a);
				vec_poly_destroy(&b);
				return 0;
				}
				double expf = vec_number_float64(ex->u.binary.right->u.number);
				if (isnan(expf) || isinf(expf) || expf != trunc_d(expf)) {
					vec_poly_destroy(&a);
					vec_poly_destroy(&b);
					return 0;
				}
			int exp = (int)expf;
			if (exp < 0 || exp > 64) {
				vec_poly_destroy(&a);
				vec_poly_destroy(&b);
				return 0;
			}
			rc = poly_pow(&a, exp, &res);
			break;
		}
		default:
			vec_poly_destroy(&a);
			vec_poly_destroy(&b);
			return 0;
		}
		vec_poly_destroy(&a);
		vec_poly_destroy(&b);
		if (rc != 0) {
			snprintf(err, errsz, "out of memory");
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

int vec_poly_from_expr(vec_env *e, const vec_node *expr, const char *var_name,
		       vec_poly *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	out->coeffs = NULL;
	out->len = 0;
	int ok = 0;
	if (!e) {
		snprintf(err, errsz, "nil env");
		return -1;
	}
	if (poly_from_node(e, expr, var_name, out, &ok, err, errsz) != 0) {
		vec_poly_destroy(out);
		return -1;
	}
	if (!ok) {
		vec_poly_destroy(out);
		snprintf(err, errsz, "not a polynomial");
		return -1;
	}
	poly_trim(out);
	return 0;
}

int vec_poly_from_coeffs(const double *coeffs, size_t len, vec_poly *out, char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	out->coeffs = NULL;
	out->len = 0;
	if (!coeffs && len) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	double *c = len ? malloc(sizeof(c[0]) * len) : NULL;
	if (len && !c) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	if (len)
		memcpy(c, coeffs, sizeof(c[0]) * len);
	out->coeffs = c;
	out->len = len;
	poly_trim(out);
	return 0;
}

vec_node *vec_poly_to_expr_horner(const vec_poly *p, const char *var_name)
{
	if (!p || !var_name)
		return NULL;
	if (!p->coeffs || p->len == 0)
		return vec_node_number_new(vec_float(0));

	vec_node *x = vec_node_ident_new(var_name, strlen(var_name));
	if (!x)
		return NULL;

	vec_node *ex = vec_node_number_new(vec_float(p->coeffs[p->len - 1]));
	if (!ex) {
		vec_node_destroy(x);
		return NULL;
	}
	for (size_t i = p->len - 1; i > 0; i--) {
		vec_node *coef = vec_node_number_new(vec_float(p->coeffs[i - 1]));
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
