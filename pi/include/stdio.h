/* pc486 bare-metal libc shim: stdio on top of FatFs */
#ifndef PC486_STDIO_H
#define PC486_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct __pc486_file FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 512
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int fseek(FILE *f, long off, int whence);
int fseeko(FILE *f, off_t off, int whence);
long ftell(FILE *f);
off_t ftello(FILE *f);
void rewind(FILE *f);
int fflush(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
char *fgets(char *s, int n, FILE *f);
int fgetc(FILE *f);
#define getc fgetc
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int putchar(int c);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
void perror(const char *s);
#ifdef __cplusplus
}
#endif
#endif
