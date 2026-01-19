#ifndef VEC_ENV_H
#define VEC_ENV_H

#include "vec_ast.h"
#include "vec_value.h"

typedef enum {
	VEC_MODE_FLOAT = 0,
	VEC_MODE_EXACT = 1,
} vec_mode;

typedef struct vec_var vec_var;
typedef struct vec_userfunc vec_userfunc;

struct vec_var {
	char *name;
	vec_value value;
	vec_var *next;
};

struct vec_userfunc {
	char *name;
	char *param;
	vec_node *body;
	vec_userfunc *next;
};

typedef struct {
	vec_mode mode;
	int prec;
	vec_var *vars;
	vec_userfunc *funcs;
} vec_env;

void vec_env_init(vec_env *e);
void vec_env_destroy(vec_env *e);

int vec_env_get_var(vec_env *e, const char *name, vec_value *out);
int vec_env_set_var(vec_env *e, const char *name, vec_value v);

int vec_env_set_func(vec_env *e, const char *name, const char *param, vec_node *body);
int vec_env_get_func(vec_env *e, const char *name, const vec_userfunc **out);

#endif

