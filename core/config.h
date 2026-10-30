#ifndef PC486_CONFIG_H
#define PC486_CONFIG_H
#include "pc.h"
void pc_config_defaults(PCConfig *c);
/* basedir is prepended to relative paths (may be NULL) */
int pc_config_load(PCConfig *c, const char *path, const char *basedir);
#endif
