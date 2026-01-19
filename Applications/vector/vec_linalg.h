#ifndef VEC_LINALG_H
#define VEC_LINALG_H

#include "vec_matrix.h"

#include <stddef.h>

vec_mat_err vec_solve_linear_system(const double *a, const double *b, int n, double **out);
vec_mat_err vec_solve_linear_system_multi(const double *a, const double *b, int n, int m, double **out);

#endif

