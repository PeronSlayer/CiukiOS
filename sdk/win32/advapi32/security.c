/* Security data structures, hardware profile, random bytes and the typed
 * W21 service/NT-security/MSI failures of ADVAPI32 for the reused Wine
 * 10.0 DLLs (f5-20A). SIDs, ACLs and security descriptors are built in the
 * NT layout; FAT objects enforce none of them (decision
 * core-security-structures). Windows 9x has no Service Control Manager,
 * no tokens and no MSI: those calls fail with ERROR_CALL_NOT_IMPLEMENTED.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "internal.h"
#ifdef W32_HOST_MODEL
#define IMPORT
#else
#define IMPORT __declspec(dllimport)
#endif
IMPORT uint32_t w32_shim_stub(volatile uint32_t *, const char *, const char *, uint32_t);
IMPORT void w32_report_refusal(uint32_t, const char *, uint32_t);
static int fail(uint32_t e) {
  w32r_last_error(e);
  return 0;
}
/* SID: revision 1, at most 15 subauthorities. */
static uint32_t sid_length(const uint8_t *sid) {
  return sid && sid[0] == 1 && sid[1] <= 15 ? 8u + 4u * sid[1] : 0;
}
uint32_t W32API GetLengthSid(const void *sid) {
  const uint8_t *s = sid;
  return s ? 8u + 4u * s[1] : 0;
}
/* ACL header: revision, sbz1, size, ACE count, sbz2. */
int W32API InitializeAcl(void *acl, uint32_t length, uint32_t revision) {
  if (!acl || (revision != 2 && revision != 4) || length > 0xffff)
    return fail(87);
  if (length < 8)
    return fail(122);
  uint8_t *a = acl;
  w32_fill(a, 0, 8);
  a[0] = (uint8_t)revision;
  w32_put16(a + 2, (uint16_t)(length & ~3u));
  return 1;
}
static int add_ace(void *acl, uint32_t revision, uint32_t flags, uint32_t mask, const void *sid,
                   uint8_t type) {
  uint8_t *a = acl;
  uint32_t length = sid_length(sid);
  if (!a || (a[0] != 2 && a[0] != 4) || w32_u16(a + 2) < 8)
    return fail(1336);
  if (revision != 2 && revision != 4)
    return fail(1336);
  if (!length)
    return fail(1337);
  if (flags & ~0x1fu)
    return fail(87);
  uint32_t size = w32_u16(a + 2), count = w32_u16(a + 4), at = 8;
  for (uint32_t i = 0; i < count; i++) {
    if (at + 4 > size || w32_u16(a + at + 2) < 4 || w32_u16(a + at + 2) > size - at)
      return fail(1336);
    at += w32_u16(a + at + 2);
  }
  uint32_t ace = 8 + length;
  if (at + ace > size)
    return fail(1344);
  a[at] = type;
  a[at + 1] = (uint8_t)flags;
  w32_put16(a + at + 2, (uint16_t)ace);
  w32_put32(a + at + 4, mask);
  w32_copy(a + at + 8, sid, length);
  w32_put16(a + 4, (uint16_t)(count + 1));
  if (revision > a[0])
    a[0] = (uint8_t)revision;
  return 1;
}
int W32API AddAccessAllowedAce(void *acl, uint32_t revision, uint32_t mask, const void *sid) {
  return add_ace(acl, revision, 0, mask, sid, 0);
}
int W32API AddAccessAllowedAceEx(void *acl, uint32_t revision, uint32_t flags, uint32_t mask,
                                 const void *sid) {
  return add_ace(acl, revision, flags, mask, sid, 0);
}
int W32API AddAccessDeniedAce(void *acl, uint32_t revision, uint32_t mask, const void *sid) {
  return add_ace(acl, revision, 0, mask, sid, 1);
}
int W32API AddAccessDeniedAceEx(void *acl, uint32_t revision, uint32_t flags, uint32_t mask,
                                const void *sid) {
  return add_ace(acl, revision, flags, mask, sid, 1);
}
/* Absolute descriptor: revision 1, sbz1, control, owner, group, SACL, DACL. */
struct descriptor {
  uint8_t revision, sbz1;
  uint16_t control;
  void *owner, *group, *sacl, *dacl;
};
int W32API InitializeSecurityDescriptor(void *sd, uint32_t revision) {
  if (!sd)
    return fail(87);
  if (revision != 1)
    return fail(1305);
  w32_fill(sd, 0, sizeof(struct descriptor));
  ((struct descriptor *)sd)->revision = 1;
  return 1;
}
static struct descriptor *absolute(void *sd) {
  struct descriptor *d = sd;
  if (!d) {
    fail(87);
    return 0;
  }
  if (d->revision != 1) {
    fail(1305);
    return 0;
  }
  if (d->control & 0x8000) {
    fail(1338);
    return 0;
  }
  return d;
}
/* SE_DACL_PRESENT 4, SE_DACL_DEFAULTED 8, SE_OWNER_DEFAULTED 1. */
int W32API SetSecurityDescriptorDacl(void *sd, int present, void *dacl, int defaulted) {
  struct descriptor *d = absolute(sd);
  if (!d)
    return 0;
  d->control = (uint16_t)((d->control & ~0xcu) | (present ? 4 : 0) |
                          (present && defaulted ? 8 : 0));
  d->dacl = present ? dacl : 0;
  return 1;
}
int W32API SetSecurityDescriptorOwner(void *sd, void *owner, int defaulted) {
  struct descriptor *d = absolute(sd);
  if (!d)
    return 0;
  d->control = (uint16_t)((d->control & ~1u) | (defaulted ? 1 : 0));
  d->owner = owner;
  return 1;
}
/* HW_PROFILE_INFOA: one undockable configuration, "Original Configuration"
 * (the Windows 98 default profile name), a fixed CiukiOS profile GUID. */
int W32API GetCurrentHwProfileA(void *info) {
  static const char guid[] = "{6B2E6E3C-0A6F-4B4E-9C1D-43494B494F53}";
  static const char name[] = "Original Configuration";
  uint8_t *p = info;
  if (!p)
    return fail(87);
  w32_fill(p, 0, 124);
  w32_put32(p, 4 | 2);
  w32_copy(p + 4, guid, sizeof(guid));
  w32_copy(p + 43, name, sizeof(name));
  return 1;
}
/* RtlGenRandom. No kernel entropy service exists yet: SplitMix64 over a
 * time-stamp-counter seed, unique per call and process, not suitable as a
 * cryptographic source. */
static volatile uint64_t random_state;
static uint64_t next_random(void) {
  uint64_t s = __atomic_add_fetch(&random_state, 0x9e3779b97f4a7c15ull, __ATOMIC_RELAXED);
  if (s == 0x9e3779b97f4a7c15ull) {
    uint64_t seed = __builtin_ia32_rdtsc() ^ (uint64_t)(uintptr_t)&s;
    __atomic_add_fetch(&random_state, seed, __ATOMIC_RELAXED);
    s += seed;
  }
  s = (s ^ (s >> 30)) * 0xbf58476d1ce4e5b9ull;
  s = (s ^ (s >> 27)) * 0x94d049bb133111ebull;
  return s ^ (s >> 31);
}
uint8_t W32API SystemFunction036(void *buffer, uint32_t length) {
  uint8_t *p = buffer;
  if (!p && length)
    return 0;
  while (length) {
    uint64_t r = next_random();
    for (unsigned i = 0; i < 8 && length; i++, length--)
      *p++ = (uint8_t)(r >> (i * 8));
  }
  return 1;
}
#define W21(name, convention)                                                  \
  static volatile uint32_t once;                                               \
  return w32_shim_stub(&once, "advapi32.dll", name, convention)
void *W32API OpenSCManagerW(const WCHAR *machine, const WCHAR *database, uint32_t access) {
  (void)machine;
  (void)database;
  (void)access;
  static volatile uint32_t once;
  return (void *)(uintptr_t)w32_shim_stub(&once, "advapi32.dll", "OpenSCManagerW", 0);
}
void *W32API OpenServiceW(void *manager, const WCHAR *name, uint32_t access) {
  (void)manager;
  (void)name;
  (void)access;
  static volatile uint32_t once;
  return (void *)(uintptr_t)w32_shim_stub(&once, "advapi32.dll", "OpenServiceW", 0);
}
/* A kernel or file-system driver service is the documented driver refusal
 * (ERROR_ACCESS_DENIED), as for CreateServiceA. */
void *W32API CreateServiceW(void *manager, const WCHAR *name, const WCHAR *display,
                            uint32_t access, uint32_t type, uint32_t start, uint32_t error,
                            const WCHAR *binary, const WCHAR *group, uint32_t *tag,
                            const WCHAR *dependencies, const WCHAR *user, const WCHAR *password) {
  (void)manager;
  (void)name;
  (void)display;
  (void)access;
  (void)start;
  (void)error;
  (void)binary;
  (void)group;
  (void)tag;
  (void)dependencies;
  (void)user;
  (void)password;
  static volatile uint32_t once;
  if (type & 3) {
    w32_report_refusal(2, "advapi32.dll", 5);
    return (void *)(uintptr_t)w32_shim_stub(&once, "advapi32.dll", "CreateServiceW", 5);
  }
  return (void *)(uintptr_t)w32_shim_stub(&once, "advapi32.dll", "CreateServiceW", 0);
}
int W32API ChangeServiceConfigW(void *service, uint32_t type, uint32_t start, uint32_t error,
                                const WCHAR *binary, const WCHAR *group, uint32_t *tag,
                                const WCHAR *dependencies, const WCHAR *user,
                                const WCHAR *password, const WCHAR *display) {
  (void)service;
  (void)type;
  (void)start;
  (void)error;
  (void)binary;
  (void)group;
  (void)tag;
  (void)dependencies;
  (void)user;
  (void)password;
  (void)display;
  W21("ChangeServiceConfigW", 0);
}
int W32API ChangeServiceConfig2W(void *service, uint32_t level, void *info) {
  (void)service;
  (void)level;
  (void)info;
  W21("ChangeServiceConfig2W", 0);
}
int W32API QueryServiceConfigW(void *service, void *config, uint32_t bytes, uint32_t *needed) {
  (void)service;
  (void)config;
  (void)bytes;
  (void)needed;
  W21("QueryServiceConfigW", 0);
}
int W32API QueryServiceStatusEx(void *service, uint32_t level, void *status, uint32_t bytes,
                                uint32_t *needed) {
  (void)service;
  (void)level;
  (void)status;
  (void)bytes;
  (void)needed;
  W21("QueryServiceStatusEx", 0);
}
int W32API StartServiceW(void *service, uint32_t count, const WCHAR **arguments) {
  (void)service;
  (void)count;
  (void)arguments;
  W21("StartServiceW", 0);
}
int W32API ConvertSidToStringSidW(const void *sid, WCHAR **text) {
  (void)sid;
  (void)text;
  W21("ConvertSidToStringSidW", 0);
}
int W32API CreateProcessAsUserW(void *token, const WCHAR *application, WCHAR *command,
                                void *process, void *thread, int inherit, uint32_t flags,
                                void *environment, const WCHAR *directory, void *startup,
                                void *information) {
  (void)token;
  (void)application;
  (void)command;
  (void)process;
  (void)thread;
  (void)inherit;
  (void)flags;
  (void)environment;
  (void)directory;
  (void)startup;
  (void)information;
  W21("CreateProcessAsUserW", 0);
}
int W32API GetFileSecurityW(const WCHAR *file, uint32_t information, void *sd, uint32_t bytes,
                            uint32_t *needed) {
  (void)file;
  (void)information;
  (void)sd;
  (void)bytes;
  (void)needed;
  W21("GetFileSecurityW", 0);
}
/* f5-20B (amendment 1, item 2): the ANSI form fails like the Unicode one;
 * Windows 98 keeps no file security on FAT. */
int W32API GetFileSecurityA(const char *file, uint32_t information, void *sd, uint32_t bytes,
                            uint32_t *needed) {
  (void)file;
  (void)information;
  (void)sd;
  (void)bytes;
  (void)needed;
  W21("GetFileSecurityA", 0);
}
int W32API SetFileSecurityW(const WCHAR *file, uint32_t information, void *sd) {
  (void)file;
  (void)information;
  (void)sd;
  W21("SetFileSecurityW", 0);
}
int W32API LookupPrivilegeValueW(const WCHAR *system, const WCHAR *name, void *luid) {
  (void)system;
  (void)name;
  (void)luid;
  W21("LookupPrivilegeValueW", 0);
}
/* MSI advertised shortcuts: W32_REFUSE_MSI, the UINT error result. */
uint32_t W32API CommandLineFromMsiDescriptor(const WCHAR *descriptor, WCHAR *command,
                                             uint32_t *length) {
  (void)descriptor;
  (void)command;
  (void)length;
  W21("CommandLineFromMsiDescriptor", 3);
}
