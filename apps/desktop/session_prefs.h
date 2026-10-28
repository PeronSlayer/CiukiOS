/* SPDX-License-Identifier: MIT
 * sec-user-04: the per-user lock preferences of a LOGON session, kept as
 * extra lines of the user's own desktop.cfg (CIUKI_PROFILE_SETTINGS), like
 * Windows XP's HKCU "ScreenSaverIsSecure" and the power-options "Prompt for
 * password when computer resumes from standby". Absent lines mean on: once
 * the owner has a password, both protections are enabled by default
 * (user-security-contract.md 4.3); the user may turn them off for himself.
 * A preference is never authority: LOGON decides (a password-less owner gets
 * no lock), and the G32 interim desktop keeps its USERS_LOCK/power_password.
 * Header-only: the desktop and the Control Panel share it.
 */
#ifndef DESK_SESSION_PREFS_H
#define DESK_SESSION_PREFS_H
#include <stdlib.h>
#include <string.h>
#define DESK_SECURE_SAVER "secure_saver"
#define DESK_SECURE_RESUME "secure_resume"
/* 1 (default) or 0 from a "key=0|1" line of extra. */
static inline int desk_secure_pref(const char *extra, const char *key)
{
    size_t n=strlen(key);
    for (const char *p=extra ? extra : ""; *p;) {
        const char *end=strchr(p,'\n'); size_t len=end ? (size_t)(end-p) : strlen(p);
        if (len==n+2 && !strncmp(p,key,n) && p[n]=='=' && (p[n+1]=='0' || p[n+1]=='1')) return p[n+1]=='1';
        p+=len; if (*p) p++;
    }
    return 1;
}
/* Replaces (or appends) the key's line in extra, keeping every other line in
 * order; 0, or -1 when the result does not fit size. */
static inline int desk_secure_store(char *extra, size_t size, const char *key, int value)
{
    size_t n=strlen(key), used=0;
    char *kept=malloc(size);
    if (!kept) return -1;
    for (const char *p=extra; *p;) {
        const char *end=strchr(p,'\n'); size_t len=end ? (size_t)(end-p) : strlen(p);
        if (len && !(len>n && !strncmp(p,key,n) && p[n]=='=')) {
            if (used+len+2>size) { free(kept); return -1; }
            memcpy(kept+used,p,len); used+=len; kept[used++]='\n';
        }
        p+=len; if (*p) p++;
    }
    if (used+n+4>size) { free(kept); return -1; }
    memcpy(kept+used,key,n); used+=n; kept[used++]='='; kept[used++]=value ? '1' : '0'; kept[used++]='\n'; kept[used]=0;
    memcpy(extra,kept,used+1); free(kept);
    return 0;
}
#endif
