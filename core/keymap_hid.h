#ifndef KEYMAP_HID_H
#define KEYMAP_HID_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KEY_PRINTSCREEN 0x1e037
#define KEY_PAUSE       0x1e11d
typedef struct { uint8_t modifiers; uint8_t keys[6]; } HidKbdState;
int hid_to_set1(int usage);
void hid_report_diff(HidKbdState *st, uint8_t modifiers, const uint8_t keys[6],
                     void (*emit)(void *opaque, int down, int set1), void *opaque);
#ifdef __cplusplus
}
#endif
#endif
