/* f5-20B: DDEML client/server bookkeeping (instances, string handles, data
 * handles, name registration) with the Windows 98 error model. CiukiOS has
 * no inter-process DDE transport: DDE conversations exchange global memory
 * handles between processes, and Win32 global memory is private to each
 * process here, so DdeConnect never finds a partner and reports
 * DMLERR_NO_CONV_ESTABLISHED, the result Windows returns when no server
 * answers WM_DDE_INITIATE. Callers (SHELL32 ShellExecute "ddeexec") then
 * fall back to starting the program, as on Windows when the server is not
 * running.
 * References: Microsoft "DdeInitializeA", "DdeUninitialize",
 * "DdeCreateStringHandleA", "DdeQueryStringA", "DdeFreeStringHandle",
 * "DdeGetLastError", "DdeConnect", "DdeDisconnect", "DdeClientTransaction",
 * "DdeCreateDataHandle", "DdeGetData", "DdeFreeDataHandle",
 * "DdeNameService" and "DDEML error codes".
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
#define DMLERR_NO_ERROR 0u
#define DMLERR_DLL_USAGE 0x4004u
#define DMLERR_INVALIDPARAMETER 0x4006u
#define DMLERR_MEMORY_ERROR 0x4008u
#define DMLERR_NO_CONV_ESTABLISHED 0x400au
#define DMLERR_DLL_NOT_INITIALIZED 0x4003u
#define DMLERR_SYS_ERROR 0x400fu
#define APPCMD_CLIENTONLY 0x10u
#define CP_WINANSI 1004u
#define CP_WINUNICODE 1200u
#define INSTANCES 16u
#define STRINGS 128u
#define DATA 64u
#define NAMES 16u
typedef void *(W32API *PFNCALLBACK)(UINT, UINT, void *, uintptr_t, uintptr_t,
                                     void *, uintptr_t, uintptr_t);
static struct {
  DWORD id, tid, flags, error;
  PFNCALLBACK callback;
  BOOL unicode;
  uintptr_t names[NAMES];
} instances[INSTANCES];
static struct {
  UINT refs;
  char text[256];
} strings[STRINGS];
static struct {
  DWORD instance, size, format;
  uintptr_t item;
  uint8_t *data;
} data_handles[DATA];
static DWORD serial;
static UINT instance_of(DWORD id) {
  for (UINT i = 0; id && i < INSTANCES; i++)
    if (instances[i].id == id)
      return i;
  return INSTANCES;
}
static BOOL fail(UINT i, DWORD error) {
  if (i < INSTANCES)
    instances[i].error = error;
  return 0;
}
static UINT initialize(DWORD *id, PFNCALLBACK callback, DWORD flags,
                       DWORD reserved, BOOL unicode) {
  U32_GUARD;
  if (!id || reserved)
    return DMLERR_INVALIDPARAMETER;
  if (*id) {
    /* Reinitialization changes the command and filter flags only. */
    UINT i = instance_of(*id);
    if (i == INSTANCES || instances[i].tid != u32_tid())
      return DMLERR_INVALIDPARAMETER;
    instances[i].flags = flags;
    return DMLERR_NO_ERROR;
  }
  if (!callback)
    return DMLERR_INVALIDPARAMETER;
  for (UINT i = 0; i < INSTANCES; i++)
    if (!instances[i].id) {
      w32_fill(&instances[i], 0, sizeof(instances[i]));
      if (!++serial)
        serial = 1;
      instances[i].id = serial;
      instances[i].tid = u32_tid();
      instances[i].flags = flags;
      instances[i].callback = callback;
      instances[i].unicode = unicode;
      *id = instances[i].id;
      return DMLERR_NO_ERROR;
    }
  return DMLERR_SYS_ERROR;
}
UINT W32API DdeInitializeA(DWORD *id, PFNCALLBACK callback, DWORD flags,
                           DWORD reserved) {
  return initialize(id, callback, flags, reserved, 0);
}
UINT W32API DdeInitializeW(DWORD *id, PFNCALLBACK callback, DWORD flags,
                           DWORD reserved) {
  return initialize(id, callback, flags, reserved, 1);
}
static BOOL valid_string(uintptr_t h) {
  return h >= 0xc000u && h < 0xc000u + STRINGS && strings[h - 0xc000u].refs;
}
BOOL W32API DdeUninitialize(DWORD id) {
  U32_GUARD;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  for (UINT k = 0; k < DATA; k++)
    if (data_handles[k].instance == id) {
      u32_free(data_handles[k].data);
      w32_fill(&data_handles[k], 0, sizeof(data_handles[k]));
    }
  w32_fill(&instances[i], 0, sizeof(instances[i]));
  return 1;
}
UINT W32API DdeGetLastError(DWORD id) {
  U32_GUARD;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return DMLERR_DLL_NOT_INITIALIZED;
  UINT e = instances[i].error;
  instances[i].error = DMLERR_NO_ERROR;
  return e;
}
static uintptr_t create_string(DWORD id, const char *text) {
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if (!text || w32_strlen(text) > 255)
    return fail(i, DMLERR_INVALIDPARAMETER);
  /* String handles are case-insensitive and shared, like global atoms. */
  for (UINT k = 0; k < STRINGS; k++)
    if (strings[k].refs && !u32_stricmp(strings[k].text, text)) {
      strings[k].refs++;
      return 0xc000u + k;
    }
  for (UINT k = 0; k < STRINGS; k++)
    if (!strings[k].refs) {
      strings[k].refs = 1;
      u32_strcpy(strings[k].text, sizeof(strings[k].text), text);
      return 0xc000u + k;
    }
  return fail(i, DMLERR_MEMORY_ERROR);
}
uintptr_t W32API DdeCreateStringHandleA(DWORD id, const char *text, int page) {
  U32_GUARD;
  (void)page;
  return create_string(id, text);
}
uintptr_t W32API DdeCreateStringHandleW(DWORD id, const WCHAR *text, int page) {
  U32_GUARD;
  (void)page;
  if (!text || u32_wlen(text) > 255)
    return fail(instance_of(id), DMLERR_INVALIDPARAMETER);
  char a[256];
  u32_to_ansi(a, sizeof(a), text, -1);
  return create_string(id, a);
}
BOOL W32API DdeFreeStringHandle(DWORD id, uintptr_t h) {
  U32_GUARD;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if (!valid_string(h))
    return fail(i, DMLERR_INVALIDPARAMETER);
  strings[h - 0xc000u].refs--;
  return 1;
}
DWORD W32API DdeQueryStringA(DWORD id, uintptr_t h, char *out, DWORD cap,
                             int page) {
  U32_GUARD;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if (!valid_string(h) || (page && page != (int)CP_WINANSI))
    return fail(i, DMLERR_INVALIDPARAMETER);
  const char *s = strings[h - 0xc000u].text;
  DWORD n = w32_strlen(s);
  if (!out)
    return n;
  if (!cap)
    return 0;
  DWORD k = n < cap - 1 ? n : cap - 1;
  w32_copy(out, s, k);
  out[k] = 0;
  return k;
}
DWORD W32API DdeQueryStringW(DWORD id, uintptr_t h, WCHAR *out, DWORD cap,
                             int page) {
  U32_GUARD;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if (!valid_string(h) || (page && page != (int)CP_WINUNICODE))
    return fail(i, DMLERR_INVALIDPARAMETER);
  const char *s = strings[h - 0xc000u].text;
  DWORD n = w32_strlen(s);
  if (!out)
    return n;
  if (!cap)
    return 0;
  return u32_to_wide(out, cap, s, (int)n);
}
uintptr_t W32API DdeNameService(DWORD id, uintptr_t name, uintptr_t reserved,
                                UINT command) {
  U32_GUARD;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if (instances[i].flags & APPCMD_CLIENTONLY)
    return fail(i, DMLERR_DLL_USAGE);
  if (reserved || (command & ~0xfu) || (name && !valid_string(name)))
    return fail(i, DMLERR_INVALIDPARAMETER);
  if (command & 1) { /* DNS_REGISTER */
    if (!name)
      return fail(i, DMLERR_INVALIDPARAMETER);
    for (UINT k = 0; k < NAMES; k++)
      if (!instances[i].names[k] || instances[i].names[k] == name) {
        instances[i].names[k] = name;
        return 1;
      }
    return fail(i, DMLERR_MEMORY_ERROR);
  }
  if (command & 2) /* DNS_UNREGISTER: one name, or all with a zero name */
    for (UINT k = 0; k < NAMES; k++)
      if (!name || instances[i].names[k] == name)
        instances[i].names[k] = 0;
  return 1;
}
uintptr_t W32API DdeConnect(DWORD id, uintptr_t service, uintptr_t topic,
                            void *context) {
  U32_GUARD;
  (void)context;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if ((service && !valid_string(service)) || (topic && !valid_string(topic)))
    return fail(i, DMLERR_INVALIDPARAMETER);
  return fail(i, DMLERR_NO_CONV_ESTABLISHED);
}
/* No conversation handle is ever issued, so every one is invalid. */
BOOL W32API DdeDisconnect(uintptr_t conversation) {
  (void)conversation;
  return 0;
}
uintptr_t W32API DdeClientTransaction(void *data, DWORD bytes,
                                      uintptr_t conversation, uintptr_t item,
                                      UINT format, UINT type, DWORD timeout,
                                      DWORD *result) {
  (void)data, (void)bytes, (void)conversation, (void)item, (void)format,
      (void)type, (void)timeout;
  if (result)
    *result = 0;
  return 0;
}
static UINT data_index(uintptr_t h) {
  UINT k = (UINT)(h - 0x40000001u);
  return (h >> 28) == 4u && k < DATA && data_handles[k].instance ? k : DATA;
}
uintptr_t W32API DdeCreateDataHandle(DWORD id, const void *source, DWORD bytes,
                                     DWORD offset, uintptr_t item, UINT format,
                                     UINT flags) {
  U32_GUARD;
  (void)flags;
  UINT i = instance_of(id);
  if (i == INSTANCES)
    return 0;
  if (bytes > (1u << 24) || (item && !valid_string(item)))
    return fail(i, DMLERR_INVALIDPARAMETER);
  for (UINT k = 0; k < DATA; k++)
    if (!data_handles[k].instance) {
      uint8_t *p = u32_alloc(bytes ? bytes : 1);
      if (!p)
        return fail(i, DMLERR_MEMORY_ERROR);
      if (source && bytes)
        w32_copy(p, (const uint8_t *)source + offset, bytes);
      data_handles[k].instance = id;
      data_handles[k].size = bytes;
      data_handles[k].format = format;
      data_handles[k].item = item;
      data_handles[k].data = p;
      return 0x40000001u + k;
    }
  return fail(i, DMLERR_MEMORY_ERROR);
}
DWORD W32API DdeGetData(uintptr_t h, void *out, DWORD cap, DWORD offset) {
  U32_GUARD;
  UINT k = data_index(h);
  if (k == DATA)
    return 0;
  DWORD size = data_handles[k].size;
  if (!out)
    return size;
  if (offset > size)
    return fail(instance_of(data_handles[k].instance), DMLERR_INVALIDPARAMETER);
  DWORD n = size - offset < cap ? size - offset : cap;
  w32_copy(out, data_handles[k].data + offset, n);
  return n;
}
BOOL W32API DdeFreeDataHandle(uintptr_t h) {
  U32_GUARD;
  UINT k = data_index(h);
  if (k == DATA)
    return 0;
  u32_free(data_handles[k].data);
  w32_fill(&data_handles[k], 0, sizeof(data_handles[k]));
  return 1;
}
