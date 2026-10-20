/* f5-20A NTDLL native services for the reused Wine DLLs: NT registry over a
 * w32srv protocol model, virtual memory/handle/time/file queries over a
 * copied-record model, bitmaps, random, image RVAs, string services.
 * Production ntdll sources; runs on a low-address stack because the file
 * query hands 32-bit buffer addresses to the transport. ASan/UBSan.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
// clang-format off
#include <w32/ldr.h>
#include <w32/server.h>
#include <ciuki/w32_core.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
// clang-format on
typedef uint32_t DWORD;
#define CHECK(c)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      fprintf(stderr, "ntdll upper line %u: %s\n", __LINE__, #c);              \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static unsigned checks;
static struct w32_teb teb;
static struct w32_peb peb;
_Thread_local struct w32_teb *w32_model_teb;
const struct w32_services *w32_get_services(void) { return 0; }
void *w32_copy(void *d, const void *s, uint32_t n) { return memmove(d, s, n); }
void *w32_fill(void *d, uint8_t v, uint32_t n) { return memset(d, v, n); }
uint32_t w32_strlen(const char *s) { return s ? (uint32_t)strlen(s) : 0; }
uint16_t w32_u16(const void *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
uint32_t w32_u32(const void *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
void w32_put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static int heap_blocks;
void *W32API RtlAllocateHeap(void *h, ULONG f, uint32_t n) {
  CHECK(h == (void *)&peb && !f);
  heap_blocks++;
  return malloc(n);
}
BOOLEAN W32API RtlFreeHeap(void *h, ULONG f, void *p) {
  CHECK(h == (void *)&peb && !f);
  heap_blocks--;
  free(p);
  return 1;
}
/* Copied-record transport model. */
static int32_t transport_status;
static DWORD injected, closed, vm_flags;
int32_t w32_ciuki_call(void *record, uint32_t n) {
  struct ciuki_w32_header *h = record;
  if (transport_status)
    return transport_status;
  if (h->operation == W32_VM_ALLOC) {
    struct ciuki_w32_vm *a = record;
    CHECK(n == sizeof(*a));
    vm_flags = a->flags;
    a->address = a->address ? a->address & ~0xfffu : 0x500000;
    return 0;
  }
  if (h->operation == W32_OBJECT) {
    struct ciuki_w32_object_args *a = record;
    CHECK(a->action == W32_OBJ_CLOSE);
    if (a->handle != 0x40)
      a->error = 6;
    else
      closed++;
    return 0;
  }
  if (h->operation == W32_SYSTEM) {
    struct ciuki_w32_system_args *a = record;
    CHECK(a->action == W32_S_TIME);
    a->low = 0x12345678;
    a->high = 0x01dc0000;
    return 0;
  }
  CHECK(h->operation == W32_FILE);
  struct ciuki_w32_file_args *a = record;
  if (injected) {
    a->error = injected;
    return 0;
  }
  if (a->action == W32_F_SEEK) {
    CHECK(a->flags == 1 && !a->low && !a->high);
    a->low = 0x1234;
    a->high = 2;
    return 0;
  }
  CHECK(a->action == W32_F_STAT && !a->path && a->bytes == sizeof(struct ciuki_w32_file_info));
  struct ciuki_w32_file_info *f = (void *)(uintptr_t)a->buffer;
  memset(f, 0, sizeof(*f));
  f->attributes = 0x11;
  f->size_low = 0x100;
  f->size_high = 1;
  f->create_date = 0x21;
  f->create_tenths = 150;
  f->access_date = 0x21;
  f->write_date = 0x21;
  f->write_time = (1 << 11) | (2 << 5) | 3;
  return 0;
}
/* w32srv registry model: UTF-8 paths under HKLM, ids from 1. */
static char keys[16][128];
static DWORD key_root[16], last_access, closes, deletes;
static uint32_t lookup_key(DWORD id, char *out) {
  if (id == W32R_HKLM) {
    out[0] = 0;
    return 0;
  }
  if (id >= 1 && id <= 16 && keys[id - 1][0]) {
    strcpy(out, keys[id - 1]);
    return 0;
  }
  return 6;
}
static const char *existing[] = {"Software", "Software\\Classes", "Software\\Classes\\CLSID"};
static char created[8][128];
static int known(const char *path) {
  for (unsigned i = 0; i < 3; i++)
    if (!strcasecmp(path, existing[i]))
      return 1;
  for (unsigned i = 0; i < 8; i++)
    if (created[i][0] && !strcasecmp(path, created[i]))
      return 1;
  return 0;
}
uint32_t w32_server_exchange(const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used) {
  struct w32r_request r;
  memcpy(&r, in, sizeof(r));
  CHECK(n == sizeof(r) + r.path_bytes + r.name_bytes && cap >= sizeof(struct w32r_reply));
  const char *path = (const char *)in + sizeof(r);
  struct w32r_reply reply = {.version = 1};
  char base[128], full[300];
  *used = sizeof(reply);
  if (r.op == W32R_CLOSE) {
    closes++;
    if (r.key < 1 || r.key > 16 || !keys[r.key - 1][0])
      reply.error = 6;
    else
      keys[r.key - 1][0] = 0;
  } else if (r.op == W32R_DELETE_KEY) {
    /* f5-20B: as w32srv, an empty name deletes an open handle's key;
     * a predefined root is refused. */
    reply.error = !*path && (r.key & 0x80000000u) ? 5 : 0;
    deletes++;
  } else {
    CHECK(r.op == W32R_OPEN || r.op == W32R_CREATE);
    last_access = r.access;
    if ((reply.error = lookup_key(r.key, base)))
      goto done;
    if (r.access & ~0xf003fu) {
      reply.error = 87;
      goto done;
    }
    if (r.op == W32R_OPEN && !*path && (r.key & 0x80000000u)) {
      reply.key = r.key; /* as w32srv: an empty open names the root itself */
      goto done;
    }
    snprintf(full, sizeof(full), "%s%s%s", base, *base && *path ? "\\" : "", path);
    if (!known(full)) {
      if (r.op == W32R_OPEN) {
        reply.error = 2;
        goto done;
      }
      for (unsigned i = 0; i < 8; i++)
        if (!created[i][0]) {
          strcpy(created[i], full);
          break;
        }
      reply.disposition = 1;
    } else
      reply.disposition = 2;
    for (unsigned i = 0; i < 16; i++)
      if (!keys[i][0]) {
        strcpy(keys[i], full);
        key_root[i] = r.key;
        reply.key = i + 1;
        break;
      }
  }
done:
  memcpy(out, &reply, sizeof(reply));
  return 0;
}
struct object_attributes {
  ULONG length;
  void *root;
  const UNICODE_STRING *name;
  ULONG attributes;
  void *sd, *qos;
};
#define DECL(ret, name, args) extern ret W32API name args
DECL(NTSTATUS, NtOpenKey, (void **, ULONG, const struct object_attributes *));
DECL(NTSTATUS, NtCreateKey, (void **, ULONG, const struct object_attributes *, ULONG,
                             const UNICODE_STRING *, ULONG, ULONG *));
DECL(NTSTATUS, NtDeleteKey, (void *));
DECL(NTSTATUS, NtClose, (void *));
DECL(NTSTATUS, NtAllocateVirtualMemory, (void *, void **, ULONG, ULONG *, ULONG, ULONG));
DECL(NTSTATUS, NtQuerySystemTime, (int64_t *));
DECL(BOOLEAN, RtlTimeToSecondsSince1970, (const int64_t *, ULONG *));
DECL(NTSTATUS, NtQueryInformationFile, (void *, void *, void *, ULONG, ULONG));
struct bitmap {
  ULONG size;
  ULONG *buffer;
};
DECL(void, RtlInitializeBitMap, (struct bitmap *, ULONG *, ULONG));
DECL(void, RtlSetBits, (struct bitmap *, ULONG, ULONG));
DECL(BOOLEAN, RtlAreBitsSet, (const struct bitmap *, ULONG, ULONG));
DECL(BOOLEAN, RtlAreBitsClear, (const struct bitmap *, ULONG, ULONG));
DECL(ULONG, RtlRandom, (ULONG *));
DECL(void *, RtlImageRvaToVa, (const void *, void *, ULONG, const void **));
DECL(BOOLEAN, RtlCreateUnicodeStringFromAsciiz, (UNICODE_STRING *, const char *));
DECL(NTSTATUS, RtlUnicodeToMultiByteN, (char *, ULONG, ULONG *, const WCHAR *, ULONG));
DECL(NTSTATUS, RtlUnicodeToMultiByteSize, (ULONG *, const WCHAR *, ULONG));
DECL(NTSTATUS, RtlGUIDFromString, (const UNICODE_STRING *, void *));
DECL(BOOLEAN, RtlIsTextUnicode, (const void *, int, int *));
void w32_registry_forget(ULONG);
static UNICODE_STRING us(const char *s) {
  static WCHAR buffers[8][128];
  static unsigned next;
  WCHAR *w = buffers[next++ % 8];
  unsigned i = 0;
  for (; s[i]; i++)
    w[i] = (uint8_t)s[i];
  w[i] = 0;
  return (UNICODE_STRING){(uint16_t)(i * 2), (uint16_t)(i * 2 + 2), w};
}
static void registry(void) {
  UNICODE_STRING classes = us("\\REGISTRY\\Machine\\Software\\Classes");
  struct object_attributes attr = {sizeof(attr), 0, &classes, 0x40, 0, 0};
  void *root = 0;
  CHECK(!NtOpenKey(&root, 0x02000000, &attr) && root && last_access == 0xf003f);
  UNICODE_STRING sub = us("CLSID\\{0000}");
  struct object_attributes rel = {sizeof(rel), root, &sub, 0, 0, 0};
  void *key = 0;
  ULONG disposition = 0;
  CHECK(!NtCreateKey(&key, 0x80000000u | 0x100, &rel, 0, 0, 1, &disposition) && key &&
        disposition == 1 && last_access == 0x20019);
  void *again = 0;
  CHECK(!NtCreateKey(&again, 0x20019, &rel, 0, 0, 0, &disposition) && disposition == 2);
  CHECK(NtCreateKey(&again, 0x20019, &rel, 0, 0, 2, &disposition) == STATUS_INVALID_PARAMETER);
  UNICODE_STRING missing = us("Missing");
  struct object_attributes absent = {sizeof(absent), root, &missing, 0, 0, 0};
  void *none = (void *)1;
  CHECK(NtOpenKey(&none, 1, &absent) == (NTSTATUS)0xc0000034 && !none);
  UNICODE_STRING bad_root = us("\\Registry\\Foo\\x");
  struct object_attributes foreign = {sizeof(foreign), 0, &bad_root, 0, 0, 0};
  CHECK(NtOpenKey(&none, 1, &foreign) == (NTSTATUS)0xc0000034);
  UNICODE_STRING leading = us("\\x");
  struct object_attributes syntax = {sizeof(syntax), root, &leading, 0, 0, 0};
  CHECK(NtOpenKey(&none, 1, &syntax) == (NTSTATUS)0xc000003b);
  CHECK(NtOpenKey(&none, 1, 0) == (NTSTATUS)0xc0000005);
  CHECK(NtOpenKey(&none, 0x01000000, &rel) == STATUS_INVALID_PARAMETER);
  UNICODE_STRING odd = {3, 4, sub.Buffer};
  struct object_attributes odd_attr = {sizeof(odd_attr), root, &odd, 0, 0, 0};
  CHECK(NtOpenKey(&none, 1, &odd_attr) == (NTSTATUS)0xc0000033);
  UNICODE_STRING machine = us("\\Registry\\Machine");
  struct object_attributes top = {sizeof(top), 0, &machine, 0, 0, 0};
  void *hklm = 0;
  CHECK(!NtOpenKey(&hklm, 1, &top) && hklm == (void *)(uintptr_t)W32R_HKLM);
  DWORD deleted = deletes;
  CHECK(NtDeleteKey(key) == 0 && deletes == deleted + 1);
  CHECK(NtDeleteKey(hklm) == (NTSTATUS)0xc0000022);
  CHECK(NtDeleteKey(0) == (NTSTATUS)0xc0000008);
  DWORD before = closes;
  CHECK(!NtClose(key) && closes == before + 1);
  CHECK(NtClose(key) == (NTSTATUS)0xc0000008 && closes == before + 1);
  w32_registry_forget((ULONG)(uintptr_t)again);
  CHECK(NtClose(again) == (NTSTATUS)0xc0000008 && closes == before + 1);
  CHECK(!NtClose((void *)0x40) && closed == 1);
  CHECK(!NtClose(root) && closes == before + 2);
  CHECK(!heap_blocks);
  puts("PASS ntdll upper NT registry paths access rights dispositions close routing");
}
static void native(void) {
  void *base = 0;
  ULONG size = 5000;
  CHECK(!NtAllocateVirtualMemory((void *)(intptr_t)-1, &base, 0, &size, 0x3000, 4) &&
        base == (void *)0x500000 && size == 8192 && vm_flags == 0x3000);
  base = (void *)0x600123;
  size = 5000;
  CHECK(!NtAllocateVirtualMemory((void *)(intptr_t)-1, &base, 0, &size, 0x1000, 4) &&
        base == (void *)0x600000 && size == 8192);
  CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-2, &base, 0, &size, 0x1000, 4) ==
        (NTSTATUS)0xc0000008);
  CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-1, &base, 1, &size, 0x1000, 4) ==
        (NTSTATUS)0xc00000f1);
  size = 0;
  CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-1, &base, 0, &size, 0x1000, 4) ==
        (NTSTATUS)0xc00000f2);
  CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-1, 0, 0, &size, 0x1000, 4) ==
        (NTSTATUS)0xc0000005);
  size = 1;
  transport_status = -12;
  CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-1, &base, 0, &size, 0x1000, 4) == STATUS_NO_MEMORY);
  transport_status = 0;
  int64_t now = 0;
  CHECK(!NtQuerySystemTime(&now) && now == 0x01dc000012345678ll);
  CHECK(NtQuerySystemTime(0) == (NTSTATUS)0xc0000005);
  ULONG seconds = 7;
  int64_t epoch = 116444736000000000ll, later = epoch + 17608500000000000ll;
  CHECK(RtlTimeToSecondsSince1970(&epoch, &seconds) && seconds == 0);
  CHECK(RtlTimeToSecondsSince1970(&later, &seconds) && seconds == 1760850000u);
  int64_t early = epoch - 1, far = epoch + 0x100000000ll * 10000000;
  seconds = 7;
  CHECK(!RtlTimeToSecondsSince1970(&early, &seconds) && seconds == 7);
  CHECK(!RtlTimeToSecondsSince1970(&far, &seconds) && seconds == 7);
  struct {
    NTSTATUS status;
    ULONG information;
  } io = {1, 1};
  uint8_t info[48];
  CHECK(!NtQueryInformationFile((void *)4, &io, info, 40, 4) && !io.status && io.information == 40);
  int64_t times[4];
  memcpy(times, info, 32);
  CHECK(times[0] == 119600064000000000ll + 150 * 100000 && times[1] == 119600064000000000ll &&
        times[2] == 119600064000000000ll + (3600 + 120 + 6) * 10000000ll && times[3] == times[2] &&
        w32_u32(info + 32) == 0x11);
  CHECK(!NtQueryInformationFile((void *)4, &io, info, 24, 5) && io.information == 24);
  int64_t end, allocation;
  memcpy(&allocation, info, 8);
  memcpy(&end, info + 8, 8);
  CHECK(end == 0x100000100ll && allocation == 0x100001000ll && w32_u32(info + 16) == 1 &&
        !info[20] && info[21] == 1);
  CHECK(!NtQueryInformationFile((void *)4, &io, info, 8, 14) && w32_u32(info) == 0x1234 &&
        w32_u32(info + 4) == 2);
  CHECK(NtQueryInformationFile((void *)4, &io, info, 39, 4) == (NTSTATUS)0xc0000004);
  CHECK(NtQueryInformationFile((void *)4, &io, info, 40, 9) == (NTSTATUS)0xc0000003);
  injected = 6;
  memset(info, 0xa5, sizeof(info));
  CHECK(NtQueryInformationFile((void *)99, &io, info, 40, 4) == (NTSTATUS)0xc0000008 &&
        io.status == (NTSTATUS)0xc0000008 && !io.information && info[0] == 0xa5);
  injected = 0;
  struct bitmap map;
  ULONG bits[2] = {0, 0};
  RtlInitializeBitMap(&map, bits, 64);
  CHECK(map.size == 64 && map.buffer == bits);
  RtlSetBits(&map, 30, 5);
  CHECK(bits[0] == 0xc0000000u && bits[1] == 7);
  CHECK(RtlAreBitsSet(&map, 30, 5) && !RtlAreBitsSet(&map, 29, 2) && RtlAreBitsClear(&map, 0, 30) &&
        !RtlAreBitsClear(&map, 0, 31) && RtlAreBitsClear(&map, 35, 29));
  RtlSetBits(&map, 60, 5);
  CHECK(bits[1] == 7 && !RtlAreBitsSet(&map, 63, 2) && !RtlAreBitsClear(&map, 64, 1) &&
        !RtlAreBitsSet(&map, 0, 0));
  ULONG seed = 1;
  CHECK(RtlRandom(&seed) == 16807 && seed == 16807 && RtlRandom(&seed) == 282475249);
  seed = 0;
  CHECK(RtlRandom(&seed) == 16807 && !RtlRandom(0));
  ULONG value = 0x7ffffffe;
  for (unsigned i = 0; i < 1000; i++) {
    ULONG r = RtlRandom(&value);
    CHECK(r && r < 0x7fffffff);
  }
  /* PE header: two sections, raw data offset differs from the RVA. */
  uint8_t headers[24 + 224 + 80] = {0};
  memcpy(headers, "PE\0\0", 4);
  headers[6] = 2;
  headers[20] = 224;
  uint8_t *section = headers + 24 + 224;
  w32_put32(section + 12, 0x1000);
  w32_put32(section + 16, 0x200);
  w32_put32(section + 20, 0x400);
  w32_put32(section + 40 + 12, 0x2000);
  w32_put32(section + 40 + 16, 0x100);
  w32_put32(section + 40 + 20, 0x600);
  uint8_t file[0x800];
  const void *last = 0;
  CHECK(RtlImageRvaToVa(headers, file, 0x2010, &last) == file + 0x610 && last == section + 40);
  CHECK(RtlImageRvaToVa(headers, file, 0x20ff, &last) == file + 0x6ff);
  CHECK(RtlImageRvaToVa(headers, file, 0x1004, &last) == file + 0x404 && last == section);
  CHECK(!RtlImageRvaToVa(headers, file, 0x2100, &last) && last == section);
  CHECK(RtlImageRvaToVa(headers, file, 0x1000, 0) == file + 0x400 && !RtlImageRvaToVa(0, file, 1, 0));
  puts("PASS ntdll upper virtual memory handles time file queries bitmaps random image RVAs");
}
static void strings(void) {
  UNICODE_STRING u = {0};
  CHECK(RtlCreateUnicodeStringFromAsciiz(&u, "Caf\xe9\x80") && u.Length == 10 && u.MaximumLength == 12 &&
        u.Buffer[3] == 0xe9 && u.Buffer[4] == 0x20ac && !u.Buffer[5] && heap_blocks == 1);
  RtlFreeHeap(&peb, 0, u.Buffer);
  CHECK(!RtlCreateUnicodeStringFromAsciiz(&u, 0) && !RtlCreateUnicodeStringFromAsciiz(0, "x"));
  const WCHAR text[] = {0x20ac, 'x', 0x4e00, 'y'};
  char out[8] = {0};
  ULONG done = 0;
  CHECK(!RtlUnicodeToMultiByteN(out, 8, &done, text, 8) && done == 4 && (uint8_t)out[0] == 0x80 &&
        out[1] == 'x' && out[2] == '?' && out[3] == 'y');
  CHECK(!RtlUnicodeToMultiByteN(out, 2, &done, text, 8) && done == 2);
  CHECK(!RtlUnicodeToMultiByteN(out, 8, 0, text, 3) && RtlUnicodeToMultiByteN(0, 8, &done, text, 8));
  ULONG size = 0;
  CHECK(!RtlUnicodeToMultiByteSize(&size, text, 8) && size == 4 && RtlUnicodeToMultiByteSize(0, text, 2));
  UNICODE_STRING g = us("{6B29FC40-CA47-1067-b31d-00DD010662DA}");
  uint8_t guid[16];
  static const uint8_t expected[16] = {0x40, 0xfc, 0x29, 0x6b, 0x47, 0xca, 0x67, 0x10,
                                       0xb3, 0x1d, 0x00, 0xdd, 0x01, 0x06, 0x62, 0xda};
  CHECK(!RtlGUIDFromString(&g, guid) && !memcmp(guid, expected, 16));
  memset(guid, 0x5a, 16);
  const char *bad[] = {"6B29FC40-CA47-1067-B31D-00DD010662DA}", "{6B29FC40-CA47-1067-B31D-00DD010662DA",
                       "{6B29FC40-CA47-1067-B31D-00DD010662DX}", "{6B29FC40CCA47-1067-B31D-00DD010662DA}",
                       "{6B29FC40-CA47-1067-B31D-00DD010662DA}x"};
  for (unsigned i = 0; i < 5; i++) {
    UNICODE_STRING b = us(bad[i]);
    CHECK(RtlGUIDFromString(&b, guid) == STATUS_INVALID_PARAMETER && guid[0] == 0x5a);
  }
  const WCHAR hello[] = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '\r', '\n'};
  int flags = -1;
  CHECK(RtlIsTextUnicode(hello, sizeof(hello), &flags) && (flags & 0x1) && (flags & 0x2) &&
        (flags & 0x4) && (flags & 0x1000) && !(flags & 0x0ff0));
  CHECK(RtlIsTextUnicode(hello, sizeof(hello), 0));
  const char ansi[] = "Hello World, ANSI!\r\n";
  CHECK(!RtlIsTextUnicode(ansi, (int)strlen(ansi), 0));
  flags = -1;
  CHECK(!RtlIsTextUnicode(ansi, (int)strlen(ansi), &flags) && (flags & 0x100));
  const WCHAR bom[] = {0xfeff, 0x4e00, 0x4e01};
  flags = 0x8;
  CHECK(RtlIsTextUnicode(bom, sizeof(bom), &flags) && flags == 0x8);
  const WCHAR reverse[] = {0xfffe, 0x4100, 0x4200};
  flags = -1;
  CHECK(!RtlIsTextUnicode(reverse, sizeof(reverse), &flags) && (flags & 0x80));
  flags = -1;
  CHECK(!RtlIsTextUnicode(hello, 7, &flags) && (flags & 0x200));
  flags = 9;
  CHECK(!RtlIsTextUnicode(hello, 1, &flags) && !flags && !RtlIsTextUnicode(0, 4, 0));
  puts("PASS ntdll upper ACP strings multibyte sizes GUID parsing text detection");
}
static void *run(void *unused) {
  (void)unused;
  w32_model_teb = &teb;
  teb.ProcessEnvironmentBlock = &peb;
  peb.ProcessHeap = &peb;
  registry();
  native();
  strings();
  return 0;
}
int main(void) {
  const size_t bytes = 1024 * 1024;
  void *stack = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                     -1, 0);
  CHECK(stack != MAP_FAILED);
  pthread_attr_t attr;
  pthread_t thread;
  CHECK(!pthread_attr_init(&attr) && !pthread_attr_setstack(&attr, stack, bytes));
  CHECK(!pthread_create(&thread, &attr, run, 0) && !pthread_join(thread, 0));
  printf("PASS ntdll upper checks=%u\n", checks);
  return 0;
}
