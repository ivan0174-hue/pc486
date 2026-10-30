#ifndef PC486_OSD_H
#define PC486_OSD_H
#include <stdint.h>
#include "pc.h"
void osd_set_basedir(const char *dir);
void osd_load_options(const char *inifile);
const char *osd_get_option(const char *name);
void osd_init(PC *pc, const PCConfig *conf);
int osd_active(void);
int osd_visible(void);              /* menu or a short message on screen */
int osd_paused(void);
int osd_hotkey(int down, int set1);     /* returns 1 if the key opened the menu */
void osd_key(PC *pc, int down, int set1);
void osd_draw(uint32_t *fb, int pitch, int w, int h);
void osd_draw_message(uint32_t *fb, int pitch, int w, int h, const char *msg);
int osd_take_reboot_request(void);
#endif
