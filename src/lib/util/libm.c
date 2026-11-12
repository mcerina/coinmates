// license:BSD-3-Clause
/***************************************************************************

    libm.c

    The C library's elementary functions, correctly rounded by CORE-MATH
    (3rdparty/core-math). The C libraries of Linux, Windows and macOS
    differ in the last bit, and netplay needs the same machine state on
    every system; a correctly rounded result is the same everywhere.

***************************************************************************/

double cr_acos(double);
double cr_asin(double);
double cr_atan(double);
double cr_atan2(double, double);
double cr_cbrt(double);
double cr_cos(double);
double cr_cosh(double);
double cr_exp(double);
double cr_exp2(double);
double cr_expm1(double);
double cr_hypot(double, double);
double cr_log(double);
double cr_log10(double);
double cr_log1p(double);
double cr_log2(double);
double cr_pow(double, double);
double cr_sin(double);
double cr_sinh(double);
double cr_tan(double);
double cr_tanh(double);
void cr_sincos(double, double *, double *);

float cr_acosf(float);
float cr_asinf(float);
float cr_atanf(float);
float cr_atan2f(float, float);
float cr_cbrtf(float);
float cr_cosf(float);
float cr_coshf(float);
float cr_expf(float);
float cr_exp2f(float);
float cr_expm1f(float);
float cr_hypotf(float, float);
float cr_logf(float);
float cr_log10f(float);
float cr_log1pf(float);
float cr_log2f(float);
float cr_powf(float, float);
float cr_sinf(float);
float cr_sinhf(float);
float cr_tanf(float);
float cr_tanhf(float);

double acos(double x) { return cr_acos(x); }
double asin(double x) { return cr_asin(x); }
double atan(double x) { return cr_atan(x); }
double atan2(double y, double x) { return cr_atan2(y, x); }
double cbrt(double x) { return cr_cbrt(x); }
double cos(double x) { return cr_cos(x); }
double cosh(double x) { return cr_cosh(x); }
double exp(double x) { return cr_exp(x); }
double exp2(double x) { return cr_exp2(x); }
double expm1(double x) { return cr_expm1(x); }
double hypot(double x, double y) { return cr_hypot(x, y); }
double log(double x) { return cr_log(x); }
double log10(double x) { return cr_log10(x); }
double log1p(double x) { return cr_log1p(x); }
double log2(double x) { return cr_log2(x); }
double pow(double x, double y) { return cr_pow(x, y); }
double sin(double x) { return cr_sin(x); }
double sinh(double x) { return cr_sinh(x); }
double tan(double x) { return cr_tan(x); }
double tanh(double x) { return cr_tanh(x); }

float acosf(float x) { return cr_acosf(x); }
float asinf(float x) { return cr_asinf(x); }
float atanf(float x) { return cr_atanf(x); }
float atan2f(float y, float x) { return cr_atan2f(y, x); }
float cbrtf(float x) { return cr_cbrtf(x); }
float cosf(float x) { return cr_cosf(x); }
float coshf(float x) { return cr_coshf(x); }
float expf(float x) { return cr_expf(x); }
float exp2f(float x) { return cr_exp2f(x); }
float expm1f(float x) { return cr_expm1f(x); }
float hypotf(float x, float y) { return cr_hypotf(x, y); }
float logf(float x) { return cr_logf(x); }
float log10f(float x) { return cr_log10f(x); }
float log1pf(float x) { return cr_log1pf(x); }
float log2f(float x) { return cr_log2f(x); }
float powf(float x, float y) { return cr_powf(x, y); }
float sinf(float x) { return cr_sinf(x); }
float sinhf(float x) { return cr_sinhf(x); }
float tanf(float x) { return cr_tanf(x); }
float tanhf(float x) { return cr_tanhf(x); }

// what gcc makes of a sine and a cosine of the same angle
void sincos(double x, double *s, double *c) { cr_sincos(x, s, c); }
void sincosf(float x, float *s, float *c) { *s = cr_sinf(x); *c = cr_cosf(x); }

// CORE-MATH's __builtin_roundeven without SSE4.1; Windows' C library doesn't have it
double roundeven(double x)
{
	double r = __builtin_round(x);
	return __builtin_fabs(r - x) == 0.5 ? 2.0 * __builtin_round(x * 0.5) : r;
}
