#include "vec_cas_ext.h"

#include "vec_cas.h"
#include "vec_eval.h"
#include "vec_number.h"
#include "vec_value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vec_node *expand_owned(vec_node *n);

static vec_node *expand_owned(vec_node *n)
{
	if (!n)
		return NULL;
	switch (n->kind) {
	case VEC_NODE_NUMBER:
	case VEC_NODE_IDENT:
		return n;
	case VEC_NODE_UNARY: {
		vec_node *x = expand_owned(n->u.unary.x);
		if (!x) {
			vec_node_destroy(n);
			return NULL;
		}
		n->u.unary.x = x;
		return vec_node_simplify_owned(n);
	}
	case VEC_NODE_COMPARE: {
		vec_node *l = expand_owned(n->u.compare.left);
		vec_node *r = expand_owned(n->u.compare.right);
		if (!l || !r) {
			vec_node_destroy(l);
			vec_node_destroy(r);
			vec_node_destroy(n);
			return NULL;
		}
		n->u.compare.left = l;
		n->u.compare.right = r;
		return vec_node_simplify_owned(n);
	}
	case VEC_NODE_CALL: {
		for (size_t i = 0; i < n->u.call.argc; i++) {
			n->u.call.args[i] = expand_owned(n->u.call.args[i]);
			if (!n->u.call.args[i]) {
				vec_node_destroy(n);
				return NULL;
			}
		}
		return n;
	}
	case VEC_NODE_BINARY: {
		vec_node *left = expand_owned(n->u.binary.left);
		vec_node *right = expand_owned(n->u.binary.right);
		if (!left || !right) {
			vec_node_destroy(left);
			vec_node_destroy(right);
			vec_node_destroy(n);
			return NULL;
		}
		char op = n->u.binary.op;
		n->u.binary.left = left;
		n->u.binary.right = right;

		if (op == '*') {
			if (left->kind == VEC_NODE_BINARY && (left->u.binary.op == '+' || left->u.binary.op == '-')) {
				char aop = left->u.binary.op;
				vec_node *a = left->u.binary.left;
				vec_node *b = left->u.binary.right;
				left->u.binary.left = NULL;
				left->u.binary.right = NULL;
				vec_node_destroy(left);

				vec_node *rclone = vec_node_clone(right);
				if (!rclone) {
					vec_node_destroy(a);
					vec_node_destroy(b);
					vec_node_destroy(right);
					vec_node_destroy(n);
					return NULL;
				}
				vec_node *t1 = vec_node_binary_new('*', a, rclone);
				vec_node *t2 = vec_node_binary_new('*', b, right);
				n->u.binary.left = NULL;
				n->u.binary.right = NULL;
				vec_node_destroy(n);
				return expand_owned(vec_node_binary_new(aop, t1, t2));
			}
			if (right->kind == VEC_NODE_BINARY && (right->u.binary.op == '+' || right->u.binary.op == '-')) {
				char aop = right->u.binary.op;
				vec_node *a = right->u.binary.left;
				vec_node *b = right->u.binary.right;
				right->u.binary.left = NULL;
				right->u.binary.right = NULL;
				vec_node_destroy(right);

				vec_node *lclone = vec_node_clone(left);
				if (!lclone) {
					vec_node_destroy(a);
					vec_node_destroy(b);
					vec_node_destroy(left);
					vec_node_destroy(n);
					return NULL;
				}
				vec_node *t1 = vec_node_binary_new('*', left, a);
				vec_node *t2 = vec_node_binary_new('*', lclone, b);
				n->u.binary.left = NULL;
				n->u.binary.right = NULL;
				vec_node_destroy(n);
				return expand_owned(vec_node_binary_new(aop, t1, t2));
			}
			return vec_node_simplify_owned(n);
		}

		if (op == '^') {
			if (right->kind == VEC_NODE_NUMBER) {
				double expf = vec_number_float64(right->u.number);
				if (expf == trunc(expf)) {
					int pow = (int)expf;
					if (pow >= 0 && pow <= 12) {
						if (left->kind == VEC_NODE_BINARY && (left->u.binary.op == '+' || left->u.binary.op == '-')) {
							vec_node_destroy(right);
							vec_node *out = vec_node_number_new(vec_rat_number(vec_rat_int(1)));
							if (!out) {
								vec_node_destroy(left);
								vec_node_destroy(n);
								return NULL;
							}
							for (int i = 0; i < pow; i++) {
								vec_node *mul = vec_node_binary_new('*', out, vec_node_clone(left));
								vec_node *simp = mul ? vec_node_simplify_owned(mul) : NULL;
								if (!simp) {
									vec_node_destroy(out);
									vec_node_destroy(left);
									vec_node_destroy(n);
									return NULL;
								}
								out = expand_owned(simp);
								if (!out) {
									vec_node_destroy(left);
									vec_node_destroy(n);
									return NULL;
								}
							}
							vec_node_destroy(left);
							vec_node_destroy(n);
							return out;
						}
					}
				}
			}
			return vec_node_simplify_owned(n);
		}

		return vec_node_simplify_owned(n);
	}
	default:
		vec_node_destroy(n);
		return NULL;
	}
}

vec_node *vec_node_expand(const vec_node *n)
{
	if (!n)
		return NULL;
	vec_node *c = vec_node_clone(n);
	if (!c)
		return NULL;
	return expand_owned(c);
}

vec_node *vec_node_taylor_series(vec_env *e, const vec_node *expr, const char *var_name,
				 double a, int n, char *err, size_t errsz)
{
	if (!e || !expr || !var_name) {
		snprintf(err, errsz, "eval: bad args");
		return NULL;
	}

	vec_node *x = vec_node_ident_new(var_name, strlen(var_name));
	vec_node *an = vec_node_number_new(vec_float(a));
	vec_node *dx = (x && an) ? vec_node_simplify_owned(vec_node_binary_new('-', x, an)) : NULL;
	if (!dx) {
		snprintf(err, errsz, "eval: out of memory");
		return NULL;
	}

	vec_node *fk = vec_node_clone(expr);
	if (!fk) {
		vec_node_destroy(dx);
		snprintf(err, errsz, "eval: out of memory");
		return NULL;
	}

	vec_node *out = vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	if (!out) {
		vec_node_destroy(dx);
		vec_node_destroy(fk);
		snprintf(err, errsz, "eval: out of memory");
		return NULL;
	}

	double fact = 1.0;
	for (int k = 0; k <= n; k++) {
		vec_value v;
		memset(&v, 0, sizeof(v));
		if (vec_eval_node_override(e, fk, var_name, vec_value_number(vec_float(a)), &v, err, errsz) != 0) {
			vec_node_destroy(dx);
			vec_node_destroy(fk);
			vec_node_destroy(out);
			return NULL;
		}
		if (v.kind != VEC_VALUE_NUMBER) {
			vec_value_destroy(&v);
			vec_node_destroy(dx);
			vec_node_destroy(fk);
			vec_node_destroy(out);
			snprintf(err, errsz, "eval: series expects numeric expression");
			return NULL;
		}
		double ck = vec_number_float64(v.num) / fact;
		vec_value_destroy(&v);

		vec_node *term = vec_node_number_new(vec_float(ck));
		if (!term) {
			vec_node_destroy(dx);
			vec_node_destroy(fk);
			vec_node_destroy(out);
			snprintf(err, errsz, "eval: out of memory");
			return NULL;
		}
		if (k > 0) {
			vec_node *pow = vec_node_clone(dx);
			if (!pow) {
				vec_node_destroy(dx);
				vec_node_destroy(fk);
				vec_node_destroy(out);
				vec_node_destroy(term);
				snprintf(err, errsz, "eval: out of memory");
				return NULL;
			}
			for (int i = 1; i < k; i++) {
				vec_node *mul = vec_node_binary_new('*', pow, vec_node_clone(dx));
				vec_node *simp = mul ? vec_node_simplify_owned(mul) : NULL;
				if (!simp) {
					vec_node_destroy(dx);
					vec_node_destroy(fk);
					vec_node_destroy(out);
					vec_node_destroy(term);
					vec_node_destroy(pow);
					snprintf(err, errsz, "eval: out of memory");
					return NULL;
				}
				pow = simp;
			}
			vec_node *mul = vec_node_binary_new('*', term, pow);
			vec_node *simp = mul ? vec_node_simplify_owned(mul) : NULL;
			if (!simp) {
				vec_node_destroy(dx);
				vec_node_destroy(fk);
				vec_node_destroy(out);
				vec_node_destroy(term);
				snprintf(err, errsz, "eval: out of memory");
				return NULL;
			}
			term = simp;
		}

		vec_node *add = vec_node_binary_new('+', out, term);
		vec_node *simp = add ? vec_node_simplify_owned(add) : NULL;
		if (!simp) {
			vec_node_destroy(dx);
			vec_node_destroy(fk);
			vec_node_destroy(out);
			snprintf(err, errsz, "eval: out of memory");
			return NULL;
		}
		out = simp;

		vec_node *dfk = vec_node_deriv(fk, var_name);
		vec_node_destroy(fk);
		fk = dfk ? vec_node_simplify_owned(dfk) : NULL;
		if (!fk) {
			vec_node_destroy(dx);
			vec_node_destroy(out);
			snprintf(err, errsz, "eval: out of memory");
			return NULL;
		}

		fact *= (double)(k + 1);
		if (fact == 0 || isnan(fact) || isinf(fact))
			break;
	}

	vec_node_destroy(dx);
	vec_node_destroy(fk);
	return vec_node_simplify_owned(out);
}

