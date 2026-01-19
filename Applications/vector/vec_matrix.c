#include "vec_matrix.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int mul_size(size_t a, size_t b, size_t *out)
{
	if (!out)
		return -1;
	if (a && b > SIZE_MAX / a)
		return -1;
	*out = a * b;
	return 0;
}

static vec_mat_err matrix_check(int rows, int cols, const double *data)
{
	if (rows <= 0 || cols <= 0)
		return VEC_MAT_ERR_INVALID;
	if (!data)
		return VEC_MAT_ERR_INVALID;
	size_t n;
	if (mul_size((size_t)rows, (size_t)cols, &n) != 0)
		return VEC_MAT_ERR_INVALID;
	(void)n;
	return VEC_MAT_OK;
}

static vec_mat_err matrix_alloc(int rows, int cols, int zero, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (rows <= 0 || cols <= 0)
		return VEC_MAT_ERR_INVALID;
	size_t n;
	if (mul_size((size_t)rows, (size_t)cols, &n) != 0)
		return VEC_MAT_ERR_INVALID;
	if (!n)
		return VEC_MAT_ERR_INVALID;

	double *p = zero ? calloc(n, sizeof(p[0])) : malloc(n * sizeof(p[0]));
	if (!p)
		return VEC_MAT_ERR_NOMEM;
	*out = p;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_zeros(int rows, int cols, double **out)
{
	return matrix_alloc(rows, cols, 1, out);
}

vec_mat_err vec_matrix_ones(int rows, int cols, double **out)
{
	double *m;
	vec_mat_err rc = matrix_alloc(rows, cols, 0, &m);
	if (rc != VEC_MAT_OK)
		return rc;
	size_t n = (size_t)rows * (size_t)cols;
	for (size_t i = 0; i < n; i++)
		m[i] = 1;
	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_eye(int n, double **out)
{
	double *m;
	vec_mat_err rc = vec_matrix_zeros(n, n, &m);
	if (rc != VEC_MAT_OK)
		return rc;
	for (int i = 0; i < n; i++)
		m[(size_t)i * (size_t)n + (size_t)i] = 1;
	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_transpose(int rows, int cols, const double *a, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(rows, cols, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;

	double *m;
	vec_mat_err rc = matrix_alloc(rows, cols, 0, &m);
	if (rc != VEC_MAT_OK)
		return rc;

	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			m[(size_t)c * (size_t)rows + (size_t)r] = a[(size_t)r * (size_t)cols + (size_t)c];
		}
	}
	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_add(int rows, int cols, const double *a, const double *b, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(rows, cols, a) != VEC_MAT_OK || matrix_check(rows, cols, b) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;

	double *m;
	vec_mat_err rc = matrix_alloc(rows, cols, 0, &m);
	if (rc != VEC_MAT_OK)
		return rc;

	size_t n = (size_t)rows * (size_t)cols;
	for (size_t i = 0; i < n; i++)
		m[i] = a[i] + b[i];
	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_sub(int rows, int cols, const double *a, const double *b, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(rows, cols, a) != VEC_MAT_OK || matrix_check(rows, cols, b) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;

	double *m;
	vec_mat_err rc = matrix_alloc(rows, cols, 0, &m);
	if (rc != VEC_MAT_OK)
		return rc;

	size_t n = (size_t)rows * (size_t)cols;
	for (size_t i = 0; i < n; i++)
		m[i] = a[i] - b[i];
	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_scale(int rows, int cols, const double *a, double k, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(rows, cols, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;

	double *m;
	vec_mat_err rc = matrix_alloc(rows, cols, 0, &m);
	if (rc != VEC_MAT_OK)
		return rc;

	size_t n = (size_t)rows * (size_t)cols;
	for (size_t i = 0; i < n; i++)
		m[i] = a[i] * k;
	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_mul(int a_rows, int a_cols, const double *a, int b_rows, int b_cols, const double *b, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(a_rows, a_cols, a) != VEC_MAT_OK || matrix_check(b_rows, b_cols, b) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;
	if (a_cols != b_rows)
		return VEC_MAT_ERR_SHAPE;

	size_t out_elems;
	if (mul_size((size_t)a_rows, (size_t)b_cols, &out_elems) != 0)
		return VEC_MAT_ERR_INVALID;
	double *m = calloc(out_elems, sizeof(m[0]));
	if (!m)
		return VEC_MAT_ERR_NOMEM;

	for (int i = 0; i < a_rows; i++) {
		for (int k = 0; k < a_cols; k++) {
			double aik = a[(size_t)i * (size_t)a_cols + (size_t)k];
			if (aik == 0)
				continue;
			for (int j = 0; j < b_cols; j++)
				m[(size_t)i * (size_t)b_cols + (size_t)j] += aik * b[(size_t)k * (size_t)b_cols + (size_t)j];
		}
	}

	*out = m;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_mul_vec(int a_rows, int a_cols, const double *a, const double *x, size_t xlen, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(a_rows, a_cols, a) != VEC_MAT_OK || !x)
		return VEC_MAT_ERR_INVALID;
	if ((size_t)a_cols != xlen)
		return VEC_MAT_ERR_SHAPE;

	double *v = malloc(sizeof(v[0]) * (size_t)a_rows);
	if (!v)
		return VEC_MAT_ERR_NOMEM;

	for (int i = 0; i < a_rows; i++) {
		double s = 0;
		const double *row = &a[(size_t)i * (size_t)a_cols];
		for (int j = 0; j < a_cols; j++)
			s += row[j] * x[(size_t)j];
		v[(size_t)i] = s;
	}

	*out = v;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_det(int rows, int cols, const double *a, double *out_det)
{
	if (!out_det)
		return VEC_MAT_ERR_INVALID;
	*out_det = 0;
	if (matrix_check(rows, cols, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;
	if (rows != cols)
		return VEC_MAT_ERR_SHAPE;

	int n = rows;
	size_t elems = (size_t)n * (size_t)n;
	double *m = malloc(sizeof(m[0]) * elems);
	if (!m)
		return VEC_MAT_ERR_NOMEM;
	memcpy(m, a, sizeof(m[0]) * elems);
	double sign = 1.0;

	for (int k = 0; k < n; k++) {
		int pivot = k;
		double max_abs = fabs(m[(size_t)k * (size_t)n + (size_t)k]);
		for (int i = k + 1; i < n; i++) {
			double v = fabs(m[(size_t)i * (size_t)n + (size_t)k]);
			if (v > max_abs) {
				max_abs = v;
				pivot = i;
			}
		}
		if (max_abs == 0) {
			free(m);
			*out_det = 0;
			return VEC_MAT_OK;
		}
		if (pivot != k) {
			for (int j = 0; j < n; j++) {
				double tmp = m[(size_t)k * (size_t)n + (size_t)j];
				m[(size_t)k * (size_t)n + (size_t)j] = m[(size_t)pivot * (size_t)n + (size_t)j];
				m[(size_t)pivot * (size_t)n + (size_t)j] = tmp;
			}
			sign = -sign;
		}
		double p = m[(size_t)k * (size_t)n + (size_t)k];
		for (int i = k + 1; i < n; i++) {
			double f = m[(size_t)i * (size_t)n + (size_t)k] / p;
			m[(size_t)i * (size_t)n + (size_t)k] = f;
			for (int j = k + 1; j < n; j++)
				m[(size_t)i * (size_t)n + (size_t)j] -= f * m[(size_t)k * (size_t)n + (size_t)j];
		}
	}

	double det = sign;
	for (int i = 0; i < n; i++)
		det *= m[(size_t)i * (size_t)n + (size_t)i];
	free(m);
	*out_det = det;
	return VEC_MAT_OK;
}

vec_mat_err vec_matrix_inv(int rows, int cols, const double *a, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (matrix_check(rows, cols, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;
	if (rows != cols)
		return VEC_MAT_ERR_SHAPE;
	int n = rows;

	size_t nn = (size_t)n;
	size_t row_len = nn * 2;
	size_t aug_elems;
	if (mul_size(nn, row_len, &aug_elems) != 0)
		return VEC_MAT_ERR_INVALID;
	double *aug = calloc(aug_elems, sizeof(aug[0]));
	if (!aug)
		return VEC_MAT_ERR_NOMEM;

	for (int r = 0; r < n; r++) {
		memcpy(&aug[(size_t)r * row_len], &a[(size_t)r * nn], nn * sizeof(a[0]));
		aug[(size_t)r * row_len + nn + (size_t)r] = 1;
	}

	for (int col = 0; col < n; col++) {
		int pivot = col;
		double max_abs = fabs(aug[(size_t)col * row_len + (size_t)col]);
		for (int r = col + 1; r < n; r++) {
			double v = fabs(aug[(size_t)r * row_len + (size_t)col]);
			if (v > max_abs) {
				max_abs = v;
				pivot = r;
			}
		}
		if (max_abs == 0) {
			free(aug);
			return VEC_MAT_ERR_SINGULAR;
		}
		if (pivot != col) {
			for (size_t j = 0; j < row_len; j++) {
				double tmp = aug[(size_t)col * row_len + j];
				aug[(size_t)col * row_len + j] = aug[(size_t)pivot * row_len + j];
				aug[(size_t)pivot * row_len + j] = tmp;
			}
		}

		double p = aug[(size_t)col * row_len + (size_t)col];
		double inv_p = 1 / p;
		for (size_t j = 0; j < row_len; j++)
			aug[(size_t)col * row_len + j] *= inv_p;

		for (int r = 0; r < n; r++) {
			if (r == col)
				continue;
			double f = aug[(size_t)r * row_len + (size_t)col];
			if (f == 0)
				continue;
			for (size_t j = 0; j < row_len; j++)
				aug[(size_t)r * row_len + j] -= f * aug[(size_t)col * row_len + j];
		}
	}

	double *inv = malloc(nn * nn * sizeof(inv[0]));
	if (!inv) {
		free(aug);
		return VEC_MAT_ERR_NOMEM;
	}
	for (int r = 0; r < n; r++) {
		memcpy(&inv[(size_t)r * nn], &aug[(size_t)r * row_len + nn], nn * sizeof(inv[0]));
	}

	free(aug);
	*out = inv;
	return VEC_MAT_OK;
}

