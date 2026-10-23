/* Exact facade/transport tests on a low-address host thread stack, so the
 * production 32-bit copied pointers retain their meaning on a 64-bit host.
 * This is a mock transport, complemented by kernel32_files.c's real VFS.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
/* Keep the personality ABI before libc, which supplies the host mmap macros. */
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
      fprintf(stderr, "file wrapper line %u: %s\n", __LINE__, #c);             \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static DWORD last_error, calls, signals, io_flags, io_bytes, io_low, io_high;
static DWORD injected_error, opened, closes, copied, stat_path, seek_method;
/* f5-20A: deterministic FAT-to-FILETIME stand-in (time.c is not linked). */
BOOL W32API DosDateTimeToFileTime(uint16_t date, uint16_t time, FILETIME *out) {
  out->low = date;
  out->high = time;
  return 1;
}
/* G1: the current time-zone bias stand-in (UTC; time.c is not linked). */
int64_t k32_current_bias(void) { return 0; }
extern BOOL W32API GetFileAttributesExA(const char *, DWORD, void *);
extern BOOL W32API GetFileInformationByHandle(HANDLE, BY_HANDLE_FILE_INFORMATION *);
extern BOOL W32API GetFileSizeEx(HANDLE, int64_t *);
extern BOOL W32API SetFilePointerEx(HANDLE, int64_t, int64_t *, DWORD);
extern BOOL W32API DeviceIoControl(HANDLE, DWORD, void *, DWORD, void *, DWORD, DWORD *,
                                   OVERLAPPED *);
extern DWORD W32API GetFinalPathNameByHandleW(HANDLE, WCHAR *, DWORD, DWORD);
static unsigned spawn_calls, spawn_failure;
static const char *expected_command;
static const char *expected_environment="A=B\0\0";
static DWORD expected_environment_bytes=5;
struct startup_a {
  DWORD size;
  char *reserved, *desktop, *title;
  DWORD x, y, width, height, chars_x, chars_y, fill, flags;
  uint16_t show, reserved_bytes;
  uint8_t *reserved2;
  HANDLE input, output, error;
};
struct process_info {
  HANDLE process, thread;
  DWORD pid, tid;
};
extern BOOL W32API CreateProcessA(const char *, char *, SECURITY_ATTRIBUTES *,
                                  SECURITY_ATTRIBUTES *, BOOL, DWORD, void *,
                                  const char *, struct startup_a *,
                                  struct process_info *);
char *W32API GetEnvironmentStringsA(void) {
  static char env[2];
  return env;
}
BOOL W32API FreeEnvironmentStringsA(char *p) {
  (void)p;
  return 1;
}
uint32_t w32_strlen(const char *p) { return (uint32_t)strlen(p); }
int w32_copy_string(char *out, uint32_t n, const char *p) {
  if (strlen(p) >= n)
    return 0;
  strcpy(out, p);
  return 1;
}
static struct ciuki_w32_file_info source, destination;
static unsigned char input[4] = {'d', 'a', 't', 'a'}, output[4];
DWORD W32API GetLastError(void) { return last_error; }
void W32API SetLastError(DWORD e) { last_error = e; }
int k32_status(int32_t s) {
  if (s < 0) {
    last_error = 87;
    return 0;
  }
  return 1;
}
void w32_diag(const char *a,const char *b,const char *c){(void)a;(void)b;(void)c;}
void *k32_alloc(DWORD bytes) {
  void *p = mmap(0, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}
void k32_free(void *p, DWORD bytes) {
  if (p)
    CHECK(!munmap(p, bytes));
}
void *w32_copy(void *to, const void *from, ULONG n) {
  return memcpy(to, from, n);
}
BOOL W32API CloseHandle(HANDLE h) {
  CHECK(h == (HANDLE)4 || h == (HANDLE)8);
  closes++;
  return 1;
}
extern BOOL W32API CopyFileA(const char *, const char *, BOOL);
int32_t w32_ciuki_call(void *record, uint32_t bytes) {
  (void)bytes;
  struct ciuki_w32_header *header = record;
  if (header->operation == W32_PROCESS) {
    struct ciuki_w32_spawn_args *a = record;
    CHECK(!a->error &&
          !strcmp((char *)(uintptr_t)a->command, expected_command));
    CHECK(a->inherit == 1 && a->handle_flags == 3);
    CHECK(!(a->flags&0x400)&&a->environment_bytes==expected_environment_bytes);
    CHECK(!memcmp((void *)(uintptr_t)a->environment,expected_environment,expected_environment_bytes));
    const char *candidate = (char *)(uintptr_t)a->image;
    CHECK(!strcmp(candidate,
                  spawn_calls ? "C:\\Program Files\\Tool.exe" : "C:\\Program"));
    spawn_calls++;
    if (spawn_failure) {
      a->error = spawn_failure;
      return 0;
    }
    if (spawn_calls == 1) {
      a->error = 2;
      return 0;
    }
    a->process_handle = 16;
    a->thread_handle = 20;
    a->pid = 7;
    a->tid = 8;
    return 0;
  }
  if (header->operation == W32_OBJECT) {
    struct ciuki_w32_object_args *a = record;
    CHECK(a->action == W32_OBJ_SET && a->handle == 12);
    signals++;
    return 0;
  }
  CHECK(header->operation == W32_FILE);
  struct ciuki_w32_file_args *a = record;
  calls++;
  switch (a->action) {
  case W32_F_LOCK:
  case W32_F_UNLOCK:
    io_flags=a->flags;io_bytes=a->bytes;io_low=a->low;io_high=a->high;
    CHECK(a->share==1);a->error=injected_error;break;
  case W32_F_OPEN:
    a->result = opened++ ? 8 : 4;
    break;
  case W32_F_STAT:
    if (injected_error) {
      a->error = injected_error;
      break;
    }
    stat_path = a->path;
    memcpy((void *)(uintptr_t)a->buffer, &source, sizeof(source));
    break;
  case W32_F_SEEK:
    seek_method = a->flags;
    io_low = a->low;
    io_high = a->high;
    a->low += 10;
    break;
  case W32_F_READ:
  case W32_F_WRITE:
    io_flags = a->flags;
    io_bytes = a->bytes;
    io_low = a->low;
    io_high = a->high;
    if (injected_error) {
      a->error = injected_error;
      break;
    }
    if (!opened) {
      a->result = a->bytes;
      break;
    }
    if (a->action == W32_F_READ) {
      a->result = copied ? 0 : sizeof(input);
      if (a->result)
        memcpy((void *)(uintptr_t)a->buffer, input, sizeof(input));
    } else {
      CHECK(a->bytes == sizeof(input));
      memcpy(output, (void *)(uintptr_t)a->buffer, sizeof(output));
      a->result = a->bytes;
      copied += a->bytes;
    }
    break;
  case W32_F_FLUSH:
    break;
  case W32_F_TIMES:
    CHECK(a->flags == 7 && a->handle == 8);
    memcpy(&destination, (void *)(uintptr_t)a->buffer, sizeof(destination));
    break;
  case W32_F_ATTR:
    destination.attributes = a->flags;
    break;
  default:
    CHECK(0);
  }
  return 0;
}
static void upper_files(void) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  memset(&fa, 0xa5, sizeof(fa));
  WIN32_FILE_ATTRIBUTE_DATA before = fa;
  DWORD n = calls;
  CHECK(!GetFileAttributesExA("C:\\x", 1, &fa) && GetLastError() == 87 && calls == n &&
        !memcmp(&fa, &before, sizeof(fa)));
  source.size_low = 5;
  source.size_high = 1;
  CHECK(GetFileAttributesExA("C:\\x", 0, &fa) && stat_path && fa.attributes == 0x27 &&
        fa.size_low == 5 && fa.size_high == 1 && fa.write.low == 0x5822 &&
        fa.write.high == 0x3344 && fa.access.low == 0x5823 && !fa.access.high &&
        fa.creation.low == 0x5821 + 12300000u && fa.creation.high == 0x1122);
  injected_error = 2;
  CHECK(!GetFileAttributesExA("C:\\missing", 0, &fa) && GetLastError() == 2);
  BY_HANDLE_FILE_INFORMATION bh;
  CHECK(!GetFileInformationByHandle((HANDLE)4, &bh) && GetLastError() == 2);
  int64_t size = 0;
  CHECK(!GetFileSizeEx((HANDLE)4, &size) && !size && GetLastError() == 2);
  injected_error = 0;
  source.volume_serial = 0xc1a001;
  source.file_id = UINT64_C(0xfedcba9876543210);
  CHECK(GetFileInformationByHandle((HANDLE)4, &bh) && !stat_path && bh.links == 1 &&
        bh.attributes == 0x27 && bh.size_low == 5 && bh.size_high == 1 &&
        bh.serial == 0xc1a001 && bh.index_high == 0xfedcba98 && bh.index_low == 0x76543210);
  source.volume_serial = 0;
  source.file_id = 0;
  CHECK(GetFileInformationByHandle((HANDLE)4, &bh) && !bh.serial &&
        !bh.index_high && !bh.index_low);
  CHECK(!GetFileInformationByHandle((HANDLE)4, 0) && GetLastError() == 87);
  CHECK(GetFileSizeEx((HANDLE)4, &size) && size == 0x100000005ll);
  CHECK(!GetFileSizeEx((HANDLE)4, 0) && GetLastError() == 87);
  int64_t position = 0;
  CHECK(SetFilePointerEx((HANDLE)4, 0x200000003ll, &position, 1) && seek_method == 1 &&
        io_low == 3 && io_high == 2 && position == 0x20000000dll);
  CHECK(SetFilePointerEx((HANDLE)4, -2, 0, 2) && io_low == 0xfffffffeu && io_high == ~0u);
  CHECK(!SetFilePointerEx((HANDLE)4, 0, &position, 3) && GetLastError() == 87);
  DWORD done = 9;
  CHECK(!DeviceIoControl((HANDLE)4, 0x90018, 0, 0, 0, 0, &done, 0) && GetLastError() == 50 &&
        !done);
  injected_error = 6;
  CHECK(!DeviceIoControl((HANDLE)99, 0x90018, 0, 0, 0, 0, &done, 0) && GetLastError() == 6);
  WCHAR path[8] = {0xaaaa};
  CHECK(!GetFinalPathNameByHandleW((HANDLE)99, path, 8, 0) && GetLastError() == 6);
  injected_error = 0;
  CHECK(!GetFinalPathNameByHandleW((HANDLE)4, path, 8, 0) && GetLastError() == 50 &&
        path[0] == 0xaaaa);
  CHECK(!GetFinalPathNameByHandleW((HANDLE)4, path, 8, 3) && GetLastError() == 87);
  CHECK(!GetFinalPathNameByHandleW((HANDLE)4, path, 8, 0x10) && GetLastError() == 87);
  CHECK(GetFinalPathNameByHandleW((HANDLE)4, path, 8, 8 | 4) == 0 && GetLastError() == 50);
}
static void *run(void *unused) {
  (void)unused;
  DWORD done = 0xfeed;
  unsigned char byte = 0;
  OVERLAPPED ov = {
      .offset = 0xfffffff0u, .offset_high = 1, .event = (HANDLE)12};
  SetLastError(0xabc);
  CHECK(ReadFile((HANDLE)4, &byte, 1, &done, &ov));
  CHECK(done == 1 && ov.internal == 0 && ov.internal_high == 1 && signals == 1);
  CHECK(io_flags == 0x10000 && io_low == 0xfffffff0u && io_high == 1);
  CHECK(ov.offset == 0xfffffff0u && ov.offset_high == 1 &&
        GetLastError() == 0xabc);
  injected_error = 33;
  CHECK(!WriteFile((HANDLE)4, &byte, 1, &done, &ov));
  CHECK(done == 0 && ov.internal == 0xc0070021u && ov.internal_high == 0 &&
        signals == 2 && GetLastError() == 33);
  injected_error = 0;
  CHECK(ReadFile((HANDLE)4, (void *)0x10000000, 1048593, &done, 0));
  CHECK(io_bytes == 1048593 && done == 1048593 && io_flags == 0);
  DWORD before = calls;
  done = 99;
  CHECK(!ReadFile((HANDLE)4, 0, 1, &done, 0) && done == 0 && calls == before &&
        GetLastError() == 87);
  source.attributes = 0x27;
  source.create_date = 0x5821;
  source.create_time = 0x1122;
  source.create_tenths = 123;
  source.access_date = 0x5823;
  source.write_date = 0x5822;
  source.write_time = 0x3344;
  CHECK(CopyFileA("source", "destination", 1));
  CHECK(closes == 2 && copied == 4 && !memcmp(input, output, 4));
  CHECK(destination.attributes == source.attributes &&
        destination.create_date == source.create_date &&
        destination.create_time == source.create_time &&
        destination.create_tenths == source.create_tenths &&
        destination.access_date == source.access_date &&
        destination.write_date == source.write_date &&
        destination.write_time == source.write_time);
  struct startup_a startup = {.size = sizeof(startup)};
  struct process_info process = {0};
  SECURITY_ATTRIBUTES security = {.length = sizeof(security), .inherit = 1};
  char command[] = "C:\\Program Files\\Tool.exe /a  /b", env[] = "A=B\0\0";
  expected_command = command;
  CHECK(CreateProcessA(0, command, &security, &security, 1, 0, env, 0, &startup,
                       &process));
  CHECK(spawn_calls == 2 && process.process == (HANDLE)16 &&
        process.thread == (HANDLE)20 && process.pid == 7 && process.tid == 8);
  CHECK(!strcmp(command, "C:\\Program Files\\Tool.exe /a  /b"));
  process.pid = 0xfeed;
  spawn_calls = 0;
  spawn_failure = 5;
  CHECK(!CreateProcessA(0, command, &security, &security, 1, 0, env, 0,
                        &startup, &process));
  CHECK(spawn_calls == 1 && GetLastError() == 5 && process.pid == 0xfeed);
  spawn_calls=0;spawn_failure=0;
  WCHAR wide_env[]={'A','=',0x20ac,0,0};
  expected_environment="A=\x80\0\0";expected_environment_bytes=5;
  CHECK(CreateProcessA(0,command,&security,&security,1,0x400,wide_env,0,&startup,&process));
  CHECK(spawn_calls==2&&wide_env[2]==0x20ac);
  wide_env[2]=0x2603;spawn_calls=0;process.pid=0xfeed;
  CHECK(!CreateProcessA(0,command,&security,&security,1,0x400,wide_env,0,&startup,&process));
  CHECK(GetLastError()==1113&&!spawn_calls&&process.pid==0xfeed);
  injected_error=0;
  CHECK(LockFile((HANDLE)4,0x1234,2,8,1)&&io_low==0x1234&&io_high==2&&io_bytes==8&&io_flags==3);
  OVERLAPPED lock={.internal=0xfeed,.offset=0x1234,.offset_high=2};
  CHECK(!LockFileEx((HANDLE)4,3,1,8,1,&lock)&&GetLastError()==87&&lock.internal==0xfeed);
  injected_error=33;
  CHECK(!LockFileEx((HANDLE)4,3,0,8,1,&lock)&&GetLastError()==33&&lock.internal==0xfeed);
  injected_error=0;
  CHECK(UnlockFileEx((HANDLE)4,0,8,1,&lock)&&!lock.internal);
  upper_files();
  return 0;
}
int main(void) {
  const size_t bytes = 8 * 1024 * 1024;
  void *stack = mmap(0, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(stack != MAP_FAILED && (uintptr_t)stack + bytes <= UINT32_MAX);
  pthread_attr_t attr;
  pthread_t thread;
  CHECK(!pthread_attr_init(&attr) &&
        !pthread_attr_setstack(&attr, stack, bytes));
  CHECK(!pthread_create(&thread, &attr, run, 0) && !pthread_join(thread, 0));
  CHECK(!pthread_attr_destroy(&attr) && !munmap(stack, bytes));
  puts("PASS file wrappers offsets counts completion errors copy metadata "
       "process ambiguity");
  puts("PASS file wrappers f5-20A attributes handle information size pointer device final path");
  return 0;
}
