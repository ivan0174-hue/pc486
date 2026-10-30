#ifndef RTC_H
#define RTC_H
#include <stdint.h>

typedef struct RTC RTC;
RTC *rtc_new(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level));
void rtc_step(RTC *r);
uint8_t rtc_ioport_read(RTC *r, int port);
void rtc_ioport_write(RTC *r, int port, uint8_t val);
/* wday: 0 = Sunday */
void rtc_set_time(RTC *r, int year, int mon, int day, int wday,
                  int hour, int min, int sec);
uint8_t *rtc_nvram(RTC *r);            /* 256 bytes */
void rtc_load_nvram(RTC *r, const uint8_t *data, int len);
int rtc_take_dirty(RTC *r);            /* 1 if NVRAM changed since last call */

#endif
