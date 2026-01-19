#ifndef VEC_PLOT_H
#define VEC_PLOT_H

#include "vec_ast.h"
#include "vec_env.h"
#include "vec_fb.h"

#include <stddef.h>

int vec_plot_render(struct vec_fb *fb, int x0, int y0, int w, int h,
		    vec_env *env, const vec_node *expr,
		    double x_min, double x_max, double y_min, double y_max,
		    char *err, size_t errsz);

#endif

