/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
struct process_entry {
  DWORD size, usage, pid;
  uintptr_t heap;
  DWORD module, threads, parent;
  LONG priority;
  DWORD flags;
  char exe[260];
};
struct thread_entry {
  DWORD size, usage, tid, owner;
  LONG priority, delta;
  DWORD flags;
};
struct module_entry {
  DWORD size, module, pid, global_usage, process_usage;
  void *base;
  DWORD bytes;
  HANDLE handle;
  char name[256], path[260];
};
static BOOL snapshot(struct ciuki_w32_snapshot_args *a) {
  a->header = K32_HEADER(W32_SNAPSHOT, *a);
  if (!k32_status(w32_ciuki_call(a, sizeof(*a))))
    return 0;
  if (a->error) {
    SetLastError(a->error);
    return 0;
  }
  return 1;
}
HANDLE W32API CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
  struct ciuki_w32_snapshot_args a = {.action = 1, .flags = flags, .pid = pid};
  return snapshot(&a) ? (HANDLE)(uintptr_t)a.result : K32_INVALID;
}
static int entry(HANDLE h, DWORD kind, BOOL first,
                 struct ciuki_w32_snapshot_entry *e) {
  struct ciuki_w32_snapshot_entry *copy = k32_alloc(sizeof(*copy));
  if (!copy) {
    SetLastError(8);
    return 0;
  }
  struct ciuki_w32_snapshot_args a = {.action = first ? 2 : 3,
                                      .flags = kind,
                                      .handle = K32_PTR(h),
                                      .buffer = K32_PTR(copy),
                                      .bytes = sizeof(*copy)};
  int ok = snapshot(&a);
  if (ok)
    *e = *copy;
  k32_free(copy, sizeof(*copy));
  return ok;
}
static BOOL process(HANDLE h, struct process_entry *out, BOOL first) {
  if (!out || out->size < sizeof(*out)) {
    SetLastError(24);
    return 0;
  }
  struct ciuki_w32_snapshot_entry e;
  if (!entry(h, 0, first, &e))
    return 0;
  struct process_entry p = {.size = sizeof(p),
                            .pid = e.id,
                            .threads = e.threads,
                            .parent = e.parent,
                            .priority = (LONG)e.priority};
  w32_copy(p.exe, e.name, 260);
  *out = p;
  return 1;
}
BOOL W32API Process32First(HANDLE h, struct process_entry *p) {
  return process(h, p, 1);
}
BOOL W32API Process32Next(HANDLE h, struct process_entry *p) {
  return process(h, p, 0);
}
BOOL W32API Process32FirstA(HANDLE h, struct process_entry *p) {
  return process(h, p, 1);
}
BOOL W32API Process32NextA(HANDLE h, struct process_entry *p) {
  return process(h, p, 0);
}
static BOOL thread(HANDLE h, struct thread_entry *out, BOOL first) {
  if (!out || out->size < sizeof(*out)) {
    SetLastError(24);
    return 0;
  }
  struct ciuki_w32_snapshot_entry e;
  if (!entry(h, 1, first, &e))
    return 0;
  *out = (struct thread_entry){.size = sizeof(*out),
                               .tid = e.id,
                               .owner = e.owner,
                               .priority = (LONG)e.priority};
  return 1;
}
BOOL W32API Thread32First(HANDLE h, struct thread_entry *p) {
  return thread(h, p, 1);
}
BOOL W32API Thread32Next(HANDLE h, struct thread_entry *p) {
  return thread(h, p, 0);
}
static BOOL module(HANDLE h, struct module_entry *out, BOOL first) {
  if (!out || out->size < sizeof(*out)) {
    SetLastError(24);
    return 0;
  }
  struct ciuki_w32_snapshot_entry e;
  if (!entry(h, 2, first, &e))
    return 0;
  struct module_entry m = {.size = sizeof(m),
                           .module = e.id,
                           .pid = e.owner,
                           .global_usage = 1,
                           .process_usage = 1,
                           .base = (void *)(uintptr_t)e.base,
                           .bytes = e.bytes,
                           .handle = (void *)(uintptr_t)e.base};
  w32_copy(m.name, e.name, 256);
  w32_copy(m.path, e.path, 260);
  *out = m;
  return 1;
}
BOOL W32API Module32First(HANDLE h, struct module_entry *p) {
  return module(h, p, 1);
}
BOOL W32API Module32Next(HANDLE h, struct module_entry *p) {
  return module(h, p, 0);
}
BOOL W32API Module32FirstA(HANDLE h, struct module_entry *p) {
  return module(h, p, 1);
}
BOOL W32API Module32NextA(HANDLE h, struct module_entry *p) {
  return module(h, p, 0);
}

#ifndef W32_HOST_MODEL
_Static_assert(sizeof(struct process_entry) == 296, "PROCESSENTRY32A");
_Static_assert(sizeof(struct thread_entry) == 28, "THREADENTRY32");
_Static_assert(sizeof(struct module_entry) == 548, "MODULEENTRY32A");
#endif
