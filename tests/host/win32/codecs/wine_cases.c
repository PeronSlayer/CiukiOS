/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Selected portable assertions adapted from pinned Wine 10.0:
 * dlls/msacm32/tests/msacm.c:test_acmFormatSuggest,
 * Copyright (c) 2004 Robert Reif;
 * dlls/msvfw32/tests/msvfw.c:test_ICInfo (Wine contributors; see upstream
 * AUTHORS). This is a narrow test selection, not the complete Wine test
 * programs.
 */
#include "codec.h"
#ifdef _WIN32
#define IMPORT __declspec(dllimport)
#else
#define IMPORT
#endif
IMPORT uint32_t W32API acmFormatSuggest(uintptr_t, const struct cc_wave *,
                                        struct cc_wave *, uint32_t, uint32_t);
IMPORT int W32API ICInfo(uint32_t, uint32_t, struct cc_icinfo *);
uint32_t W32API entry(void) {
  struct cc_wave src = {1, 1, 8000, 16000, 2, 16, 0}, dst = src;
  struct cc_icinfo info, again;
  uint32_t i;
  for (i = 0; i < 2; i++) {
    if (acmFormatSuggest(0, &src, &dst, 16, i ? 0xf0000 : 0))
      return 1;
    if (dst.tag != src.tag || dst.channels != src.channels ||
        dst.rate != src.rate || dst.average != src.average ||
        dst.align != src.align || dst.bits != src.bits)
      return 2;
  }
  for (i = 0; i < 3; i++) {
    if (!ICInfo(0, i, &info) || !info.type || !info.handler)
      return 3;
    if (info.flags || info.version || info.icm_version != 0x104 ||
        info.name[0] || info.description[0])
      return 4;
    if (!ICInfo(info.type, info.handler ^ 0x20, &again))
      return 5;
  }
  if (ICInfo(0, 3, &info) ||
      ICInfo(CC_FOUR('v', 'i', 'd', 'c'), CC_FOUR('f', 'a', 'k', 'e'), &info))
    return 6;
  return 0;
}
#ifdef W32_HOST_MODEL
int main(void) { return (int)entry(); }
#endif
