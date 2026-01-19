#ifndef VEC_LINALG_H
#define VEC_LINALG_H

#include "vec_matrix.h"

#include <stddef.h>

vec_mat_err vec_solve_linear_system(const double *a, const double *b, int n, double **out);
vec_mat_err vec_solve_linear_system_multi(const double *a, const double *b, int n, int m, double **out);

vec_mat_err vec_qr_decompose(int m, int n, const double *a, double **out_q, double **out_r);
vec_mat_err vec_svd_thin(int m, int n, const double *a, double **out_u, double **out_s, double **out_v);

#endif
