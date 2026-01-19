#ifndef VEC_EVAL_H
#define VEC_EVAL_H

#include "vec_env.h"

int vec_eval_node(vec_env *e, const vec_node *n, vec_value *out, char *err, size_t errsz);
int vec_eval_node_override(vec_env *e, const vec_node *n, const char *ov_name, vec_value ov_value,
			   vec_value *out, char *err, size_t errsz);

#endif
