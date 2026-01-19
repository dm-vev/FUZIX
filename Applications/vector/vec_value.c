#include "vec_value.h"

#include "vec_ast.h"

#include <stdlib.h>
#include <string.h>

vec_value vec_value_number(vec_number n)
{
	vec_value v;
	v.kind = VEC_VALUE_NUMBER;
	v.num = n;
	v.expr = NULL;
	v.arr = NULL;
	v.len = 0;
	v.mat = NULL;
	v.rows = 0;
	v.cols = 0;
	v.c.re = 0;
	v.c.im = 0;
	return v;
}

vec_value vec_value_complex(double re, double im)
{
	vec_value v;
	v.kind = VEC_VALUE_COMPLEX;
	v.num = vec_float(0);
	v.expr = NULL;
	v.arr = NULL;
	v.len = 0;
	v.mat = NULL;
	v.rows = 0;
	v.cols = 0;
	v.c.re = re;
	v.c.im = im;
	return v;
}

vec_value vec_value_expr(struct vec_node *expr)
{
	vec_value v;
	memset(&v, 0, sizeof(v));
	v.kind = VEC_VALUE_EXPR;
	v.expr = expr;
	return v;
}

vec_value vec_value_array(double *arr, size_t len)
{
	vec_value v;
	memset(&v, 0, sizeof(v));
	v.kind = VEC_VALUE_ARRAY;
	v.arr = arr;
	v.len = len;
	return v;
}

vec_value vec_value_matrix(int rows, int cols, double *mat)
{
	vec_value v;
	memset(&v, 0, sizeof(v));
	v.kind = VEC_VALUE_MATRIX;
	v.mat = mat;
	v.rows = rows;
	v.cols = cols;
	return v;
}

int vec_value_clone(vec_value *dst, const vec_value *src)
{
	if (!dst || !src)
		return -1;

	memset(dst, 0, sizeof(*dst));
	dst->kind = src->kind;
	dst->num = src->num;
	dst->c = src->c;
	dst->rows = src->rows;
	dst->cols = src->cols;
	dst->len = src->len;

	switch (src->kind) {
	case VEC_VALUE_NUMBER:
	case VEC_VALUE_COMPLEX:
		return 0;
	case VEC_VALUE_EXPR:
		dst->expr = src->expr ? vec_node_clone(src->expr) : NULL;
		return src->expr && !dst->expr ? -1 : 0;
	case VEC_VALUE_ARRAY:
		if (!src->arr || src->len == 0) {
			dst->arr = NULL;
			dst->len = 0;
			return 0;
		}
		dst->arr = malloc(sizeof(dst->arr[0]) * src->len);
		if (!dst->arr)
			return -1;
		memcpy(dst->arr, src->arr, sizeof(dst->arr[0]) * src->len);
		return 0;
	case VEC_VALUE_MATRIX: {
		size_t n = 0;
		if (src->rows > 0 && src->cols > 0)
			n = (size_t)src->rows * (size_t)src->cols;
		if (!src->mat || n == 0) {
			dst->mat = NULL;
			dst->rows = 0;
			dst->cols = 0;
			return 0;
		}
		dst->mat = malloc(sizeof(dst->mat[0]) * n);
		if (!dst->mat)
			return -1;
		memcpy(dst->mat, src->mat, sizeof(dst->mat[0]) * n);
		return 0;
	}
	default:
		return -1;
	}
}

void vec_value_destroy(vec_value *v)
{
	if (!v)
		return;
	if (v->kind == VEC_VALUE_EXPR)
		vec_node_destroy(v->expr);
	free(v->arr);
	free(v->mat);
	v->arr = NULL;
	v->mat = NULL;
	v->expr = NULL;
	v->len = 0;
	v->rows = 0;
	v->cols = 0;
}
