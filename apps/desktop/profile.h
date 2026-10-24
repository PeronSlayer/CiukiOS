/* SPDX-License-Identifier: MIT
 * Paths inherited by native children. No global environment mutation. */
#ifndef CIUKI_NATIVE_PROFILE_H
#define CIUKI_NATIVE_PROFILE_H
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static inline const char *profile_home(void)
{
    const char *active=getenv("CIUKI_PROFILE");
    const char *s=active && *active ? getenv("HOME") : NULL;
    return s && s[0]=='/' && strlen(s)<96 ? s : "/Documents";
}
static inline const char *profile_settings(void)
{
    const char *s=getenv("CIUKI_SETTINGS");
    return s && s[0]=='/' && strlen(s)<128 ? s : "/system/settings/desktop.cfg";
}
static inline void profile_child(char *out,size_t cap,const char *child)
{ snprintf(out,cap,"%s/%s",profile_home(),child); }
#endif
