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
double cr_cos(double);
double cr_exp(double);
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

float cr_acosf(float);
float cr_atan2f(float, float);
float cr_cosf(float);
float cr_expf(float);
float cr_log10f(float);
float cr_logf(float);
float cr_powf(float, float);
float cr_sinf(float);
float cr_tanf(float);

double acos(double x) { return cr_acos(x); }
double asin(double x) { return cr_asin(x); }
double atan(double x) { return cr_atan(x); }
double atan2(double y, double x) { return cr_atan2(y, x); }
double cos(double x) { return cr_cos(x); }
double exp(double x) { return cr_exp(x); }
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
float atan2f(float y, float x) { return cr_atan2f(y, x); }
float cosf(float x) { return cr_cosf(x); }
float expf(float x) { return cr_expf(x); }
float log10f(float x) { return cr_log10f(x); }
float logf(float x) { return cr_logf(x); }
float powf(float x, float y) { return cr_powf(x, y); }
float sinf(float x) { return cr_sinf(x); }
float tanf(float x) { return cr_tanf(x); }

// what gcc makes of a sine and a cosine of the same angle
void sincosf(float x, float *s, float *c) { *s = cr_sinf(x); *c = cr_cosf(x); }

// CORE-MATH's __builtin_roundeven without SSE4.1; Windows' C library doesn't have it
double roundeven(double x)
{
	double r = __builtin_round(x);
	return __builtin_fabs(r - x) == 0.5 ? 2.0 * __builtin_round(x * 0.5) : r;
}
