#include "vec_number.h"

#include <math.h>
#include <stdio.h>

static int64_t abs64(int64_t x) { return x < 0 ? -x : x; }

static int64_t gcd64(int64_t a, int64_t b)
{
	while (b) {
		int64_t t = a % b;
		a = b;
		b = t;
	}
	return a ? a : 1;
}

static vec_num_err add_checked(int64_t a, int64_t b, int64_t *out)
{
	if (__builtin_add_overflow(a, b, out))
		return VEC_NUM_OVERFLOW;
	return VEC_NUM_OK;
}

static vec_num_err sub_checked(int64_t a, int64_t b, int64_t *out)
{
	if (__builtin_sub_overflow(a, b, out))
		return VEC_NUM_OVERFLOW;
	return VEC_NUM_OK;
}

static vec_num_err mul_checked(int64_t a, int64_t b, int64_t *out)
{
	if (__builtin_mul_overflow(a, b, out))
		return VEC_NUM_OVERFLOW;
	return VEC_NUM_OK;
}

vec_number vec_float(double f)
{
	vec_number n;
	n.kind = VEC_NUM_FLOAT;
	n.f = f;
	n.r.num = 0;
	n.r.den = 1;
	return n;
}

vec_number vec_rat_number(vec_rat r)
{
	vec_number n;
	n.kind = VEC_NUM_RAT;
	n.f = 0;
	n.r = r;
	return n;
}

vec_rat vec_rat_int(int64_t n)
{
	vec_rat r;
	r.num = n;
	r.den = 1;
	return r;
}

vec_num_err vec_rat_new(vec_rat *out, int64_t num, int64_t den)
{
	if (!out)
		return VEC_NUM_OVERFLOW;
	if (den == 0)
		return VEC_NUM_DIV0;
	if (den < 0) {
		num = -num;
		den = -den;
	}
	int64_t g = gcd64(abs64(num), den);
	out->num = num / g;
	out->den = den / g;
	return VEC_NUM_OK;
}

double vec_rat_float64(vec_rat r)
{
	return (double)r.num / (double)r.den;
}

vec_num_err vec_rat_add(vec_rat a, vec_rat b, vec_rat *out)
{
	int64_t n1, n2, n, d;
	vec_num_err err;

	if ((err = mul_checked(a.num, b.den, &n1)) != VEC_NUM_OK)
		return err;
	if ((err = mul_checked(b.num, a.den, &n2)) != VEC_NUM_OK)
		return err;
	if ((err = add_checked(n1, n2, &n)) != VEC_NUM_OK)
		return err;
	if ((err = mul_checked(a.den, b.den, &d)) != VEC_NUM_OK)
		return err;
	return vec_rat_new(out, n, d);
}

vec_num_err vec_rat_sub(vec_rat a, vec_rat b, vec_rat *out)
{
	int64_t n1, n2, n, d;
	vec_num_err err;

	if ((err = mul_checked(a.num, b.den, &n1)) != VEC_NUM_OK)
		return err;
	if ((err = mul_checked(b.num, a.den, &n2)) != VEC_NUM_OK)
		return err;
	if ((err = sub_checked(n1, n2, &n)) != VEC_NUM_OK)
		return err;
	if ((err = mul_checked(a.den, b.den, &d)) != VEC_NUM_OK)
		return err;
	return vec_rat_new(out, n, d);
}

vec_num_err vec_rat_mul(vec_rat a, vec_rat b, vec_rat *out)
{
	int64_t n, d;
	vec_num_err err;

	if ((err = mul_checked(a.num, b.num, &n)) != VEC_NUM_OK)
		return err;
	if ((err = mul_checked(a.den, b.den, &d)) != VEC_NUM_OK)
		return err;
	return vec_rat_new(out, n, d);
}

vec_num_err vec_rat_div(vec_rat a, vec_rat b, vec_rat *out)
{
	int64_t n, d, bnum_abs;
	vec_num_err err;

	if (b.num == 0)
		return VEC_NUM_DIV0;
	if ((err = mul_checked(a.num, b.den, &n)) != VEC_NUM_OK)
		return err;
	bnum_abs = abs64(b.num);
	if ((err = mul_checked(a.den, bnum_abs, &d)) != VEC_NUM_OK)
		return err;
	if (b.num < 0)
		n = -n;
	return vec_rat_new(out, n, d);
}

vec_num_err vec_rat_pow_int(vec_rat a, int64_t exp, vec_rat *out)
{
	vec_num_err err;
	if (exp == 0) {
		*out = vec_rat_int(1);
		return VEC_NUM_OK;
	}
	if (exp < 0) {
		vec_rat inv;
		if (a.num == 0)
			return VEC_NUM_DIV0;
		if ((err = vec_rat_new(&inv, a.den, a.num)) != VEC_NUM_OK)
			return err;
		return vec_rat_pow_int(inv, -exp, out);
	}

	vec_rat base = a;
	vec_rat acc = vec_rat_int(1);
	while (exp > 0) {
		if (exp & 1) {
			vec_rat tmp;
			err = vec_rat_mul(acc, base, &tmp);
			if (err != VEC_NUM_OK)
				return err;
			acc = tmp;
		}
		exp >>= 1;
		if (!exp)
			break;
		{
			vec_rat tmp;
			err = vec_rat_mul(base, base, &tmp);
			if (err != VEC_NUM_OK)
				return err;
			base = tmp;
		}
	}
	*out = acc;
	return VEC_NUM_OK;
}

double vec_number_float64(vec_number n)
{
	if (n.kind == VEC_NUM_RAT)
		return vec_rat_float64(n.r);
	return n.f;
}

static char *format_float(double f, int prec, char *buf, size_t bufsz)
{
	if (!buf || !bufsz)
		return buf;
	if (isnan(f)) {
		snprintf(buf, bufsz, "NaN");
		return buf;
	}
	if (isinf(f)) {
		snprintf(buf, bufsz, "%sInf", f > 0 ? "+" : "-");
		return buf;
	}
	if (prec <= 0)
		prec = 10;
	snprintf(buf, bufsz, "%.*g", prec, f);
	return buf;
}

char *vec_number_string(vec_number n, int prec, char *buf, size_t bufsz)
{
	if (!buf || !bufsz)
		return buf;
	if (n.kind == VEC_NUM_RAT) {
		if (n.r.den == 1) {
			snprintf(buf, bufsz, "%lld", (long long)n.r.num);
			return buf;
		}
		snprintf(buf, bufsz, "%lld/%lld", (long long)n.r.num, (long long)n.r.den);
		return buf;
	}
	return format_float(n.f, prec, buf, bufsz);
}

