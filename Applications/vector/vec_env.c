#include "vec_env.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *vec_strdup2(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	memcpy(out, s, n + 1);
	return out;
}

void vec_env_init(vec_env *e)
{
	if (!e)
		return;
	memset(e, 0, sizeof(*e));
	e->mode = VEC_MODE_FLOAT;
	e->prec = 12;

	(void)vec_env_set_var(e, "pi", vec_value_number(vec_float(M_PI)));
	(void)vec_env_set_var(e, "tau", vec_value_number(vec_float(2 * M_PI)));
	(void)vec_env_set_var(e, "e", vec_value_number(vec_float(M_E)));
	(void)vec_env_set_var(e, "phi", vec_value_number(vec_float((1 + sqrt(5.0)) / 2)));
	(void)vec_env_set_var(e, "sqrt2", vec_value_number(vec_float(M_SQRT2)));
	(void)vec_env_set_var(e, "sqrt3", vec_value_number(vec_float(sqrt(3.0))));
	(void)vec_env_set_var(e, "sqrt5", vec_value_number(vec_float(sqrt(5.0))));
	(void)vec_env_set_var(e, "ln2", vec_value_number(vec_float(M_LN2)));
	(void)vec_env_set_var(e, "ln10", vec_value_number(vec_float(M_LN10)));
	(void)vec_env_set_var(e, "i", vec_value_complex(0, 1));
}

void vec_env_destroy(vec_env *e)
{
	if (!e)
		return;
	vec_var *v = e->vars;
	while (v) {
		vec_var *n = v->next;
		free(v->name);
		vec_value_destroy(&v->value);
		free(v);
		v = n;
	}
	e->vars = NULL;

	vec_userfunc *f = e->funcs;
	while (f) {
		vec_userfunc *n = f->next;
		free(f->name);
		free(f->param);
		vec_node_destroy(f->body);
		free(f);
		f = n;
	}
	e->funcs = NULL;
}

int vec_env_get_var(vec_env *e, const char *name, vec_value *out)
{
	if (!e || !name || !out)
		return -1;
	for (vec_var *v = e->vars; v; v = v->next) {
		if (!strcmp(v->name, name)) {
			if (vec_value_clone(out, &v->value) != 0)
				return -1;
			return 0;
		}
	}
	return 1;
}

int vec_env_set_var(vec_env *e, const char *name, vec_value v)
{
	if (!e || !name)
		return -1;
	for (vec_var *x = e->vars; x; x = x->next) {
		if (!strcmp(x->name, name)) {
			vec_value_destroy(&x->value);
			x->value = v;
			return 0;
		}
	}

	vec_var *nv = calloc(1, sizeof(*nv));
	if (!nv)
		return -1;
	nv->name = vec_strdup2(name);
	if (!nv->name) {
		free(nv);
		return -1;
	}
	nv->value = v;
	nv->next = e->vars;
	e->vars = nv;
	return 0;
}

int vec_env_set_func(vec_env *e, const char *name, const char *param, vec_node *body)
{
	if (!e || !name || !param || !body)
		return -1;
	for (vec_userfunc *x = e->funcs; x; x = x->next) {
		if (!strcmp(x->name, name)) {
			free(x->param);
			vec_node_destroy(x->body);
			x->param = vec_strdup2(param);
			x->body = body;
			return x->param ? 0 : -1;
		}
	}

	vec_userfunc *nf = calloc(1, sizeof(*nf));
	if (!nf)
		return -1;
	nf->name = vec_strdup2(name);
	nf->param = vec_strdup2(param);
	nf->body = body;
	if (!nf->name || !nf->param) {
		free(nf->name);
		free(nf->param);
		vec_node_destroy(body);
		free(nf);
		return -1;
	}
	nf->next = e->funcs;
	e->funcs = nf;
	return 0;
}

int vec_env_get_func(vec_env *e, const char *name, const vec_userfunc **out)
{
	if (!e || !name || !out)
		return -1;
	for (vec_userfunc *f = e->funcs; f; f = f->next) {
		if (!strcmp(f->name, name)) {
			*out = f;
			return 0;
		}
	}
	return -1;
}
