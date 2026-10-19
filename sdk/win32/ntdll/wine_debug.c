/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Original implementation of the Wine
 * 10.0 include/wine/debug.h boundary (cdecl except __wine_dbg_write).
 * No upstream implementation is copied. Debug buffers live in each TEB. */
#include <w32/ntdll.h>
struct debug_channel {
    unsigned char flags;
    char name[15];
};
unsigned char __wine_dbg_get_channel_flags(struct debug_channel *channel) {
    if (!channel)
        return 0;
    channel->flags &= 7;
    return channel->flags;
}
int __wine_dbg_header(int cls, struct debug_channel *channel, const char *function) {
    (void)function;
    return cls < 0 || cls > 3 || !(__wine_dbg_get_channel_flags(channel) & (1u << cls)) ? -1 : 0;
}
int __wine_dbg_output(const char *text) {
    /* Debug output is disabled by default. Never route arbitrary upstream
     * text (which may include file/registry contents) into W21 records. */
    return (int)w32_strlen(text);
}
int W32API __wine_dbg_write(const char *text, unsigned length) {
    (void)text;
    return (int)length;
}
const char *__wine_dbg_strdup(const char *text) {
    struct w32_teb *teb = NtCurrentTeb();
    char *dst = teb->CoreDebug[teb->CoreDebugIndex++ % 4];
    unsigned n = 0;
    while (text && text[n] && n < 255) {
        dst[n] = text[n];
        n++;
    }
    dst[n] = 0;
    return dst;
}
