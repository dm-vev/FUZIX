#include "vec_poly_factor.h"

#include "vec_cas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t abs64(int64_t x) { return x < 0 ? -x : x; }

static int64_t gcd64(int64_t a, int64_t b)
{
	while (b) {
		int64_t t = a % b;
		a = b;
		b = t;
	}
	return a ? a : 1;
}

static int rat_is_zero(vec_rat a) { return a.num == 0; }
static vec_rat rat_zero(void) { return vec_rat_int(0); }
static vec_rat rat_neg(vec_rat a) { vec_rat r = a; r.num = -r.num; return r; }

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

static int poly_degree(const vec_poly_rat *p)
{
	if (!p || !p->coeffs || p->len == 0)
		return -1;
	for (size_t i = p->len; i > 0; i--) {
		if (!rat_is_zero(p->coeffs[i - 1]))
			return (int)(i - 1);
	}
	return -1;
}

static int det_rat_matrix(const vec_rat *a, int n, vec_rat *out_det, char *err, size_t errsz)
{
	if (!out_det) {
		snprintf(err, errsz, "bad dimensions");
		return -1;
	}
	*out_det = vec_rat_int(0);
	if (!a || n <= 0) {
		snprintf(err, errsz, "bad dimensions");
		return -1;
	}
	size_t nn = (size_t)n * (size_t)n;
	vec_rat *m = malloc(sizeof(m[0]) * nn);
	if (!m) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	memcpy(m, a, sizeof(m[0]) * nn);
	vec_rat sign = vec_rat_int(1);

	for (int k = 0; k < n; k++) {
		int piv = k;
		for (int i = k; i < n; i++) {
			if (!rat_is_zero(m[(size_t)i * (size_t)n + (size_t)k])) {
				piv = i;
				break;
			}
		}
		if (rat_is_zero(m[(size_t)piv * (size_t)n + (size_t)k])) {
			free(m);
			*out_det = vec_rat_int(0);
			return 0;
		}
		if (piv != k) {
			for (int j = 0; j < n; j++) {
				vec_rat tmp = m[(size_t)k * (size_t)n + (size_t)j];
				m[(size_t)k * (size_t)n + (size_t)j] = m[(size_t)piv * (size_t)n + (size_t)j];
				m[(size_t)piv * (size_t)n + (size_t)j] = tmp;
			}
			sign = rat_neg(sign);
		}
		vec_rat pivot = m[(size_t)k * (size_t)n + (size_t)k];
		for (int i = k + 1; i < n; i++) {
			if (rat_is_zero(m[(size_t)i * (size_t)n + (size_t)k]))
				continue;
			vec_rat f;
			if (rat_div(m[(size_t)i * (size_t)n + (size_t)k], pivot, &f, err, errsz) != 0) {
				free(m);
				return -1;
			}
			m[(size_t)i * (size_t)n + (size_t)k] = rat_zero();
			for (int j = k + 1; j < n; j++) {
				vec_rat p;
				if (rat_mul(f, m[(size_t)k * (size_t)n + (size_t)j], &p, err, errsz) != 0) {
					free(m);
					return -1;
				}
				vec_rat s;
				if (rat_sub(m[(size_t)i * (size_t)n + (size_t)j], p, &s, err, errsz) != 0) {
					free(m);
					return -1;
				}
				m[(size_t)i * (size_t)n + (size_t)j] = s;
			}
		}
	}

	vec_rat det = sign;
	for (int i = 0; i < n; i++) {
		vec_rat tmp;
		if (rat_mul(det, m[(size_t)i * (size_t)n + (size_t)i], &tmp, err, errsz) != 0) {
			free(m);
			return -1;
		}
		det = tmp;
	}

	free(m);
	*out_det = det;
	return 0;
}

int vec_poly_rat_resultant(const vec_poly_rat *a, const vec_poly_rat *b, vec_rat *out,
			   char *err, size_t errsz)
{
	if (!out) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	*out = vec_rat_int(0);
	if (!a || !b) {
		snprintf(err, errsz, "bad args");
		return -1;
	}

	vec_poly_rat pa = {0};
	vec_poly_rat pb = {0};
	pa.len = a->len;
	pb.len = b->len;
	pa.coeffs = pa.len ? malloc(sizeof(pa.coeffs[0]) * pa.len) : NULL;
	pb.coeffs = pb.len ? malloc(sizeof(pb.coeffs[0]) * pb.len) : NULL;
	if ((pa.len && !pa.coeffs) || (pb.len && !pb.coeffs)) {
		vec_poly_rat_destroy(&pa);
		vec_poly_rat_destroy(&pb);
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	if (pa.len)
		memcpy(pa.coeffs, a->coeffs, sizeof(pa.coeffs[0]) * pa.len);
	if (pb.len)
		memcpy(pb.coeffs, b->coeffs, sizeof(pb.coeffs[0]) * pb.len);
	poly_trim(&pa);
	poly_trim(&pb);
	int da = poly_degree(&pa);
	int db = poly_degree(&pb);
	if (da < 0 || db < 0) {
		vec_poly_rat_destroy(&pa);
		vec_poly_rat_destroy(&pb);
		*out = vec_rat_int(0);
		return 0;
	}
	if (da > 16 || db > 16) {
		vec_poly_rat_destroy(&pa);
		vec_poly_rat_destroy(&pb);
		snprintf(err, errsz, "degree too large");
		return -1;
	}

	int m = da;
	int n = db;
	int size = m + n;

	vec_rat *pd = malloc(sizeof(pd[0]) * (size_t)(m + 1));
	vec_rat *qd = malloc(sizeof(qd[0]) * (size_t)(n + 1));
	if (!pd || !qd) {
		vec_poly_rat_destroy(&pa);
		vec_poly_rat_destroy(&pb);
		free(pd);
		free(qd);
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	for (int i = 0; i <= m; i++)
		pd[(size_t)i] = pa.coeffs[(size_t)(m - i)];
	for (int i = 0; i <= n; i++)
		qd[(size_t)i] = pb.coeffs[(size_t)(n - i)];

	size_t nn = (size_t)size * (size_t)size;
	vec_rat *syl = malloc(sizeof(syl[0]) * nn);
	if (!syl) {
		vec_poly_rat_destroy(&pa);
		vec_poly_rat_destroy(&pb);
		free(pd);
		free(qd);
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	for (size_t i = 0; i < nn; i++)
		syl[i] = rat_zero();

	int row = 0;
	for (int i = 0; i < n; i++) {
		for (int j = 0; j <= m; j++)
			syl[(size_t)row * (size_t)size + (size_t)i + (size_t)j] = pd[(size_t)j];
		row++;
	}
	for (int i = 0; i < m; i++) {
		for (int j = 0; j <= n; j++)
			syl[(size_t)row * (size_t)size + (size_t)i + (size_t)j] = qd[(size_t)j];
		row++;
	}

	vec_rat det;
	int rc = det_rat_matrix(syl, size, &det, err, errsz);
	free(pd);
	free(qd);
	free(syl);
	vec_poly_rat_destroy(&pa);
	vec_poly_rat_destroy(&pb);
	if (rc != 0)
		return -1;
	*out = det;
	return 0;
}

static int64_t gcd_slice64(const int64_t *xs, size_t n)
{
	int64_t g = 0;
	for (size_t i = 0; i < n; i++) {
		int64_t x = xs[i];
		if (x < 0)
			x = -x;
		if (x == 0)
			continue;
		if (g == 0) {
			g = x;
			continue;
		}
		g = gcd64(g, x);
	}
	return g == 0 ? 1 : g;
}

static int eval_poly_rat_at(const vec_poly_rat *p, vec_rat x, int *out_is_zero, char *err, size_t errsz)
{
	if (!out_is_zero)
		return -1;
	*out_is_zero = 0;
	if (!p || !p->coeffs || p->len == 0)
		return 0;
	vec_rat v = p->coeffs[p->len - 1];
	for (size_t i = p->len - 1; i > 0; i--) {
		vec_rat tmp;
		if (rat_mul(v, x, &tmp, err, errsz) != 0)
			return -1;
		if (rat_add(tmp, p->coeffs[i - 1], &v, err, errsz) != 0)
			return -1;
	}
	*out_is_zero = (v.num == 0) ? 1 : 0;
	return 0;
}

static int divisors64(int64_t n, int64_t **out, size_t *out_len, char *err, size_t errsz)
{
	if (!out || !out_len) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	*out = NULL;
	*out_len = 0;
	if (n <= 0) {
		int64_t *xs = malloc(sizeof(xs[0]));
		if (!xs) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		xs[0] = 1;
		*out = xs;
		*out_len = 1;
		return 0;
	}

	size_t cap = 16;
	size_t len = 0;
	int64_t *xs = malloc(sizeof(xs[0]) * cap);
	if (!xs) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}

	int64_t limit = (int64_t)floor(sqrt((double)n));
	for (int64_t i = 1; i <= limit; i++) {
		if (n % i)
			continue;
		if (len == cap) {
			size_t ncap = cap * 2;
			int64_t *nx = realloc(xs, sizeof(nx[0]) * ncap);
			if (!nx) {
				free(xs);
				snprintf(err, errsz, "out of memory");
				return -1;
			}
			xs = nx;
			cap = ncap;
		}
		xs[len++] = i;
		int64_t j = n / i;
		if (i != j) {
			if (len == cap) {
				size_t ncap = cap * 2;
				int64_t *nx = realloc(xs, sizeof(nx[0]) * ncap);
				if (!nx) {
					free(xs);
					snprintf(err, errsz, "out of memory");
					return -1;
				}
				xs = nx;
				cap = ncap;
			}
			xs[len++] = j;
		}
	}

	int64_t *outx = realloc(xs, sizeof(outx[0]) * len);
	if (outx)
		xs = outx;
	*out = xs;
	*out_len = len;
	return 0;
}

static int find_rational_root(const vec_poly_rat *p, vec_rat *out_root, int *out_ok, char *err, size_t errsz)
{
	if (!out_root || !out_ok) {
		snprintf(err, errsz, "bad args");
		return -1;
	}
	*out_ok = 0;
	*out_root = vec_rat_int(0);
	if (!p)
		return 0;
	int d = poly_degree(p);
	if (d <= 0)
		return 0;
	vec_rat lead = p->coeffs[(size_t)d];
	vec_rat constant = p->coeffs[0];
	if (lead.den != 1 || constant.den != 1)
		return 0;
	int64_t a = lead.num;
	int64_t b = constant.num;
	if (a == 0)
		return 0;

	int64_t *div_p = NULL;
	int64_t *div_q = NULL;
	size_t ndiv_p = 0;
	size_t ndiv_q = 0;
	char ebuf[64];
	ebuf[0] = 0;
	if (divisors64(abs64(b), &div_p, &ndiv_p, ebuf, sizeof(ebuf)) != 0) {
		snprintf(err, errsz, "%s", ebuf[0] ? ebuf : "out of memory");
		return -1;
	}
	if (divisors64(abs64(a), &div_q, &ndiv_q, ebuf, sizeof(ebuf)) != 0) {
		free(div_p);
		snprintf(err, errsz, "%s", ebuf[0] ? ebuf : "out of memory");
		return -1;
	}

	for (size_t i = 0; i < ndiv_p; i++) {
		for (size_t j = 0; j < ndiv_q; j++) {
			for (int sgn = 0; sgn < 2; sgn++) {
				int64_t num = div_p[i];
				if (sgn)
					num = -num;
				int64_t den = div_q[j];
				vec_rat r;
				if (vec_rat_new(&r, num, den) != VEC_NUM_OK)
					continue;
				int is_zero = 0;
				char tmp[64];
				tmp[0] = 0;
				if (eval_poly_rat_at(p, r, &is_zero, tmp, sizeof(tmp)) != 0)
					continue;
				if (is_zero) {
					free(div_p);
					free(div_q);
					*out_root = r;
					*out_ok = 1;
					return 0;
				}
			}
		}
	}

	free(div_p);
	free(div_q);
	return 0;
}

vec_node *vec_poly_rat_factor_integer(const vec_poly_rat *p, const char *var_name, char *err, size_t errsz)
{
	if (!p || !var_name) {
		snprintf(err, errsz, "bad args");
		return NULL;
	}

	vec_poly_rat cur = {0};
	cur.len = p->len;
	cur.coeffs = cur.len ? malloc(sizeof(cur.coeffs[0]) * cur.len) : NULL;
	if (cur.len && !cur.coeffs) {
		snprintf(err, errsz, "out of memory");
		return NULL;
	}
	if (cur.len)
		memcpy(cur.coeffs, p->coeffs, sizeof(cur.coeffs[0]) * cur.len);
	poly_trim(&cur);

	int d = poly_degree(&cur);
	if (d < 0) {
		vec_poly_rat_destroy(&cur);
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	}
	if (d == 0) {
		vec_rat c0 = cur.coeffs[0];
		vec_poly_rat_destroy(&cur);
		return vec_node_number_new(vec_rat_number(c0));
	}
	if (d > 12) {
		vec_node *ex = vec_poly_rat_to_expr_horner(&cur, var_name);
		vec_poly_rat_destroy(&cur);
		return ex;
	}

	/* Require integer coefficients. */
	int64_t *intc = malloc(sizeof(intc[0]) * cur.len);
	if (!intc) {
		vec_poly_rat_destroy(&cur);
		snprintf(err, errsz, "out of memory");
		return NULL;
	}
	for (size_t i = 0; i < cur.len; i++) {
		if (cur.coeffs[i].den != 1) {
			free(intc);
			vec_node *ex = vec_poly_rat_to_expr_horner(&cur, var_name);
			vec_poly_rat_destroy(&cur);
			return ex;
		}
		intc[i] = cur.coeffs[i].num;
	}

	int64_t content = gcd_slice64(intc, cur.len);
	if (content < 0)
		content = -content;
	if (content == 0) {
		free(intc);
		vec_poly_rat_destroy(&cur);
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	}
	if (content != 1) {
		for (size_t i = 0; i < cur.len; i++)
			intc[i] /= content;
	}

	vec_poly_rat_destroy(&cur);
	cur.len = p->len;
	cur.coeffs = cur.len ? malloc(sizeof(cur.coeffs[0]) * cur.len) : NULL;
	if (cur.len && !cur.coeffs) {
		free(intc);
		snprintf(err, errsz, "out of memory");
		return NULL;
	}
	for (size_t i = 0; i < cur.len; i++)
		cur.coeffs[i] = vec_rat_int(intc[i]);
	free(intc);
	poly_trim(&cur);

	vec_node *out = NULL;
	if (content != 1) {
		out = vec_node_number_new(vec_rat_number(vec_rat_int(content)));
		if (!out) {
			vec_poly_rat_destroy(&cur);
			snprintf(err, errsz, "out of memory");
			return NULL;
		}
	}

	while (poly_degree(&cur) > 0) {
		vec_rat root;
		int ok = 0;
		char ebuf[96];
		ebuf[0] = 0;
		if (find_rational_root(&cur, &root, &ok, ebuf, sizeof(ebuf)) != 0) {
			vec_node_destroy(out);
			vec_poly_rat_destroy(&cur);
			snprintf(err, errsz, "%s", ebuf[0] ? ebuf : "error");
			return NULL;
		}
		if (!ok)
			break;

		int64_t num = root.num;
		int64_t den = root.den;
		vec_node *factor = NULL;
		{
			vec_node *lhs = vec_node_binary_new('*',
							    vec_node_number_new(vec_rat_number(vec_rat_int(den))),
							    vec_node_ident_new(var_name, strlen(var_name)));
			vec_node *rhs = vec_node_number_new(vec_rat_number(vec_rat_int(num)));
			vec_node *sub = vec_node_binary_new('-', lhs, rhs);
			factor = sub ? vec_node_simplify_owned(sub) : NULL;
		}
		if (!factor) {
			vec_node_destroy(out);
			vec_poly_rat_destroy(&cur);
			snprintf(err, errsz, "out of memory");
			return NULL;
		}

		if (out) {
			vec_node *mul = vec_node_binary_new('*', out, factor);
			vec_node *simp = mul ? vec_node_simplify_owned(mul) : NULL;
			if (!simp) {
				vec_node_destroy(out);
				vec_node_destroy(factor);
				vec_poly_rat_destroy(&cur);
				snprintf(err, errsz, "out of memory");
				return NULL;
			}
			out = simp;
		} else {
			out = factor;
		}

		vec_poly_rat divisor = {.coeffs = NULL, .len = 2};
		divisor.coeffs = malloc(sizeof(divisor.coeffs[0]) * 2);
		if (!divisor.coeffs) {
			vec_node_destroy(out);
			vec_poly_rat_destroy(&cur);
			snprintf(err, errsz, "out of memory");
			return NULL;
		}
		divisor.coeffs[0] = vec_rat_int(-num);
		divisor.coeffs[1] = vec_rat_int(den);

		vec_poly_rat q = {0};
		vec_poly_rat r = {0};
		char dbuf[96];
		dbuf[0] = 0;
		int divrc = vec_poly_rat_divmod(&cur, &divisor, &q, &r, dbuf, sizeof(dbuf));
		vec_poly_rat_destroy(&divisor);
		if (divrc != 0) {
			vec_node_destroy(out);
			vec_poly_rat_destroy(&cur);
			snprintf(err, errsz, "%s", dbuf[0] ? dbuf : "error");
			return NULL;
		}
		if (poly_degree(&r) >= 0) {
			vec_poly_rat_destroy(&q);
			vec_poly_rat_destroy(&r);
			break;
		}
		vec_poly_rat_destroy(&r);
		vec_poly_rat_destroy(&cur);
		cur = q;
		poly_trim(&cur);
	}

	if (poly_degree(&cur) > 0) {
		vec_node *rem = vec_poly_rat_to_expr_horner(&cur, var_name);
		if (!rem) {
			vec_node_destroy(out);
			vec_poly_rat_destroy(&cur);
			snprintf(err, errsz, "out of memory");
			return NULL;
		}
		if (out) {
			vec_node *mul = vec_node_binary_new('*', out, rem);
			vec_node *simp = mul ? vec_node_simplify_owned(mul) : NULL;
			if (!simp) {
				vec_node_destroy(out);
				vec_node_destroy(rem);
				vec_poly_rat_destroy(&cur);
				snprintf(err, errsz, "out of memory");
				return NULL;
			}
			out = simp;
		} else {
			out = rem;
		}
	}

	vec_poly_rat_destroy(&cur);
	return out ? out : vec_node_number_new(vec_rat_number(vec_rat_int(1)));
}

