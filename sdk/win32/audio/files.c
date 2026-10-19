/* Private UTF-16 file/resource adapter over existing F5 endpoints.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
struct w32_resource_info {
  uintptr_t type, name, language;
};
HANDLE wa_file_open(const void *name, int wide) {
  if (!wide)
    return CreateFileA(name, 0x80000000, 7, 0, 3, 0, 0);
  if (!name)
    return K32_INVALID;
  const WCHAR *p = name;
  char path[778];
  uint32_t at = 0;
  for (unsigned i = 0;; i++) {
    uint32_t c = p[i];
    if (!c) {
      path[at] = 0;
      break;
    }
    if (i >= 259 || (c >= 0xd800 && c <= 0xdfff))
      return K32_INVALID;
    unsigned n = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    if (at + n >= sizeof(path))
      return K32_INVALID;
    if (n == 1)
      path[at++] = (char)c;
    else {
      if (n == 3)
        path[at++] = (char)(0xe0 | (c >> 12));
      path[at++] =
          (char)((n == 3 ? 0x80 : 0xc0) | ((c >> 6) & (n == 3 ? 63 : 31)));
      path[at++] = (char)(0x80 | (c & 63));
    }
  }
  struct ciuki_w32_file_args a = {.action = W32_F_OPEN,
                                  .path = K32_PTR(path),
                                  .access = 0x80000000,
                                  .share = 7,
                                  .disposition = 3};
  a.header = K32_HEADER(W32_FILE, a);
  return w32_ciuki_call(&a, sizeof(a)) || a.error ? K32_INVALID
                                                  : (HANDLE)(uintptr_t)a.result;
}
void *wa_resource_w(void *module, const WCHAR *name) {
  static const WCHAR type[] = {'W', 'A', 'V', 'E', 0};
  struct w32_resource_info info = {(uintptr_t)type, (uintptr_t)name, 0x409};
  if (!module)
    module = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  void *resource = 0;
  return LdrFindResource_U(module, &info, 3, &resource) ? 0 : resource;
}
