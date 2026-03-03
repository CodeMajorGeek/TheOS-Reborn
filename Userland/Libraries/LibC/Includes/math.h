#ifndef _MATH_H
#define _MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define HUGE_VAL  (__builtin_huge_val())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#ifndef M_E
#define M_E       2.7182818284590452354
#endif
#ifndef M_PI
#define M_PI      3.14159265358979323846
#endif

#define isnan(x)     __builtin_isnan((x))
#define isinf(x)     __builtin_isinf_sign((x))
#define isfinite(x)  __builtin_isfinite((x))
#define signbit(x)   __builtin_signbit((x))

double fabs(double x);
float fabsf(float x);
long double fabsl(long double x);

double fmod(double x, double y);
float fmodf(float x, float y);
long double fmodl(long double x, long double y);

double floor(double x);
float floorf(float x);
long double floorl(long double x);

double ceil(double x);
float ceilf(float x);
long double ceill(long double x);

double trunc(double x);
float truncf(float x);
long double truncl(long double x);

double round(double x);
float roundf(float x);
long double roundl(long double x);

double sqrt(double x);
float sqrtf(float x);
long double sqrtl(long double x);

double pow(double x, double y);
float powf(float x, float y);
long double powl(long double x, long double y);

double exp(double x);
float expf(float x);
long double expl(long double x);

double expm1(double x);
float expm1f(float x);
long double expm1l(long double x);

double log(double x);
float logf(float x);
long double logl(long double x);

double log2(double x);
float log2f(float x);
long double log2l(long double x);

double log10(double x);
float log10f(float x);
long double log10l(long double x);

double cos(double x);
float cosf(float x);
long double cosl(long double x);

double sin(double x);
float sinf(float x);
long double sinl(long double x);

double tan(double x);
float tanf(float x);
long double tanl(long double x);

double acos(double x);
float acosf(float x);
long double acosl(long double x);

double asin(double x);
float asinf(float x);
long double asinl(long double x);

double atan(double x);
float atanf(float x);
long double atanl(long double x);

double atan2(double y, double x);
float atan2f(float y, float x);
long double atan2l(long double y, long double x);

double cosh(double x);
float coshf(float x);
long double coshl(long double x);

double sinh(double x);
float sinhf(float x);
long double sinhl(long double x);

double tanh(double x);
float tanhf(float x);
long double tanhl(long double x);

double acosh(double x);
float acoshf(float x);
long double acoshl(long double x);

double asinh(double x);
float asinhf(float x);
long double asinhl(long double x);

double atanh(double x);
float atanhf(float x);
long double atanhl(long double x);

double copysign(double x, double y);
float copysignf(float x, float y);
long double copysignl(long double x, long double y);

double ldexp(double x, int exp);
float ldexpf(float x, int exp);
long double ldexpl(long double x, int exp);

double frexp(double x, int* exp);
float frexpf(float x, int* exp);
long double frexpl(long double x, int* exp);

double modf(double x, double* iptr);
float modff(float x, float* iptr);
long double modfl(long double x, long double* iptr);

double erf(double x);
float erff(float x);
long double erfl(long double x);

double erfc(double x);
float erfcf(float x);
long double erfcl(long double x);

double tgamma(double x);
float tgammaf(float x);
long double tgammal(long double x);

double lgamma(double x);
float lgammaf(float x);
long double lgammal(long double x);

#ifdef __cplusplus
}
#endif

#endif
