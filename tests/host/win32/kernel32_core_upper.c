/* f5-20A named kernel objects through the production core.c/wide.c records
 * (OpenSemaphoreA/W, OpenEventW, CreateSemaphoreW, CreateFileMappingW) on a
 * low-address stack so the copied 32-bit name pointers stay meaningful.
 * Mock object transport; the kernel side is covered by the object suite.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
// clang-format off
#include <w32/kernel32.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
// clang-format on
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "core upper line %u: %s\n", __LINE__, #c);               \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static struct w32_teb teb;
_Thread_local struct w32_teb *w32_model_teb;
void w32_diag(const char *a, const char *b, const char *c) {
  (void)a;
  (void)b;
  (void)c;
}
uint32_t w32_strlen(const char *s) { return (uint32_t)strlen(s); }
ULONG W32API RtlNtStatusToDosError(NTSTATUS s) { return s ? 317 : 0; }
static struct {
  WCHAR name[261];
  DWORD type;
} objects[8];
static DWORD last_maximum, last_value, last_access, sections;
static int same(const WCHAR *a, const WCHAR *b) {
  for (;; a++, b++) {
    if (*a != *b)
      return 0;
    if (!*a)
      return 1;
  }
}
static void copy(WCHAR *d, const WCHAR *s) {
  unsigned i = 0;
  for (; s[i] && i < 260; i++)
    d[i] = s[i];
  d[i] = 0;
}
static WCHAR section_name[261];
int32_t w32_ciuki_call(void *record, uint32_t n) {
  struct ciuki_w32_header *h = record;
  if (h->operation == W32_SECTION) {
    struct ciuki_w32_section_args *a = record;
    CHECK(n == sizeof(*a) && a->name);
    copy(section_name, (const WCHAR *)(uintptr_t)a->name);
    a->result = 0x40 + 4 * sections++;
    for (unsigned i = 0; i < 8; i++) /* fx-hlrun: OpenFileMapping finds it */
      if (!objects[i].type) {
        objects[i].type = W32_OBJ_SECTION;
        copy(objects[i].name, section_name);
        break;
      }
    return 0;
  }
  CHECK(h->operation == W32_OBJECT && n == sizeof(struct ciuki_w32_object_args));
  struct ciuki_w32_object_args *a = record;
  const WCHAR *name = (const WCHAR *)(uintptr_t)a->name;
  last_maximum = a->maximum;
  last_value = a->value;
  last_access = a->access;
  CHECK(a->action == W32_OBJ_CREATE || a->action == W32_OBJ_OPEN);
  if (a->type == W32_OBJ_SEMAPHORE && (!a->maximum || a->value > a->maximum)) {
    a->error = 87;
    return 0;
  }
  for (unsigned i = 0; i < 8; i++)
    if (name && objects[i].type && same(objects[i].name, name)) {
      if (objects[i].type != a->type)
        a->error = 6;
      else {
        a->result = 0x100 + 4 * i;
        a->error = a->action == W32_OBJ_CREATE ? 183 : 0;
      }
      return 0;
    }
  if (a->action == W32_OBJ_OPEN) {
    a->error = 2;
    return 0;
  }
  for (unsigned i = 0; i < 8; i++)
    if (!objects[i].type) {
      objects[i].type = a->type;
      if (name)
        copy(objects[i].name, name);
      a->result = 0x100 + 4 * i;
      return 0;
    }
  a->error = 8;
  return 0;
}
extern HANDLE W32API CreateSemaphoreA(SECURITY_ATTRIBUTES *, LONG, LONG, const char *);
extern HANDLE W32API OpenSemaphoreA(DWORD, BOOL, const char *);
extern HANDLE W32API OpenSemaphoreW(DWORD, BOOL, const WCHAR *);
extern HANDLE W32API CreateSemaphoreW(SECURITY_ATTRIBUTES *, LONG, LONG, const WCHAR *);
extern HANDLE W32API OpenEventW(DWORD, BOOL, const WCHAR *);
extern HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *, BOOL, BOOL, const char *);
extern HANDLE W32API CreateFileMappingW(HANDLE, SECURITY_ATTRIBUTES *, DWORD, DWORD, DWORD,
                                        const WCHAR *);
static void *run(void *unused) {
  (void)unused;
  w32_model_teb = &teb;
  HANDLE sem = CreateSemaphoreA(0, 1, 4, "f520");
  CHECK(sem == (HANDLE)0x100);
  CHECK(OpenSemaphoreA(0x1f0003, 0, "f520") == sem && last_access == 0x1f0003 &&
        last_maximum == 1 && last_value == 0 && GetLastError() == 0);
  CHECK(!OpenSemaphoreA(0x1f0003, 0, "none") && GetLastError() == 2);
  CHECK(!OpenSemaphoreA(2, 0, 0) && GetLastError() == 87);
  HANDLE event = CreateEventA(0, 1, 0, "evt\xe9");
  CHECK(event == (HANDLE)0x104);
  CHECK(!OpenSemaphoreA(2, 0, "evt\xe9") && GetLastError() == 6);
  char long_name[300];
  memset(long_name, 'n', 299);
  long_name[299] = 0;
  CHECK(!OpenSemaphoreA(2, 0, long_name));
  const WCHAR wide_sem[] = {'f', '5', '2', '0', 0}, event_name[] = {'e', 'v', 't', 0xe9, 0},
              han[] = {0x4e00, 0};
  CHECK(OpenSemaphoreW(3, 0, wide_sem) == sem && last_access == 3);
  CHECK(!OpenSemaphoreW(3, 0, han) && GetLastError() == 123);
  CHECK(OpenEventW(2, 0, event_name) == event && last_access == 2);
  CHECK(!OpenEventW(2, 0, wide_sem) && GetLastError() == 6);
  HANDLE second = CreateSemaphoreW(0, 0, 2, event_name);
  CHECK(!second && GetLastError() == 6);
  const WCHAR fresh[] = {'w', 0x20ac, 0};
  second = CreateSemaphoreW(0, 0, 2, fresh);
  CHECK(second == (HANDLE)0x108 && last_maximum == 2 && objects[2].name[1] == 0x20ac);
  CHECK(CreateSemaphoreW(0, 0, 2, fresh) == second && GetLastError() == 183);
  CHECK(!CreateSemaphoreW(0, 3, 2, 0) && GetLastError() == 87);
  CHECK(CreateFileMappingW(K32_INVALID, 0, 4, 0, 4096, fresh) == (HANDLE)0x40 &&
        section_name[1] == 0x20ac && !section_name[2]);
  CHECK(!CreateFileMappingW(K32_INVALID, 0, 4, 0, 4096, han) && GetLastError() == 123);
  /* fx-hlrun: OpenFileMappingA/W open the named section with full access
   * (Windows 9x does not check section access); no name is 87 without a
   * record, an unknown name 2, another object type 6. */
  extern HANDLE W32API OpenFileMappingA(DWORD, BOOL, const char *);
  extern HANDLE W32API OpenFileMappingW(DWORD, BOOL, const WCHAR *);
  const WCHAR mapping[] = {'H', 'L', 'm', 'a', 'p', 0};
  CHECK(CreateFileMappingW(K32_INVALID, 0, 4, 0, 4096, mapping) == (HANDLE)0x44);
  HANDLE opened = OpenFileMappingA(4 /* FILE_MAP_READ */, 0, "HLmap");
  CHECK(opened && last_access == ~0u && GetLastError() == 0);
  CHECK(OpenFileMappingW(2, 0, mapping) == opened && last_access == ~0u);
  last_access = 0;
  CHECK(!OpenFileMappingA(4, 0, 0) && GetLastError() == 87 && last_access == 0);
  CHECK(!OpenFileMappingW(4, 0, 0) && GetLastError() == 87);
  CHECK(!OpenFileMappingA(4, 0, "nothing") && GetLastError() == 2);
  CHECK(!OpenFileMappingA(4, 0, "evt\xe9") && GetLastError() == 6);
  return 0;
}
int main(void) {
  const size_t bytes = 1024 * 1024;
  void *stack = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                     -1, 0);
  CHECK(stack != MAP_FAILED && (uintptr_t)stack + bytes <= UINT32_MAX);
  pthread_attr_t attr;
  pthread_t thread;
  CHECK(!pthread_attr_init(&attr) && !pthread_attr_setstack(&attr, stack, bytes));
  CHECK(!pthread_create(&thread, &attr, run, 0) && !pthread_join(thread, 0));
  puts("PASS kernel32 core f5-20A named semaphore event mapping records");
  return 0;
}
