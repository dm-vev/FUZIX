#ifndef VEC_MATRIX_H
#define VEC_MATRIX_H

#include <stddef.h>

typedef enum {
	VEC_MAT_OK = 0,
	VEC_MAT_ERR_INVALID = -1,
	VEC_MAT_ERR_SHAPE = -2,
	VEC_MAT_ERR_NOMEM = -3,
	VEC_MAT_ERR_SINGULAR = -4,
} vec_mat_err;

vec_mat_err vec_matrix_zeros(int rows, int cols, double **out);
vec_mat_err vec_matrix_ones(int rows, int cols, double **out);
vec_mat_err vec_matrix_eye(int n, double **out);

vec_mat_err vec_matrix_transpose(int rows, int cols, const double *a, double **out);
vec_mat_err vec_matrix_add(int rows, int cols, const double *a, const double *b, double **out);
vec_mat_err vec_matrix_sub(int rows, int cols, const double *a, const double *b, double **out);
vec_mat_err vec_matrix_scale(int rows, int cols, const double *a, double k, double **out);
vec_mat_err vec_matrix_mul(int a_rows, int a_cols, const double *a, int b_rows, int b_cols, const double *b, double **out);
vec_mat_err vec_matrix_mul_vec(int a_rows, int a_cols, const double *a, const double *x, size_t xlen, double **out);

vec_mat_err vec_matrix_det(int rows, int cols, const double *a, double *out_det);
vec_mat_err vec_matrix_inv(int rows, int cols, const double *a, double **out);

#endif

