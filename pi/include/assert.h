#ifndef PC486_ASSERT_H
#define PC486_ASSERT_H
#ifdef __cplusplus
extern "C"
#endif
void __pc486_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __pc486_assert_fail(#x, __FILE__, __LINE__))
#endif
#endif
