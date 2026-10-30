/* pc486 bare-metal math: builtins where AArch64 has instructions,
 * openlibm for the rest */
#ifndef PC486_MATH_H
#define PC486_MATH_H
#ifdef __cplusplus
extern "C" {
#endif
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define M_PI 3.14159265358979323846
#define isnan(x) __builtin_isnan(x)
#define isunordered(x, y) __builtin_isunordered(x, y)
#define isgreater(x, y) __builtin_isgreater(x, y)
#define isless(x, y) __builtin_isless(x, y)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define floor(x) __builtin_floor(x)
#define ceil(x) __builtin_ceil(x)
#define trunc(x) __builtin_trunc(x)
#define round(x) __builtin_round(x)
#define rint(x) __builtin_rint(x)
#define nearbyint(x) __builtin_nearbyint(x)
#define nearbyintf(x) __builtin_nearbyintf(x)
#define copysign(x, y) __builtin_copysign(x, y)
double sin(double);
double cos(double);
double tan(double);
double atan(double);
double atan2(double, double);
double exp(double);
double log(double);
double log2(double);
double log10(double);
double pow(double, double);
double frexp(double, int *);
double ldexp(double, int);
double scalbn(double, int);
double fmod(double, double);
#ifdef __cplusplus
}
#endif
#endif
