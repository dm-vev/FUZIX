#include "vec_linalg.h"

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

vec_mat_err vec_solve_linear_system(const double *a, const double *b, int n, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (!a || !b || n <= 0)
		return VEC_MAT_ERR_INVALID;

	size_t nn;
	if (mul_size((size_t)n, (size_t)n, &nn) != 0)
		return VEC_MAT_ERR_INVALID;

	double *aa = malloc(sizeof(aa[0]) * nn);
	if (!aa)
		return VEC_MAT_ERR_NOMEM;
	double *bb = malloc(sizeof(bb[0]) * (size_t)n);
	if (!bb) {
		free(aa);
		return VEC_MAT_ERR_NOMEM;
	}
	memcpy(aa, a, sizeof(aa[0]) * nn);
	memcpy(bb, b, sizeof(bb[0]) * (size_t)n);

	for (int k = 0; k < n; k++) {
		int piv = k;
		double max_abs = fabs(aa[(size_t)k * (size_t)n + (size_t)k]);
		for (int i = k + 1; i < n; i++) {
			double v = fabs(aa[(size_t)i * (size_t)n + (size_t)k]);
			if (v > max_abs) {
				max_abs = v;
				piv = i;
			}
		}
		if (max_abs == 0 || isnan(max_abs) || isinf(max_abs)) {
			free(aa);
			free(bb);
			return VEC_MAT_ERR_SINGULAR;
		}
		if (piv != k) {
			for (int j = k; j < n; j++) {
				double tmp = aa[(size_t)k * (size_t)n + (size_t)j];
				aa[(size_t)k * (size_t)n + (size_t)j] = aa[(size_t)piv * (size_t)n + (size_t)j];
				aa[(size_t)piv * (size_t)n + (size_t)j] = tmp;
			}
			double tmpb = bb[(size_t)k];
			bb[(size_t)k] = bb[(size_t)piv];
			bb[(size_t)piv] = tmpb;
		}

		double pivot = aa[(size_t)k * (size_t)n + (size_t)k];
		for (int i = k + 1; i < n; i++) {
			double f = aa[(size_t)i * (size_t)n + (size_t)k] / pivot;
			if (f == 0)
				continue;
			aa[(size_t)i * (size_t)n + (size_t)k] = 0;
			for (int j = k + 1; j < n; j++)
				aa[(size_t)i * (size_t)n + (size_t)j] -= f * aa[(size_t)k * (size_t)n + (size_t)j];
			bb[(size_t)i] -= f * bb[(size_t)k];
		}
	}

	double *x = malloc(sizeof(x[0]) * (size_t)n);
	if (!x) {
		free(aa);
		free(bb);
		return VEC_MAT_ERR_NOMEM;
	}

	for (int i = n - 1; i >= 0; i--) {
		double sum = bb[(size_t)i];
		for (int j = i + 1; j < n; j++)
			sum -= aa[(size_t)i * (size_t)n + (size_t)j] * x[(size_t)j];
		double pivot = aa[(size_t)i * (size_t)n + (size_t)i];
		if (pivot == 0) {
			free(aa);
			free(bb);
			free(x);
			return VEC_MAT_ERR_SINGULAR;
		}
		x[(size_t)i] = sum / pivot;
	}

	free(aa);
	free(bb);
	*out = x;
	return VEC_MAT_OK;
}

vec_mat_err vec_solve_linear_system_multi(const double *a, const double *b, int n, int m, double **out)
{
	if (!out)
		return VEC_MAT_ERR_INVALID;
	*out = NULL;
	if (!a || !b || n <= 0 || m <= 0)
		return VEC_MAT_ERR_INVALID;

	size_t nn;
	if (mul_size((size_t)n, (size_t)n, &nn) != 0)
		return VEC_MAT_ERR_INVALID;

	size_t nm;
	if (mul_size((size_t)n, (size_t)m, &nm) != 0)
		return VEC_MAT_ERR_INVALID;

	double *aa = malloc(sizeof(aa[0]) * nn);
	if (!aa)
		return VEC_MAT_ERR_NOMEM;
	double *bb = malloc(sizeof(bb[0]) * nm);
	if (!bb) {
		free(aa);
		return VEC_MAT_ERR_NOMEM;
	}
	memcpy(aa, a, sizeof(aa[0]) * nn);
	memcpy(bb, b, sizeof(bb[0]) * nm);

	for (int k = 0; k < n; k++) {
		int piv = k;
		double max_abs = fabs(aa[(size_t)k * (size_t)n + (size_t)k]);
		for (int i = k + 1; i < n; i++) {
			double v = fabs(aa[(size_t)i * (size_t)n + (size_t)k]);
			if (v > max_abs) {
				max_abs = v;
				piv = i;
			}
		}
		if (max_abs == 0 || isnan(max_abs) || isinf(max_abs)) {
			free(aa);
			free(bb);
			return VEC_MAT_ERR_SINGULAR;
		}
		if (piv != k) {
			for (int j = k; j < n; j++) {
				double tmp = aa[(size_t)k * (size_t)n + (size_t)j];
				aa[(size_t)k * (size_t)n + (size_t)j] = aa[(size_t)piv * (size_t)n + (size_t)j];
				aa[(size_t)piv * (size_t)n + (size_t)j] = tmp;
			}
			for (int j = 0; j < m; j++) {
				double tmpb = bb[(size_t)k * (size_t)m + (size_t)j];
				bb[(size_t)k * (size_t)m + (size_t)j] = bb[(size_t)piv * (size_t)m + (size_t)j];
				bb[(size_t)piv * (size_t)m + (size_t)j] = tmpb;
			}
		}

		double pivot = aa[(size_t)k * (size_t)n + (size_t)k];
		for (int i = k + 1; i < n; i++) {
			double f = aa[(size_t)i * (size_t)n + (size_t)k] / pivot;
			if (f == 0)
				continue;
			aa[(size_t)i * (size_t)n + (size_t)k] = 0;
			for (int j = k + 1; j < n; j++)
				aa[(size_t)i * (size_t)n + (size_t)j] -= f * aa[(size_t)k * (size_t)n + (size_t)j];
			for (int j = 0; j < m; j++)
				bb[(size_t)i * (size_t)m + (size_t)j] -= f * bb[(size_t)k * (size_t)m + (size_t)j];
		}
	}

	double *x = malloc(sizeof(x[0]) * nm);
	if (!x) {
		free(aa);
		free(bb);
		return VEC_MAT_ERR_NOMEM;
	}

	for (int i = n - 1; i >= 0; i--) {
		double pivot = aa[(size_t)i * (size_t)n + (size_t)i];
		if (pivot == 0) {
			free(aa);
			free(bb);
			free(x);
			return VEC_MAT_ERR_SINGULAR;
		}
		for (int j = 0; j < m; j++) {
			double sum = bb[(size_t)i * (size_t)m + (size_t)j];
			for (int k = i + 1; k < n; k++)
				sum -= aa[(size_t)i * (size_t)n + (size_t)k] * x[(size_t)k * (size_t)m + (size_t)j];
			x[(size_t)i * (size_t)m + (size_t)j] = sum / pivot;
		}
	}

	free(aa);
	free(bb);
	*out = x;
	return VEC_MAT_OK;
}

