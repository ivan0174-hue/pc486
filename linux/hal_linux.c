/* pc486 - platform layer for Linux/POSIX hosts */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/mman.h>
#include <dirent.h>
#include <string.h>
#include "pc.h"

uint32_t get_uticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

void *bigmalloc(size_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

long plat_load_file(const char *path, void *buf, long maxlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = fread(buf, 1, maxlen, f);
    fclose(f);
    return n;
}

int plat_save_file(const char *path, const void *buf, long len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

int plat_list_dir(const char *dir, char names[][64], int max)
{
    DIR *d = opendir(dir);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

/* raw SD card sectors: on Linux a whole-card image file (-sd option) */
const char *plat_sd_image;
static FILE *sd_f;

static int sd_open(void)
{
    if (!sd_f && plat_sd_image)
        sd_f = fopen(plat_sd_image, "r+b");
    return sd_f ? 0 : -1;
}

int plat_raw_read(uint64_t lba, void *buf, int count)
{
    if (sd_open() < 0) return -1;
    fseeko(sd_f, (off_t)lba * 512, SEEK_SET);
    return fread(buf, 512, count, sd_f) == (size_t)count ? 0 : -1;
}

int plat_raw_write(uint64_t lba, const void *buf, int count)
{
    if (sd_open() < 0) return -1;
    fseeko(sd_f, (off_t)lba * 512, SEEK_SET);
    int r = fwrite(buf, 512, count, sd_f) == (size_t)count ? 0 : -1;
    fflush(sd_f);
    return r;
}
