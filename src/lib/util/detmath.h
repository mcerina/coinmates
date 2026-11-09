// license:BSD-3-Clause
/***************************************************************************

    detmath.h

    Elementary functions that give the same bits on every system.

    The C library's exp, log, sin... differ in the last bit between
    systems (glibc, Windows' UCRT, Apple's libm). Where their results
    end up in the machine state, players of a netplay session on
    different systems drift apart. These use only operations that
    IEEE 754 rounds exactly (+ - * /, sqrt, floor, frexp, ldexp), so
    built without fused multiply-add (-ffp-contract=off) they agree
    everywhere. They are accurate to a few units in the last place.

***************************************************************************/
#ifndef MAME_LIB_UTIL_DETMATH_H
#define MAME_LIB_UTIL_DETMATH_H

#pragma once

#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>


namespace util::detmath {

namespace detail {

// ln 2 and pi/2 in two parts: the high ones have trailing zero bits, so
// that k * high is exact for the k of a range reduction
constexpr double LN2_HI = 6.93147180369123816490e-01;
constexpr double LN2_LO = 1.90821492927058770002e-10;
constexpr double INV_LN2 = 1.44269504088896338700e+00;
constexpr double PIO2_HI = 1.57079632673412561417e+00;
constexpr double PIO2_LO = 6.07710050650619224932e-11;
constexpr double INV_PIO2 = 6.36619772367581382433e-01;

// x - k * pi/2, for |x| up to about 1e9 (beyond it, still the same bits everywhere)
inline double reduce_pio2(double x, double &k)
{
	k = std::floor(x * INV_PIO2 + 0.5);
	return (x - k * PIO2_HI) - k * PIO2_LO;
}

// c[0] x^(n-1) + c[1] x^(n-2) + ... + c[n-1], by Horner's rule
inline double poly(const double *c, size_t n, double x)
{
	double p = c[0];
	for (size_t i = 1; i < n; i++)
		p = p * x + c[i];
	return p;
}

// 1/20!, 1/19!, ..., 1/1!, 1/0! (the denominators are exact doubles: so are these, everywhere)
constexpr double INV_FACT[] = {
		1.0 / 2432902008176640000.0, 1.0 / 121645100408832000.0, 1.0 / 6402373705728000.0, 1.0 / 355687428096000.0,
		1.0 / 20922789888000.0, 1.0 / 1307674368000.0, 1.0 / 87178291200.0, 1.0 / 6227020800.0, 1.0 / 479001600.0,
		1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0, 1.0 / 40320.0, 1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0,
		1.0 / 24.0, 1.0 / 6.0, 1.0 / 2.0, 1.0, 1.0 };

// sine and cosine of |r| <= pi/4, Taylor series to below the last bit
inline double sin_small(double r)
{
	const double r2 = r * r;
	double p = -1.0 / 1307674368000.0;                  // -1/15!
	p = p * r2 + 1.0 / 6227020800.0;
	p = p * r2 - 1.0 / 39916800.0;
	p = p * r2 + 1.0 / 362880.0;
	p = p * r2 - 1.0 / 5040.0;
	p = p * r2 + 1.0 / 120.0;
	p = p * r2 - 1.0 / 6.0;
	return r + r * r2 * p;
}

inline double cos_small(double r)
{
	const double r2 = r * r;
	double p = 1.0 / 20922789888000.0;                  // 1/16!
	p = p * r2 - 1.0 / 87178291200.0;
	p = p * r2 + 1.0 / 479001600.0;
	p = p * r2 - 1.0 / 3628800.0;
	p = p * r2 + 1.0 / 40320.0;
	p = p * r2 - 1.0 / 720.0;
	p = p * r2 + 1.0 / 24.0;
	return 1.0 - 0.5 * r2 + r2 * r2 * p;
}

} // namespace detail


inline double exp(double x)
{
	if (std::isnan(x))
		return x;
	if (x > 709.782712893384)
		return std::numeric_limits<double>::infinity();
	if (x < -745.1332191019412)
		return 0.0;
	// x = k ln2 + r, |r| <= ln2/2: exp(x) = 2^k exp(r), exp(r) by its Taylor series
	const double k = std::floor(x * detail::INV_LN2 + 0.5);
	const double r = (x - k * detail::LN2_HI) - k * detail::LN2_LO;
	const double p = detail::poly(detail::INV_FACT + 7, 14, r);   // r^13/13! + ... + r + 1
	return std::ldexp(p, int(k));
}

inline double log(double x)
{
	if (std::isnan(x) || x == std::numeric_limits<double>::infinity())
		return x;
	if (x < 0)
		return std::numeric_limits<double>::quiet_NaN();
	if (x == 0)
		return -std::numeric_limits<double>::infinity();
	// x = 2^e m, sqrt(1/2) <= m < sqrt(2): log(m) = 2 atanh(s), s = (m - 1) / (m + 1)
	int e;
	double m = std::frexp(x, &e);
	if (m < 0.70710678118654752440)
	{
		m *= 2;
		e--;
	}
	const double s = (m - 1) / (m + 1);
	static constexpr double c[] = { 1.0 / 25, 1.0 / 23, 1.0 / 21, 1.0 / 19, 1.0 / 17, 1.0 / 15, 1.0 / 13, 1.0 / 11, 1.0 / 9, 1.0 / 7, 1.0 / 5, 1.0 / 3 };
	const double s2 = s * s;
	const double logm = 2 * s + 2 * s * s2 * detail::poly(c, std::size(c), s2);
	return e * detail::LN2_HI + (logm + e * detail::LN2_LO);
}

// log(1 + x), also for x much smaller than 1
inline double log1p(double x)
{
	const double u = 1 + x;
	if (u == 1)
		return x;
	return log(u) - ((u - 1) - x) / u;
}

// exp(x) - 1, also for x much smaller than 1
inline double expm1(double x)
{
	if (std::fabs(x) >= 1)
		return exp(x) - 1;
	return x * detail::poly(detail::INV_FACT, 20, x);       // x (x^19/20! + ... + x/2! + 1)
}

inline double tanh(double x)
{
	if (std::fabs(x) > 22)
		return x > 0 ? 1.0 : -1.0;
	const double e = expm1(2 * x);
	return e / (e + 2);
}

inline double sin(double x)
{
	if (!std::isfinite(x))
		return std::numeric_limits<double>::quiet_NaN();
	double k;
	const double r = detail::reduce_pio2(x, k);
	switch (int(k - 4 * std::floor(k / 4)))
	{
	case 0: return detail::sin_small(r);
	case 1: return detail::cos_small(r);
	case 2: return -detail::sin_small(r);
	default: return -detail::cos_small(r);
	}
}

inline double cos(double x)
{
	if (!std::isfinite(x))
		return std::numeric_limits<double>::quiet_NaN();
	double k;
	const double r = detail::reduce_pio2(x, k);
	switch (int(k - 4 * std::floor(k / 4)))
	{
	case 0: return detail::cos_small(r);
	case 1: return -detail::sin_small(r);
	case 2: return -detail::cos_small(r);
	default: return detail::sin_small(r);
	}
}

inline double tan(double x)
{
	return sin(x) / cos(x);
}

inline double hypot(double x, double y)
{
	return std::sqrt(x * x + y * y);
}

// x^y: by multiplications for small whole powers, else exp(y log x)
inline double pow(double x, double y)
{
	if (y == std::floor(y) && std::fabs(y) <= 64)
	{
		double r = 1, b = x;
		for (unsigned n = unsigned(std::fabs(y)); n; n >>= 1, b *= b)
			if (n & 1)
				r *= b;
		return y < 0 ? 1 / r : r;
	}
	if (x < 0)
		return y == std::floor(y) ? (std::fmod(y, 2) != 0 ? -1 : 1) * exp(y * log(-x)) : std::numeric_limits<double>::quiet_NaN();
	if (x == 0)
		return y > 0 ? 0.0 : std::numeric_limits<double>::infinity();
	return exp(y * log(x));
}

} // namespace util::detmath

#endif // MAME_LIB_UTIL_DETMATH_H
