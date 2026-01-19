#ifndef VEC_PLOT_H
#define VEC_PLOT_H

#include "vec_ast.h"
#include "vec_env.h"
#include "vec_fb.h"

#include <stddef.h>

typedef enum {
	VEC_PLOT_FUNC = 0,
	VEC_PLOT_SERIES = 1,
} vec_plot_kind;

typedef struct {
	vec_plot_kind kind;
	const vec_node *expr;
	const float *xs;
	const float *ys;
	size_t len;
} vec_plot;

int vec_plot_render(struct vec_fb *fb, int x0, int y0, int w, int h,
		    vec_env *env, const vec_node *expr,
		    double x_min, double x_max, double y_min, double y_max,
		    char *err, size_t errsz);

int vec_plot_render_multi(struct vec_fb *fb, int x0, int y0, int w, int h,
			  vec_env *env, const vec_plot *plots, size_t plot_count,
			  const vec_node *fallback_expr,
			  double x_min, double x_max, double y_min, double y_max,
			  char *err, size_t errsz);

#endif
