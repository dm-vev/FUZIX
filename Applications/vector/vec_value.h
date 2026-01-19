#ifndef VEC_VALUE_H
#define VEC_VALUE_H

#include "vec_number.h"

#include <stddef.h>
#include <stdint.h>

struct vec_node;

typedef enum {
	VEC_VALUE_NUMBER = 0,
	VEC_VALUE_EXPR = 1,
	VEC_VALUE_ARRAY = 2,
	VEC_VALUE_MATRIX = 3,
	VEC_VALUE_COMPLEX = 4,
} vec_value_kind;

typedef struct {
	double re;
	double im;
} vec_complex;

typedef struct {
	vec_value_kind kind;
	vec_number num;
	struct vec_node *expr;
	double *arr;
	double *mat;
	int rows;
	int cols;
	vec_complex c;
} vec_value;

vec_value vec_value_number(vec_number n);
vec_value vec_value_complex(double re, double im);

void vec_value_destroy(vec_value *v);

#endif

