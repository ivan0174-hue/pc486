/*
 * pc486 - minimal C library for the Circle bare-metal build
 * (stdio on FatFs, printf family, misc stdlib/string helpers)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fatfs/ff.h>

/* provided by the C++ kernel */
void pc486_console_write(const char *s, int len);
void pc486_fatal(const char *msg) __attribute__((noreturn));

int errno;

/* ------------------------------------------------------------------ */
/* FILE                                                               */

struct __pc486_file {
    int console;
    int eof, err;
    FIL fil;
};

static struct __pc486_file s_stdin = { 1 }, s_stdout = { 1 }, s_stderr = { 1 };
FILE *stdin = &s_stdin, *stdout = &s_stdout, *stderr = &s_stderr;

FILE *fopen(const char *path, const char *mode)
{
    BYTE m = 0;
    bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': m = FA_READ | (plus ? FA_WRITE : 0) | FA_OPEN_EXISTING; break;
    case 'w': m = FA_WRITE | (plus ? FA_READ : 0) | FA_CREATE_ALWAYS; break;
    case 'a': m = FA_WRITE | (plus ? FA_READ : 0) | FA_OPEN_APPEND; break;
    default: errno = 22; return NULL;
    }
    FILE *f = calloc(1, sizeof(FILE));
    if (!f)
        return NULL;
    FRESULT r = f_open(&f->fil, path, m);
    if (r != FR_OK) {
        free(f);
        errno = 2;
        return NULL;
    }
    return f;
}

int fclose(FILE *f)
{
    if (!f || f->console)
        return 0;
    f_close(&f->fil);
    free(f);
    return 0;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    if (!f || f->console || !size)
        return 0;
    UINT br = 0;
    if (f_read(&f->fil, buf, size * n, &br) != FR_OK) {
        f->err = 1;
        return 0;
    }
    if (br < size * n)
        f->eof = 1;
    return br / size;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (!f || !size)
        return 0;
    if (f->console) {
        pc486_console_write(buf, size * n);
        return n;
    }
    UINT bw = 0;
    if (f_write(&f->fil, buf, size * n, &bw) != FR_OK) {
        f->err = 1;
        return 0;
    }
    return bw / size;
}

int fseeko(FILE *f, off_t off, int whence)
{
    if (!f || f->console)
        return -1;
    FSIZE_t pos;
    switch (whence) {
    case SEEK_SET: pos = off; break;
    case SEEK_CUR: pos = f_tell(&f->fil) + off; break;
    case SEEK_END: pos = f_size(&f->fil) + off; break;
    default: return -1;
    }
    f->eof = 0;
    return f_lseek(&f->fil, pos) == FR_OK ? 0 : -1;
}

int fseek(FILE *f, long off, int whence)
{
    return fseeko(f, off, whence);
}

off_t ftello(FILE *f)
{
    if (!f || f->console)
        return -1;
    return f_tell(&f->fil);
}

long ftell(FILE *f)
{
    return (long)ftello(f);
}

void rewind(FILE *f)
{
    fseeko(f, 0, SEEK_SET);
}

int fflush(FILE *f)
{
    if (f && !f->console)
        f_sync(&f->fil);
    return 0;
}

int feof(FILE *f) { return f ? f->eof : 1; }
int ferror(FILE *f) { return f ? f->err : 1; }

int fgetc(FILE *f)
{
    unsigned char c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    s[i] = 0;
    return s;
}

int fputc(int c, FILE *f)
{
    char ch = c;
    return fwrite(&ch, 1, 1, f) == 1 ? c : EOF;
}

int fputs(const char *s, FILE *f)
{
    return fwrite(s, 1, strlen(s), f) ? 0 : EOF;
}

int puts(const char *s)
{
    fputs(s, stdout);
    return fputc('\n', stdout);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

/* ------------------------------------------------------------------ */
/* printf                                                             */

typedef struct {
    char *buf;
    size_t size, pos;
} Out;

static void outc(Out *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void outs(Out *o, const char *s, int len, int width, bool left, char pad)
{
    int n = width > len ? width - len : 0;
    if (!left)
        while (n-- > 0) outc(o, pad);
    for (int i = 0; i < len; i++) outc(o, s[i]);
    if (left)
        while (n-- > 0) outc(o, ' ');
}

static int fmt_uint(char *tmp, unsigned long long v, int base, bool upper)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[32];
    int n = 0;
    do {
        rev[n++] = dig[v % base];
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++)
        tmp[i] = rev[n - 1 - i];
    return n;
}

static int fmt_double(char *tmp, double v, int prec, char conv)
{
    int n = 0;
    if (v != v) { memcpy(tmp, "nan", 3); return 3; }
    if (v < 0) { tmp[n++] = '-'; v = -v; }
    if (v > 1e300) { memcpy(tmp + n, "inf", 3); return n + 3; }
    if (prec < 0) prec = 6;
    if (prec > 17) prec = 17;
    int exp10 = 0;
    if (conv == 'e' || conv == 'g') {
        while (v >= 10.0) { v /= 10.0; exp10++; }
        while (v > 0 && v < 1.0) { v *= 10.0; exp10--; }
        if (conv == 'g' && exp10 >= -4 && exp10 < prec) {
            for (int i = 0; i < exp10; i++) v *= 10.0;
            for (int i = 0; i > exp10; i--) v /= 10.0;
            conv = 'f';
            prec = prec - 1 - exp10;
            if (prec < 0) prec = 0;
        } else if (conv == 'g') {
            conv = 'e';
            prec = prec > 0 ? prec - 1 : 0;
        }
    }
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10.0;
    v += r;
    if (conv == 'e' && v >= 10.0) { v /= 10.0; exp10++; }
    unsigned long long ip = (unsigned long long)v;
    n += fmt_uint(tmp + n, ip, 10, false);
    v -= (double)ip;
    if (prec > 0) {
        tmp[n++] = '.';
        for (int i = 0; i < prec; i++) {
            v *= 10.0;
            int d = (int)v;
            tmp[n++] = '0' + d;
            v -= d;
        }
    }
    if (conv == 'e') {
        tmp[n++] = 'e';
        tmp[n++] = exp10 < 0 ? '-' : '+';
        if (exp10 < 0) exp10 = -exp10;
        if (exp10 < 10) tmp[n++] = '0';
        n += fmt_uint(tmp + n, exp10, 10, false);
    }
    return n;
}

int vsnprintf(char *s, size_t size, const char *fmt, va_list ap)
{
    Out o = { s, size, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            outc(&o, *p);
            continue;
        }
        p++;
        bool left = false, plus = false, space = false, alt = false;
        char pad = ' ';
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '0') pad = '0';
            else if (*p == '+') plus = true;
            else if (*p == ' ') space = true;
            else if (*p == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0;
        for (;; p++) {
            if (*p == 'l') lng++;
            else if (*p == 'z' || *p == 'j' || *p == 't') lng = 2;
            else if (*p == 'h') ;
            else break;
        }
        char tmp[64];
        int n = 0;
        switch (*p) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            if (v < 0) tmp[n++] = '-';
            else if (plus) tmp[n++] = '+';
            else if (space) tmp[n++] = ' ';
            int start = n;
            n += fmt_uint(tmp + n, u, 10, false);
            if (prec > n - start) {
                int z = prec - (n - start);
                memmove(tmp + start + z, tmp + start, n - start);
                memset(tmp + start, '0', z);
                n += z;
            }
            if (pad == '0' && !left && (tmp[0] == '-' || tmp[0] == '+') && width > n) {
                outc(&o, tmp[0]);
                outs(&o, tmp + 1, n - 1, width - 1, left, pad);
            } else
                outs(&o, tmp, n, width, left, pad);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            int base = *p == 'u' ? 10 : *p == 'o' ? 8 : 16;
            if (alt && base == 16 && v) { tmp[n++] = '0'; tmp[n++] = *p; }
            int start = n;
            n += fmt_uint(tmp + n, v, base, *p == 'X');
            if (prec > n - start) {
                int z = prec - (n - start);
                memmove(tmp + start + z, tmp + start, n - start);
                memset(tmp + start, '0', z);
                n += z;
            }
            outs(&o, tmp, n, width, left, pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            tmp[n++] = '0'; tmp[n++] = 'x';
            n += fmt_uint(tmp + n, v, 16, false);
            outs(&o, tmp, n, width, left, ' ');
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            outs(&o, tmp, 1, width, left, ' ');
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int len = strlen(str);
            if (prec >= 0 && len > prec) len = prec;
            outs(&o, str, len, width, left, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_arg(ap, double);
            char c = *p | 0x20;
            n = fmt_double(tmp, v, prec, c);
            outs(&o, tmp, n, width, left, pad);
            break;
        }
        case '%':
            outc(&o, '%');
            break;
        default:
            outc(&o, '%');
            if (*p) outc(&o, *p); else p--;
            break;
        }
    }
    if (size)
        s[o.pos < size ? o.pos : size - 1] = 0;
    return o.pos;
}

int snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, 1 << 20, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (n < (int)sizeof(buf)) {
        fwrite(buf, 1, n, f);
    } else {
        char *b = malloc(n + 1);
        if (b) {
            vsnprintf(b, n + 1, fmt, ap);
            fwrite(b, 1, n, f);
            free(b);
        }
    }
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

void perror(const char *s)
{
    fprintf(stderr, "%s: error %d\n", s ? s : "", errno);
}

/* ------------------------------------------------------------------ */
/* stdlib / string extras                                             */

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    do {
        if (*s == (char)c) r = s;
    } while (*s++);
    return (char *)r;
}

void *memchr(const void *p, int c, size_t n)
{
    const unsigned char *q = p;
    for (size_t i = 0; i < n; i++)
        if (q[i] == (unsigned char)c) return (void *)(q + i);
    return NULL;
}

size_t strspn(const char *s, const char *a)
{
    size_t n = 0;
    while (s[n] && strchr(a, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *r)
{
    size_t n = 0;
    while (s[n] && !strchr(r, s[n])) n++;
    return n;
}

char *strerror(int e)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "error %d", e);
    return buf;
}

long strtol(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    unsigned long v = strtoul(s, end, base);
    return neg ? -(long)v : (long)v;
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

static unsigned long rnd = 1;
int rand(void)
{
    rnd = rnd * 1103515245 + 12345;
    return (int)((rnd >> 1) & RAND_MAX);
}
void srand(unsigned seed) { rnd = seed; }

char *getenv(const char *name) { (void)name; return NULL; }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    /* insertion sort: only used for tiny arrays */
    char *b = base;
    char tmp[256];
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

void abort(void)
{
    pc486_fatal("abort() called");
}

void exit(int code)
{
    (void)code;
    pc486_fatal("exit() called");
}

void __pc486_assert_fail(const char *expr, const char *file, int line)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "assertion failed: %s (%s:%d)", expr, file, line);
    pc486_fatal(buf);
}
