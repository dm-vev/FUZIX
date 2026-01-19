#include "vec_value.h"

#include <stdlib.h>

vec_value vec_value_number(vec_number n)
{
	vec_value v;
	v.kind = VEC_VALUE_NUMBER;
	v.num = n;
	v.expr = NULL;
	v.arr = NULL;
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
	v.mat = NULL;
	v.rows = 0;
	v.cols = 0;
	v.c.re = re;
	v.c.im = im;
	return v;
}

void vec_value_destroy(vec_value *v)
{
	if (!v)
		return;
	free(v->arr);
	free(v->mat);
	v->arr = NULL;
	v->mat = NULL;
	v->expr = NULL;
	v->rows = 0;
	v->cols = 0;
}

