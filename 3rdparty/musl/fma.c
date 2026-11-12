/* fma and fmaf, exact on every system, which CORE-MATH calls on x86-64: the CPU's instruction where
   there is one, as the C libraries do, else musl's (src/math/fma.c, src/math/fmaf.c). Both exact,
   so the same bits. musl's code is unchanged but for its names, a_clz_64 as __builtin_clzll and no
   FENV_ACCESS pragma: it isn't needed, the rounding is always to nearest.

   musl: Copyright © 2005-2020 Rich Felker, et al. MIT license: see COPYRIGHT. */

#pragma GCC diagnostic ignored "-Wparentheses"

#include <stdint.h>
#include <float.h>
#include <math.h>

#define ASUINT64(x) ((union {double f; uint64_t i;}){x}).i
#define ZEROINFNAN (0x7ff-0x3ff-52-1)

struct num { uint64_t m; int e; int sign; };

static struct num normalize(double x)
{
	uint64_t ix = ASUINT64(x);
	int e = ix>>52;
	int sign = e & 0x800;
	e &= 0x7ff;
	if (!e) {
		ix = ASUINT64(x*0x1p63);
		e = ix>>52 & 0x7ff;
		e = e ? e-63 : 0x800;
	}
	ix &= (1ull<<52)-1;
	ix |= 1ull<<52;
	ix <<= 1;
	e -= 0x3ff + 52 + 1;
	return (struct num){ix,e,sign};
}

static void mul(uint64_t *hi, uint64_t *lo, uint64_t x, uint64_t y)
{
	uint64_t t1,t2,t3;
	uint64_t xlo = (uint32_t)x, xhi = x>>32;
	uint64_t ylo = (uint32_t)y, yhi = y>>32;

	t1 = xlo*ylo;
	t2 = xlo*yhi + xhi*ylo;
	t3 = xhi*yhi;
	*lo = t1 + (t2<<32);
	*hi = t3 + (t2>>32) + (t1 > *lo);
}

static double musl_fma(double x, double y, double z)
{
	/* normalize so top 10bits and last bit are 0 */
	struct num nx, ny, nz;
	nx = normalize(x);
	ny = normalize(y);
	nz = normalize(z);

	if (nx.e >= ZEROINFNAN || ny.e >= ZEROINFNAN)
		return x*y + z;
	if (nz.e >= ZEROINFNAN) {
		if (nz.e > ZEROINFNAN) /* z==0 */
			return x*y;
		return z;
	}

	/* mul: r = x*y */
	uint64_t rhi, rlo, zhi, zlo;
	mul(&rhi, &rlo, nx.m, ny.m);
	/* either top 20 or 21 bits of rhi and last 2 bits of rlo are 0 */

	/* align exponents */
	int e = nx.e + ny.e;
	int d = nz.e - e;
	/* shift bits z<<=kz, r>>=kr, so kz+kr == d, set e = e+kr (== ez-kz) */
	if (d > 0) {
		if (d < 64) {
			zlo = nz.m<<d;
			zhi = nz.m>>64-d;
		} else {
			zlo = 0;
			zhi = nz.m;
			e = nz.e - 64;
			d -= 64;
			if (d == 0) {
			} else if (d < 64) {
				rlo = rhi<<64-d | rlo>>d | !!(rlo<<64-d);
				rhi = rhi>>d;
			} else {
				rlo = 1;
				rhi = 0;
			}
		}
	} else {
		zhi = 0;
		d = -d;
		if (d == 0) {
			zlo = nz.m;
		} else if (d < 64) {
			zlo = nz.m>>d | !!(nz.m<<64-d);
		} else {
			zlo = 1;
		}
	}

	/* add */
	int sign = nx.sign^ny.sign;
	int samesign = !(sign^nz.sign);
	int nonzero = 1;
	if (samesign) {
		/* r += z */
		rlo += zlo;
		rhi += zhi + (rlo < zlo);
	} else {
		/* r -= z */
		uint64_t t = rlo;
		rlo -= zlo;
		rhi = rhi - zhi - (t < rlo);
		if (rhi>>63) {
			rlo = -rlo;
			rhi = -rhi-!!rlo;
			sign = !sign;
		}
		nonzero = !!rhi;
	}

	/* set rhi to top 63bit of the result (last bit is sticky) */
	if (nonzero) {
		e += 64;
		d = __builtin_clzll(rhi)-1;
		/* note: d > 0 */
		rhi = rhi<<d | rlo>>64-d | !!(rlo<<d);
	} else if (rlo) {
		d = __builtin_clzll(rlo)-1;
		if (d < 0)
			rhi = rlo>>1 | (rlo&1);
		else
			rhi = rlo<<d;
	} else {
		/* exact +-0 */
		return x*y + z;
	}
	e -= d;

	/* convert to double */
	int64_t i = rhi; /* i is in [1<<62,(1<<63)-1] */
	if (sign)
		i = -i;
	double r = i; /* |r| is in [0x1p62,0x1p63] */

	if (e < -1022-62) {
		/* result is subnormal before rounding */
		if (e == -1022-63) {
			double c = 0x1p63;
			if (sign)
				c = -c;
			if (r == c) {
				/* min normal after rounding, underflow depends
				   on arch behaviour which can be imitated by
				   a double to float conversion */
				float fltmin = 0x0.ffffff8p-63*FLT_MIN * r;
				return DBL_MIN/FLT_MIN * fltmin;
			}
			/* one bit is lost when scaled, add another top bit to
			   only round once at conversion if it is inexact */
			if (rhi << 53) {
				i = rhi>>1 | (rhi&1) | 1ull<<62;
				if (sign)
					i = -i;
				r = i;
				r = 2*r - c; /* remove top bit */

				/* raise underflow portably, such that it
				   cannot be optimized away */
				{
					double_t tiny = DBL_MIN/FLT_MIN * r;
					r += (double)(tiny*tiny) * (r-r);
				}
			}
		} else {
			/* only round once when scaled */
			d = 10;
			i = ( rhi>>d | !!(rhi<<64-d) ) << d;
			if (sign)
				i = -i;
			r = i;
		}
	}
	return scalbn(r, e);
}


static float musl_fmaf(float x, float y, float z)
{
	double xy = (double)x * y;
	union {double r; uint64_t i;} u = {xy + z};
	int e = u.i>>52 & 0x7ff;
	/* covers |r| > 0x1p-126 halfway cases (may round incorrectly) */
	int halfway = (u.i & 0x1fffffff) == 0x10000000;
	/* covers tiny inexact (may miss uflow) and tiny halfway cases */
	int tiny = e <= 0x3ff-126 && e >= 0x3ff-149;
	if (!halfway && !tiny)
		/* common case, optimization only */
		return (float)u.r;
	if (e != 0x7ff) {
		/* r+t == x*y+z exactly in nearest rounding, otherwise
		 * rounding does not affect t!=0, t<0 and inexact flag. */
		int s = u.i >> 63;
		double t = s == (xy < z) ? xy - u.r + z : z - u.r + xy;
		if (t) {
			/* adjust r toward r+t if r%2==0 (round to odd).
			 * may be needed if r is halfway or exact float,
			 * wrong if r becomes halfway or exact float. */
			u.i -= s ^ (t<0);
			u.i |= 1;
		}
	}
	return (float)u.r;
}

#if defined(__x86_64__)
__attribute__((target("fma"))) static double cpu_fma(double x, double y, double z) { return __builtin_fma(x, y, z); }
__attribute__((target("fma"))) static float cpu_fmaf(float x, float y, float z) { return __builtin_fmaf(x, y, z); }
#endif

double fma(double x, double y, double z)
{
#if defined(__x86_64__)
	if (__builtin_cpu_supports("fma"))
		return cpu_fma(x, y, z);
#endif
	return musl_fma(x, y, z);
}

float fmaf(float x, float y, float z)
{
#if defined(__x86_64__)
	if (__builtin_cpu_supports("fma"))
		return cpu_fmaf(x, y, z);
#endif
	return musl_fmaf(x, y, z);
}
