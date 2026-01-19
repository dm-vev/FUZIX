#ifndef VEC_NUMBER_H
#define VEC_NUMBER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	VEC_NUM_FLOAT = 0,
	VEC_NUM_RAT = 1,
} vec_number_kind;

typedef enum {
	VEC_NUM_OK = 0,
	VEC_NUM_DIV0 = 1,
	VEC_NUM_OVERFLOW = 2,
} vec_num_err;

typedef struct {
	int64_t num;
	int64_t den;
} vec_rat;

typedef struct {
	vec_number_kind kind;
	double f;
	vec_rat r;
} vec_number;

vec_number vec_float(double f);
vec_number vec_rat_number(vec_rat r);

vec_rat vec_rat_int(int64_t n);
vec_num_err vec_rat_new(vec_rat *out, int64_t num, int64_t den);

double vec_rat_float64(vec_rat r);
vec_num_err vec_rat_add(vec_rat a, vec_rat b, vec_rat *out);
vec_num_err vec_rat_sub(vec_rat a, vec_rat b, vec_rat *out);
vec_num_err vec_rat_mul(vec_rat a, vec_rat b, vec_rat *out);
vec_num_err vec_rat_div(vec_rat a, vec_rat b, vec_rat *out);
vec_num_err vec_rat_pow_int(vec_rat a, int64_t exp, vec_rat *out);

double vec_number_float64(vec_number n);
char *vec_number_string(vec_number n, int prec, char *buf, size_t bufsz);

#endif

