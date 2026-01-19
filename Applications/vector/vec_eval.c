#include "vec_eval.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int vec_eval_node_impl(vec_env *e, const vec_node *n, const char *ov_name, vec_value ov_value,
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

static int eval_call(vec_env *e, const vec_node *n, const char *ov_name, vec_value ov_value,
		     vec_value *out, char *err, size_t errsz)
{
	if (!n || n->kind != VEC_NODE_CALL || !n->u.call.name) {
		snprintf(err, errsz, "eval: bad call");
		return -1;
	}

	size_t argc = n->u.call.argc;
	if (argc > 8) {
		snprintf(err, errsz, "eval: too many args");
		return -1;
	}

	vec_value args[8];
	double fargs[8];
	memset(args, 0, sizeof(args));
	memset(fargs, 0, sizeof(fargs));

	for (size_t i = 0; i < argc; i++) {
		if (vec_eval_node_impl(e, n->u.call.args[i], ov_name, ov_value, &args[i], err, errsz) != 0)
			return -1;
		if (args[i].kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: non-number argument");
			return -1;
		}
		fargs[i] = vec_number_float64(args[i].num);
	}

	const char *name = n->u.call.name;

	/* Builtins (minimal subset). */
	if (!strcmp(name, "sin") && argc == 1) {
		*out = vec_value_number(vec_float(sin(fargs[0])));
		return 0;
	}
	if (!strcmp(name, "cos") && argc == 1) {
		*out = vec_value_number(vec_float(cos(fargs[0])));
		return 0;
	}
	if (!strcmp(name, "tan") && argc == 1) {
		double c = cos(fargs[0]);
		if (c == 0) {
			snprintf(err, errsz, "eval: tan: division by zero");
			return -1;
		}
		*out = vec_value_number(vec_float(sin(fargs[0]) / c));
		return 0;
	}
	if ((!strcmp(name, "ln") || !strcmp(name, "log")) && argc == 1) {
		*out = vec_value_number(vec_float(log(fargs[0])));
		return 0;
	}
	if (!strcmp(name, "exp") && argc == 1) {
		*out = vec_value_number(vec_float(exp(fargs[0])));
		return 0;
	}
	if (!strcmp(name, "sqrt") && argc == 1) {
		*out = vec_value_number(vec_float(sqrt(fargs[0])));
		return 0;
	}
	if (!strcmp(name, "abs") && argc == 1) {
		*out = vec_value_number(vec_float(fabs(fargs[0])));
		return 0;
	}
	if (!strcmp(name, "min") && argc >= 1) {
		double m = fargs[0];
		for (size_t i = 1; i < argc; i++)
			if (fargs[i] < m)
				m = fargs[i];
		*out = vec_value_number(vec_float(m));
		return 0;
	}
	if (!strcmp(name, "max") && argc >= 1) {
		double m = fargs[0];
		for (size_t i = 1; i < argc; i++)
			if (fargs[i] > m)
				m = fargs[i];
		*out = vec_value_number(vec_float(m));
		return 0;
	}
	if (!strcmp(name, "atan2") && argc == 2) {
		*out = vec_value_number(vec_float(atan2(fargs[0], fargs[1])));
		return 0;
	}

	/* User-defined single-arg functions: f(x)=... */
	const vec_userfunc *uf;
	if (vec_env_get_func(e, name, &uf) == 0 && uf && uf->body && uf->param) {
		if (argc != 1) {
			snprintf(err, errsz, "eval: %s expects 1 argument", name);
			return -1;
		}
		return vec_eval_node_impl(e, uf->body, uf->param, args[0], out, err, errsz);
	}

	snprintf(err, errsz, "eval: unknown function '%s'", name);
	return -1;
}

static int eval_compare(vec_env *e, vec_cmp_op op, vec_value a, vec_value b, vec_value *out, char *err, size_t errsz)
{
	(void)e;
	if (a.kind != VEC_VALUE_NUMBER || b.kind != VEC_VALUE_NUMBER) {
		snprintf(err, errsz, "eval: unsupported comparison");
		return -1;
	}
	double af = vec_number_float64(a.num);
	double bf = vec_number_float64(b.num);
	int ok = 0;
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
	*out = vec_value_number(vec_rat_number(vec_rat_int(ok ? 1 : 0)));
	return 0;
}

static int vec_eval_node_impl(vec_env *e, const vec_node *n, const char *ov_name, vec_value ov_value,
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
			*out = ov_value;
			return 0;
		}
		if (vec_env_get_var(e, n->u.ident.name, out) == 0)
			return 0;
		snprintf(err, errsz, "eval: unknown variable '%s'", n->u.ident.name ? n->u.ident.name : "?");
		return -1;
	case VEC_NODE_UNARY: {
		vec_value x;
		if (vec_eval_node_impl(e, n->u.unary.x, ov_name, ov_value, &x, err, errsz) != 0)
			return -1;
		if (x.kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: unary on non-number");
			return -1;
		}
		switch (n->u.unary.op) {
		case '+':
			*out = x;
			return 0;
		case '-':
			*out = vec_value_number(neg_number(x.num));
			return 0;
		default:
			snprintf(err, errsz, "eval: bad unary op");
			return -1;
		}
	}
	case VEC_NODE_BINARY: {
		vec_value a, b;
		if (vec_eval_node_impl(e, n->u.binary.left, ov_name, ov_value, &a, err, errsz) != 0)
			return -1;
		if (vec_eval_node_impl(e, n->u.binary.right, ov_name, ov_value, &b, err, errsz) != 0)
			return -1;
		if (a.kind != VEC_VALUE_NUMBER || b.kind != VEC_VALUE_NUMBER) {
			snprintf(err, errsz, "eval: binary on non-number");
			return -1;
		}
		switch (n->u.binary.op) {
		case '+':
			*out = vec_value_number(add_number(e, a.num, b.num));
			return 0;
		case '-':
			*out = vec_value_number(sub_number(e, a.num, b.num));
			return 0;
		case '*':
			*out = vec_value_number(mul_number(e, a.num, b.num));
			return 0;
		case '/': {
			vec_number r;
			if (div_number(e, a.num, b.num, &r, err, errsz) != 0)
				return -1;
			*out = vec_value_number(r);
			return 0;
		}
		case '^':
			*out = vec_value_number(pow_number(e, a.num, b.num));
			return 0;
		default:
			snprintf(err, errsz, "eval: bad binary op");
			return -1;
		}
	}
	case VEC_NODE_CALL:
		return eval_call(e, n, ov_name, ov_value, out, err, errsz);
	case VEC_NODE_COMPARE: {
		vec_value a, b;
		if (vec_eval_node_impl(e, n->u.compare.left, ov_name, ov_value, &a, err, errsz) != 0)
			return -1;
		if (vec_eval_node_impl(e, n->u.compare.right, ov_name, ov_value, &b, err, errsz) != 0)
			return -1;
		return eval_compare(e, n->u.compare.op, a, b, out, err, errsz);
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
	return vec_eval_node_impl(e, n, NULL, dummy, out, err, errsz);
}

int vec_eval_node_override(vec_env *e, const vec_node *n, const char *ov_name, vec_value ov_value,
			   vec_value *out, char *err, size_t errsz)
{
	return vec_eval_node_impl(e, n, ov_name, ov_value, out, err, errsz);
}
