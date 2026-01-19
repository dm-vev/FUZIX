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

struct eig_pair {
	double w;
	int i;
};

static int eig_pair_cmp_asc(const void *a, const void *b)
{
	const struct eig_pair *pa = (const struct eig_pair *)a;
	const struct eig_pair *pb = (const struct eig_pair *)b;
	if (pa->w < pb->w)
		return -1;
	if (pa->w > pb->w)
		return 1;
	return 0;
}

static vec_mat_err jacobi_eigen_sym(const double *a, int n, double **out_w, double **out_v)
{
	if (!out_w || !out_v)
		return VEC_MAT_ERR_INVALID;
	*out_w = NULL;
	*out_v = NULL;
	if (matrix_check(n, n, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;

	size_t nn;
	if (mul_size((size_t)n, (size_t)n, &nn) != 0)
		return VEC_MAT_ERR_INVALID;

	double *aa = malloc(sizeof(aa[0]) * nn);
	if (!aa)
		return VEC_MAT_ERR_NOMEM;
	memcpy(aa, a, sizeof(aa[0]) * nn);

	double *v;
	vec_mat_err rc = vec_matrix_eye(n, &v);
	if (rc != VEC_MAT_OK) {
		free(aa);
		return rc;
	}

	const int max_iter = 64;
	const double eps = 1e-12;

	for (int iter = 0; iter < max_iter; iter++) {
		int p = 0;
		int q = 0;
		double max_abs = 0;
		for (int i = 0; i < n; i++) {
			for (int j = i + 1; j < n; j++) {
				double val = fabs(aa[(size_t)i * (size_t)n + (size_t)j]);
				if (val > max_abs) {
					max_abs = val;
					p = i;
					q = j;
				}
			}
		}
		if (max_abs < eps)
			break;

		double app = aa[(size_t)p * (size_t)n + (size_t)p];
		double aqq = aa[(size_t)q * (size_t)n + (size_t)q];
		double apq = aa[(size_t)p * (size_t)n + (size_t)q];
		if (apq == 0)
			continue;

		double tau = (aqq - app) / (2 * apq);
		double t = 1 / (fabs(tau) + sqrt(1 + tau * tau));
		if (tau < 0)
			t = -t;
		double c = 1 / sqrt(1 + t * t);
		double s = t * c;

		for (int k = 0; k < n; k++) {
			if (k == p || k == q)
				continue;
			double akp = aa[(size_t)k * (size_t)n + (size_t)p];
			double akq = aa[(size_t)k * (size_t)n + (size_t)q];
			aa[(size_t)k * (size_t)n + (size_t)p] = c * akp - s * akq;
			aa[(size_t)k * (size_t)n + (size_t)q] = s * akp + c * akq;
			aa[(size_t)p * (size_t)n + (size_t)k] = aa[(size_t)k * (size_t)n + (size_t)p];
			aa[(size_t)q * (size_t)n + (size_t)k] = aa[(size_t)k * (size_t)n + (size_t)q];
		}
		aa[(size_t)p * (size_t)n + (size_t)p] = c * c * app - 2 * s * c * apq + s * s * aqq;
		aa[(size_t)q * (size_t)n + (size_t)q] = s * s * app + 2 * s * c * apq + c * c * aqq;
		aa[(size_t)p * (size_t)n + (size_t)q] = 0;
		aa[(size_t)q * (size_t)n + (size_t)p] = 0;

		for (int k = 0; k < n; k++) {
			double vkp = v[(size_t)k * (size_t)n + (size_t)p];
			double vkq = v[(size_t)k * (size_t)n + (size_t)q];
			v[(size_t)k * (size_t)n + (size_t)p] = c * vkp - s * vkq;
			v[(size_t)k * (size_t)n + (size_t)q] = s * vkp + c * vkq;
		}
	}

	double *w = malloc(sizeof(w[0]) * (size_t)n);
	if (!w) {
		free(aa);
		free(v);
		return VEC_MAT_ERR_NOMEM;
	}
	for (int i = 0; i < n; i++)
		w[(size_t)i] = aa[(size_t)i * (size_t)n + (size_t)i];

	struct eig_pair *pairs = malloc(sizeof(pairs[0]) * (size_t)n);
	if (!pairs) {
		free(aa);
		free(v);
		free(w);
		return VEC_MAT_ERR_NOMEM;
	}
	for (int i = 0; i < n; i++) {
		pairs[(size_t)i].w = w[(size_t)i];
		pairs[(size_t)i].i = i;
	}
	qsort(pairs, (size_t)n, sizeof(pairs[0]), eig_pair_cmp_asc);

	double *ws = malloc(sizeof(ws[0]) * (size_t)n);
	double *vs = malloc(sizeof(vs[0]) * nn);
	if (!ws || !vs) {
		free(aa);
		free(v);
		free(w);
		free(pairs);
		free(ws);
		free(vs);
		return VEC_MAT_ERR_NOMEM;
	}

	for (int col = 0; col < n; col++) {
		int src = pairs[(size_t)col].i;
		ws[(size_t)col] = pairs[(size_t)col].w;
		for (int r = 0; r < n; r++)
			vs[(size_t)r * (size_t)n + (size_t)col] = v[(size_t)r * (size_t)n + (size_t)src];
	}

	free(aa);
	free(v);
	free(w);
	free(pairs);

	*out_w = ws;
	*out_v = vs;
	return VEC_MAT_OK;
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

vec_mat_err vec_qr_decompose(int m, int n, const double *a, double **out_q, double **out_r)
{
	if (!out_q || !out_r)
		return VEC_MAT_ERR_INVALID;
	*out_q = NULL;
	*out_r = NULL;
	if (matrix_check(m, n, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;
	if (m < 1 || n < 1)
		return VEC_MAT_ERR_INVALID;

	size_t mn;
	if (mul_size((size_t)m, (size_t)n, &mn) != 0)
		return VEC_MAT_ERR_INVALID;
	size_t nn;
	if (mul_size((size_t)n, (size_t)n, &nn) != 0)
		return VEC_MAT_ERR_INVALID;

	double *q = malloc(sizeof(q[0]) * mn);
	if (!q)
		return VEC_MAT_ERR_NOMEM;
	memcpy(q, a, sizeof(q[0]) * mn);

	double *r = calloc(nn, sizeof(r[0]));
	if (!r) {
		free(q);
		return VEC_MAT_ERR_NOMEM;
	}

	for (int k = 0; k < n; k++) {
		double norm = 0;
		for (int i = 0; i < m; i++) {
			double v = q[(size_t)i * (size_t)n + (size_t)k];
			norm += v * v;
		}
		norm = sqrt(norm);
		if (norm == 0 || isnan(norm) || isinf(norm)) {
			free(q);
			free(r);
			return VEC_MAT_ERR_SINGULAR;
		}
		r[(size_t)k * (size_t)n + (size_t)k] = norm;
		double inv = 1 / norm;
		for (int i = 0; i < m; i++)
			q[(size_t)i * (size_t)n + (size_t)k] *= inv;

		for (int j = k + 1; j < n; j++) {
			double dot = 0;
			for (int i = 0; i < m; i++)
				dot += q[(size_t)i * (size_t)n + (size_t)k] * q[(size_t)i * (size_t)n + (size_t)j];
			r[(size_t)k * (size_t)n + (size_t)j] = dot;
			for (int i = 0; i < m; i++)
				q[(size_t)i * (size_t)n + (size_t)j] -= dot * q[(size_t)i * (size_t)n + (size_t)k];
		}
	}

	*out_q = q;
	*out_r = r;
	return VEC_MAT_OK;
}

struct sv_pair {
	double s;
	int i;
};

static int sv_pair_cmp_desc(const void *a, const void *b)
{
	const struct sv_pair *pa = (const struct sv_pair *)a;
	const struct sv_pair *pb = (const struct sv_pair *)b;
	if (pa->s > pb->s)
		return -1;
	if (pa->s < pb->s)
		return 1;
	return 0;
}

vec_mat_err vec_svd_thin(int m, int n, const double *a, double **out_u, double **out_s, double **out_v)
{
	if (!out_u || !out_s || !out_v)
		return VEC_MAT_ERR_INVALID;
	*out_u = NULL;
	*out_s = NULL;
	*out_v = NULL;
	if (matrix_check(m, n, a) != VEC_MAT_OK)
		return VEC_MAT_ERR_INVALID;
	if (n > 64)
		return VEC_MAT_ERR_INVALID;

	/* ATA = A^T A (n x n). */
	double *at = NULL;
	double *ata = NULL;
	vec_mat_err rc = vec_matrix_transpose(m, n, a, &at);
	if (rc != VEC_MAT_OK)
		return rc;
	rc = vec_matrix_mul(n, m, at, m, n, a, &ata);
	free(at);
	if (rc != VEC_MAT_OK)
		return rc;

	double *evals = NULL;
	double *vecs = NULL;
	rc = jacobi_eigen_sym(ata, n, &evals, &vecs);
	free(ata);
	if (rc != VEC_MAT_OK)
		return rc;

	struct sv_pair *ps = malloc(sizeof(ps[0]) * (size_t)n);
	if (!ps) {
		free(evals);
		free(vecs);
		return VEC_MAT_ERR_NOMEM;
	}
	for (int i = 0; i < n; i++) {
		double ev = evals[(size_t)i];
		if (ev < 0 && ev > -1e-9)
			ev = 0;
		if (ev < 0)
			ev = 0;
		ps[(size_t)i].s = sqrt(ev);
		ps[(size_t)i].i = i;
	}
	qsort(ps, (size_t)n, sizeof(ps[0]), sv_pair_cmp_desc);

	size_t nn;
	if (mul_size((size_t)n, (size_t)n, &nn) != 0) {
		free(evals);
		free(vecs);
		free(ps);
		return VEC_MAT_ERR_INVALID;
	}
	size_t mn;
	if (mul_size((size_t)m, (size_t)n, &mn) != 0) {
		free(evals);
		free(vecs);
		free(ps);
		return VEC_MAT_ERR_INVALID;
	}

	double *v = malloc(sizeof(v[0]) * nn);
	double *s = malloc(sizeof(s[0]) * (size_t)n);
	if (!v || !s) {
		free(evals);
		free(vecs);
		free(ps);
		free(v);
		free(s);
		return VEC_MAT_ERR_NOMEM;
	}

	for (int col = 0; col < n; col++) {
		int src = ps[(size_t)col].i;
		s[(size_t)col] = ps[(size_t)col].s;
		for (int r = 0; r < n; r++)
			v[(size_t)r * (size_t)n + (size_t)col] = vecs[(size_t)r * (size_t)n + (size_t)src];
	}

	/* U = A*V*diag(1/s). */
	double *av = NULL;
	rc = vec_matrix_mul(m, n, a, n, n, v, &av);
	if (rc != VEC_MAT_OK) {
		free(evals);
		free(vecs);
		free(ps);
		free(v);
		free(s);
		return rc;
	}

	double *u = calloc(mn, sizeof(u[0]));
	if (!u) {
		free(evals);
		free(vecs);
		free(ps);
		free(v);
		free(s);
		free(av);
		return VEC_MAT_ERR_NOMEM;
	}

	for (int col = 0; col < n; col++) {
		double sigma = s[(size_t)col];
		if (sigma == 0)
			continue;
		double inv = 1 / sigma;
		for (int r = 0; r < m; r++)
			u[(size_t)r * (size_t)n + (size_t)col] = av[(size_t)r * (size_t)n + (size_t)col] * inv;
	}
	free(av);

	/* Re-orthonormalize U via QR to improve stability. */
	double *qq = NULL;
	double *rr = NULL;
	rc = vec_qr_decompose(m, n, u, &qq, &rr);
	if (rc == VEC_MAT_OK) {
		free(u);
		u = qq;
		free(rr);
	}

	free(evals);
	free(vecs);
	free(ps);

	*out_u = u;
	*out_s = s;
	*out_v = v;
	return VEC_MAT_OK;
}
