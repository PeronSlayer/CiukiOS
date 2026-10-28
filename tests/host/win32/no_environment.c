/* No profile in host harnesses that do not link production kernel32.
 * Printing consequently uses the machine queue (CP_ROOT).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/gdi32.h>

void W32API SetLastError(DWORD);

DWORD W32API GetEnvironmentVariableA(const char *name, char *out, DWORD bytes) {
  (void)name;
  (void)out;
  (void)bytes;
  SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
  return 0;
}
