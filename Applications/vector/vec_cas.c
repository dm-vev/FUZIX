#include "vec_cas.h"

#include "vec_number.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bin_prec(char op)
{
	switch (op) {
	case '+':
	case '-':
		return 1;
	case '*':
	case '/':
		return 2;
	case '^':
		return 3;
	default:
		return 0;
	}
}

static const char *cmp_text(vec_cmp_op op)
{
	switch (op) {
	case VEC_CMP_EQ:
		return "==";
	case VEC_CMP_NE:
		return "!=";
	case VEC_CMP_LT:
		return "<";
	case VEC_CMP_LE:
		return "<=";
	case VEC_CMP_GT:
		return ">";
	case VEC_CMP_GE:
		return ">=";
	default:
		return "?";
	}
}

static int is_zero_node(const vec_node *n)
{
	if (!n || n->kind != VEC_NODE_NUMBER)
		return 0;
	if (n->u.number.kind == VEC_NUM_RAT)
		return n->u.number.r.num == 0;
	return n->u.number.f == 0;
}

static int is_one_node(const vec_node *n)
{
	if (!n || n->kind != VEC_NODE_NUMBER)
		return 0;
	if (n->u.number.kind == VEC_NUM_RAT)
		return n->u.number.r.num == 1 && n->u.number.r.den == 1;
	return n->u.number.f == 1;
}

vec_node *vec_node_simplify(const vec_node *n)
{
	if (!n)
		return NULL;

	switch (n->kind) {
	case VEC_NODE_NUMBER:
	case VEC_NODE_IDENT:
		return vec_node_clone(n);

	case VEC_NODE_UNARY: {
		vec_node *x = vec_node_simplify(n->u.unary.x);
		if (!x)
			return NULL;
		if (x->kind == VEC_NODE_NUMBER) {
			switch (n->u.unary.op) {
			case '+':
				return x;
			case '-': {
				vec_number v = x->u.number;
				if (v.kind == VEC_NUM_RAT) {
					v.r.num = -v.r.num;
					x->u.number = v;
					return x;
				}
				x->u.number = vec_float(-v.f);
				return x;
			}
			default:
				break;
			}
		}
		if (x->kind == VEC_NODE_UNARY && n->u.unary.op == '+')
			return x;
		return vec_node_unary_new(n->u.unary.op, x);
	}

	case VEC_NODE_BINARY: {
		vec_node *left = vec_node_simplify(n->u.binary.left);
		vec_node *right = vec_node_simplify(n->u.binary.right);
		if (!left || !right) {
			vec_node_destroy(left);
			vec_node_destroy(right);
			return NULL;
		}

		if (left->kind == VEC_NODE_NUMBER && right->kind == VEC_NODE_NUMBER) {
			double a = vec_number_float64(left->u.number);
			double b = vec_number_float64(right->u.number);
			int ok = 1;
			double out = 0;
			switch (n->u.binary.op) {
			case '+':
				out = a + b;
				break;
			case '-':
				out = a - b;
				break;
			case '*':
				out = a * b;
				break;
			case '/':
				if (b == 0)
					ok = 0;
				else
					out = a / b;
				break;
			case '^':
				out = pow(a, b);
				break;
			default:
				ok = 0;
				break;
			}
			if (ok) {
				vec_node_destroy(left);
				vec_node_destroy(right);
				return vec_node_number_new(vec_float(out));
			}
		}

		switch (n->u.binary.op) {
		case '+':
			if (is_zero_node(left)) {
				vec_node_destroy(left);
				return right;
			}
			if (is_zero_node(right)) {
				vec_node_destroy(right);
				return left;
			}
			break;
		case '-':
			if (is_zero_node(right)) {
				vec_node_destroy(right);
				return left;
			}
			break;
		case '*':
			if (is_zero_node(left) || is_zero_node(right)) {
				vec_node_destroy(left);
				vec_node_destroy(right);
				return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
			}
			if (is_one_node(left)) {
				vec_node_destroy(left);
				return right;
			}
			if (is_one_node(right)) {
				vec_node_destroy(right);
				return left;
			}
			break;
		case '/':
			if (is_zero_node(left)) {
				vec_node_destroy(left);
				vec_node_destroy(right);
				return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
			}
			if (is_one_node(right)) {
				vec_node_destroy(right);
				return left;
			}
			break;
		case '^':
			if (is_one_node(right)) {
				vec_node_destroy(right);
				return left;
			}
			if (is_zero_node(right)) {
				vec_node_destroy(left);
				vec_node_destroy(right);
				return vec_node_number_new(vec_rat_number(vec_rat_int(1)));
			}
			break;
		default:
			break;
		}

		return vec_node_binary_new(n->u.binary.op, left, right);
	}

	case VEC_NODE_COMPARE: {
		vec_node *left = vec_node_simplify(n->u.compare.left);
		vec_node *right = vec_node_simplify(n->u.compare.right);
		if (!left || !right) {
			vec_node_destroy(left);
			vec_node_destroy(right);
			return NULL;
		}
		if (left->kind == VEC_NODE_NUMBER && right->kind == VEC_NODE_NUMBER) {
			double a = vec_number_float64(left->u.number);
			double b = vec_number_float64(right->u.number);
			int ok = 0;
			switch (n->u.compare.op) {
			case VEC_CMP_EQ: ok = (a == b); break;
			case VEC_CMP_NE: ok = (a != b); break;
			case VEC_CMP_LT: ok = (a < b); break;
			case VEC_CMP_LE: ok = (a <= b); break;
			case VEC_CMP_GT: ok = (a > b); break;
			case VEC_CMP_GE: ok = (a >= b); break;
			default:
				break;
			}
			vec_node_destroy(left);
			vec_node_destroy(right);
			return vec_node_number_new(vec_rat_number(vec_rat_int(ok ? 1 : 0)));
		}
		return vec_node_compare_new(n->u.compare.op, left, right);
	}

	case VEC_NODE_CALL: {
		if (!n->u.call.name)
			return NULL;
		size_t argc = n->u.call.argc;
		vec_node **args = NULL;
		if (argc) {
			args = calloc(argc, sizeof(args[0]));
			if (!args)
				return NULL;
			for (size_t i = 0; i < argc; i++) {
				args[i] = vec_node_simplify(n->u.call.args[i]);
				if (!args[i]) {
					for (size_t j = 0; j < argc; j++)
						vec_node_destroy(args[j]);
					free(args);
					return NULL;
				}
			}
		}
		return vec_node_call_new(n->u.call.name, strlen(n->u.call.name), argc, args);
	}

	default:
		return NULL;
	}
}

vec_node *vec_node_simplify_owned(vec_node *n)
{
	if (!n)
		return NULL;
	vec_node *out = vec_node_simplify(n);
	vec_node_destroy(n);
	return out;
}

vec_node *vec_node_deriv(const vec_node *n, const char *var_name)
{
	if (!n || !var_name)
		return NULL;
	switch (n->kind) {
	case VEC_NODE_NUMBER:
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	case VEC_NODE_IDENT:
		if (n->u.ident.name && !strcmp(n->u.ident.name, var_name))
			return vec_node_number_new(vec_rat_number(vec_rat_int(1)));
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	case VEC_NODE_UNARY:
		if (n->u.unary.op == '+')
			return vec_node_deriv(n->u.unary.x, var_name);
		if (n->u.unary.op == '-') {
			vec_node *dx = vec_node_deriv(n->u.unary.x, var_name);
			if (!dx)
				return NULL;
			return vec_node_simplify_owned(vec_node_unary_new('-', dx));
		}
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	case VEC_NODE_BINARY: {
		char op = n->u.binary.op;
		switch (op) {
		case '+': {
			vec_node *l = vec_node_deriv(n->u.binary.left, var_name);
			vec_node *r = vec_node_deriv(n->u.binary.right, var_name);
			if (!l || !r) {
				vec_node_destroy(l);
				vec_node_destroy(r);
				return NULL;
			}
			return vec_node_simplify_owned(vec_node_binary_new('+', l, r));
		}
		case '-': {
			vec_node *l = vec_node_deriv(n->u.binary.left, var_name);
			vec_node *r = vec_node_deriv(n->u.binary.right, var_name);
			if (!l || !r) {
				vec_node_destroy(l);
				vec_node_destroy(r);
				return NULL;
			}
			return vec_node_simplify_owned(vec_node_binary_new('-', l, r));
		}
		case '*': {
			/* (uv)' = u'v + uv' */
			vec_node *du = vec_node_deriv(n->u.binary.left, var_name);
			vec_node *dv = vec_node_deriv(n->u.binary.right, var_name);
			vec_node *u = vec_node_clone(n->u.binary.left);
			vec_node *v = vec_node_clone(n->u.binary.right);
			if (!du || !dv || !u || !v) {
				vec_node_destroy(du);
				vec_node_destroy(dv);
				vec_node_destroy(u);
				vec_node_destroy(v);
				return NULL;
			}
			vec_node *t1 = vec_node_binary_new('*', du, v);
			vec_node *t2 = vec_node_binary_new('*', u, dv);
			return vec_node_simplify_owned(vec_node_binary_new('+', t1, t2));
		}
		case '/': {
			/* (u/v)' = (u'v - uv') / v^2 */
			vec_node *du = vec_node_deriv(n->u.binary.left, var_name);
			vec_node *dv = vec_node_deriv(n->u.binary.right, var_name);
			vec_node *u = vec_node_clone(n->u.binary.left);
			vec_node *v = vec_node_clone(n->u.binary.right);
			if (!du || !dv || !u || !v) {
				vec_node_destroy(du);
				vec_node_destroy(dv);
				vec_node_destroy(u);
				vec_node_destroy(v);
				return NULL;
			}
			vec_node *num = vec_node_binary_new('-', vec_node_binary_new('*', du, vec_node_clone(v)),
							    vec_node_binary_new('*', u, dv));
			vec_node *den = vec_node_binary_new('^', v,
							    vec_node_number_new(vec_rat_number(vec_rat_int(2))));
			return vec_node_simplify_owned(vec_node_binary_new('/', num, den));
		}
		case '^': {
			/* Handle f(x)^c where c is integer constant. */
			const vec_node *cn = n->u.binary.right;
			if (cn && cn->kind == VEC_NODE_NUMBER && cn->u.number.kind == VEC_NUM_RAT &&
			    cn->u.number.r.den == 1) {
				int64_t c = cn->u.number.r.num;
				if (c == 0)
					return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
				vec_node *u = vec_node_clone(n->u.binary.left);
				vec_node *du = vec_node_deriv(n->u.binary.left, var_name);
				if (!u || !du) {
					vec_node_destroy(u);
					vec_node_destroy(du);
					return NULL;
				}
				vec_node *cnode = vec_node_number_new(vec_rat_number(vec_rat_int(c)));
				vec_node *upow = vec_node_binary_new('^', u,
								     vec_node_number_new(vec_rat_number(vec_rat_int(c - 1))));
				vec_node *mul = vec_node_binary_new('*', vec_node_binary_new('*', cnode, upow), du);
				return vec_node_simplify_owned(mul);
			}
			return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
		}
		default:
			return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
		}
	}
	case VEC_NODE_CALL: {
		/* Very small set: sin/cos/exp/ln with 1 arg. */
		if (!n->u.call.name || n->u.call.argc != 1)
			return vec_node_number_new(vec_rat_number(vec_rat_int(0)));

		const char *name = n->u.call.name;
		const vec_node *u = n->u.call.args[0];
		vec_node *du = vec_node_deriv(u, var_name);
		if (!du)
			return NULL;

		if (!strcmp(name, "sin")) {
			vec_node *arg = vec_node_clone(u);
			vec_node **args = calloc(1, sizeof(args[0]));
			if (!arg || !args) {
				vec_node_destroy(arg);
				free(args);
				vec_node_destroy(du);
				return NULL;
			}
			args[0] = arg;
			vec_node *cosu = vec_node_call_new("cos", 3, 1, args);
			if (!cosu) {
				vec_node_destroy(du);
				return NULL;
			}
			return vec_node_simplify_owned(vec_node_binary_new('*', cosu, du));
		}
		if (!strcmp(name, "cos")) {
			vec_node *arg = vec_node_clone(u);
			vec_node **args = calloc(1, sizeof(args[0]));
			if (!arg || !args) {
				vec_node_destroy(arg);
				free(args);
				vec_node_destroy(du);
				return NULL;
			}
			args[0] = arg;
			vec_node *sinu = vec_node_call_new("sin", 3, 1, args);
			if (!sinu) {
				vec_node_destroy(du);
				return NULL;
			}
			vec_node *neg = vec_node_unary_new('-', sinu);
			return vec_node_simplify_owned(vec_node_binary_new('*', neg, du));
		}
		if (!strcmp(name, "exp")) {
			vec_node *arg = vec_node_clone(u);
			vec_node **args = calloc(1, sizeof(args[0]));
			if (!arg || !args) {
				vec_node_destroy(arg);
				free(args);
				vec_node_destroy(du);
				return NULL;
			}
			args[0] = arg;
			vec_node *expu = vec_node_call_new("exp", 3, 1, args);
			if (!expu) {
				vec_node_destroy(du);
				return NULL;
			}
			return vec_node_simplify_owned(vec_node_binary_new('*', expu, du));
		}
		if (!strcmp(name, "ln")) {
			vec_node *one = vec_node_number_new(vec_rat_number(vec_rat_int(1)));
			vec_node *arg = vec_node_clone(u);
			if (!one || !arg) {
				vec_node_destroy(one);
				vec_node_destroy(arg);
				vec_node_destroy(du);
				return NULL;
			}
			vec_node *inv = vec_node_binary_new('/', one, arg);
			return vec_node_simplify_owned(vec_node_binary_new('*', inv, du));
		}

		vec_node_destroy(du);
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	}
	case VEC_NODE_COMPARE:
	default:
		return vec_node_number_new(vec_rat_number(vec_rat_int(0)));
	}
}

size_t vec_node_size(const vec_node *n)
{
	if (!n)
		return 0;
	switch (n->kind) {
	case VEC_NODE_NUMBER:
	case VEC_NODE_IDENT:
		return 1;
	case VEC_NODE_UNARY:
		return 1 + vec_node_size(n->u.unary.x);
	case VEC_NODE_BINARY:
		return 1 + vec_node_size(n->u.binary.left) + vec_node_size(n->u.binary.right);
	case VEC_NODE_COMPARE:
		return 1 + vec_node_size(n->u.compare.left) + vec_node_size(n->u.compare.right);
	case VEC_NODE_CALL: {
		size_t sum = 1;
		for (size_t i = 0; i < n->u.call.argc; i++)
			sum += vec_node_size(n->u.call.args[i]);
		return sum;
	}
	default:
		return 1;
	}
}

static void buf_init(char *buf, size_t bufsz, size_t *pos)
{
	if (bufsz)
		buf[0] = 0;
	*pos = 0;
}

static void buf_append(char *buf, size_t bufsz, size_t *pos, const char *s)
{
	if (!buf || bufsz == 0 || !pos || !s)
		return;
	if (*pos >= bufsz - 1)
		return;
	size_t n = strlen(s);
	size_t avail = bufsz - 1 - *pos;
	if (n > avail)
		n = avail;
	memcpy(buf + *pos, s, n);
	*pos += n;
	buf[*pos] = 0;
}

static void node_to_string_prec(const vec_node *n, int parent_prec, char *buf, size_t bufsz, size_t *pos);

static void node_to_string_prec(const vec_node *n, int parent_prec, char *buf, size_t bufsz, size_t *pos)
{
	if (!n) {
		buf_append(buf, bufsz, pos, "<?>");
		return;
	}

	switch (n->kind) {
	case VEC_NODE_NUMBER: {
		char tmp[64];
		buf_append(buf, bufsz, pos, vec_number_string(n->u.number, 12, tmp, sizeof(tmp)));
		break;
	}
	case VEC_NODE_IDENT:
		buf_append(buf, bufsz, pos, n->u.ident.name ? n->u.ident.name : "?");
		break;
	case VEC_NODE_UNARY: {
		int prec = 4;
		char op[2] = {n->u.unary.op, 0};
		int need = prec < parent_prec;
		if (need)
			buf_append(buf, bufsz, pos, "(");
		buf_append(buf, bufsz, pos, op);
		node_to_string_prec(n->u.unary.x, prec, buf, bufsz, pos);
		if (need)
			buf_append(buf, bufsz, pos, ")");
		break;
	}
	case VEC_NODE_COMPARE: {
		int prec = 0;
		int need = prec < parent_prec;
		if (need)
			buf_append(buf, bufsz, pos, "(");
		node_to_string_prec(n->u.compare.left, prec, buf, bufsz, pos);
		buf_append(buf, bufsz, pos, " ");
		buf_append(buf, bufsz, pos, cmp_text(n->u.compare.op));
		buf_append(buf, bufsz, pos, " ");
		node_to_string_prec(n->u.compare.right, prec, buf, bufsz, pos);
		if (need)
			buf_append(buf, bufsz, pos, ")");
		break;
	}
	case VEC_NODE_BINARY: {
		int prec = bin_prec(n->u.binary.op);
		int need = prec < parent_prec;
		if (need)
			buf_append(buf, bufsz, pos, "(");
		node_to_string_prec(n->u.binary.left, prec, buf, bufsz, pos);
		buf_append(buf, bufsz, pos, " ");
		{
			char op[2] = {n->u.binary.op, 0};
			buf_append(buf, bufsz, pos, op);
		}
		buf_append(buf, bufsz, pos, " ");
		int right_prec = prec;
		if (n->u.binary.op == '^')
			right_prec = prec - 1;
		node_to_string_prec(n->u.binary.right, right_prec, buf, bufsz, pos);
		if (need)
			buf_append(buf, bufsz, pos, ")");
		break;
	}
	case VEC_NODE_CALL:
		buf_append(buf, bufsz, pos, n->u.call.name ? n->u.call.name : "?");
		buf_append(buf, bufsz, pos, "(");
		for (size_t i = 0; i < n->u.call.argc; i++) {
			if (i)
				buf_append(buf, bufsz, pos, ", ");
			node_to_string_prec(n->u.call.args[i], 0, buf, bufsz, pos);
		}
		buf_append(buf, bufsz, pos, ")");
		break;
	default:
		buf_append(buf, bufsz, pos, "<?>");
		break;
	}
}

int vec_node_to_string(const vec_node *n, char *buf, size_t bufsz)
{
	size_t pos = 0;
	if (!buf || bufsz == 0)
		return -1;
	buf_init(buf, bufsz, &pos);
	node_to_string_prec(n, 0, buf, bufsz, &pos);
	return 0;
}
