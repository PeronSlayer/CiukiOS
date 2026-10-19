/* Loader-complete offline facade. No sockets or network queues are created.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ntdll.h>
enum offline_api {OFF_SOCKET,OFF_CLOSE,OFF_BIND,OFF_CONNECT,OFF_PEER,OFF_NAME,OFF_OPTION,OFF_IOCTL,OFF_RECV,OFF_SEND,OFF_RECVFROM,OFF_SENDTO,OFF_HOSTBYNAME,OFF_HOSTNAME,OFF_START,OFF_CLEAN};
static void offline_record(unsigned api){
  static volatile ULONG once[16];
  static const char *const names[]={"socket","closesocket","bind","connect","getpeername","getsockname","setsockopt","ioctlsocket","recv","send","recvfrom","sendto","gethostbyname","gethostname","WSAStartup","WSACleanup"};
  if(!__atomic_exchange_n(&once[api],1,__ATOMIC_RELAXED))w32_diag("stub","wsock32.dll",names[api]);
}
static int unavailable(unsigned api) {
  offline_record(api);
  NtCurrentTeb()->CoreSocketError = 10050;
  return -1;
}
int W32API WSAGetLastError(void) {
  return (int)NtCurrentTeb()->CoreSocketError;
}
void W32API WSASetLastError(int n) {
  NtCurrentTeb()->CoreSocketError = (ULONG)n;
}
int W32API WSAStartup(uint16_t version, void *data) {
  (void)version;
  (void)data;
  offline_record(OFF_START);
  return 10091;
}
int W32API WSACleanup(void) {
  offline_record(OFF_CLEAN);
  NtCurrentTeb()->CoreSocketError = 10093;
  return -1;
}
uintptr_t W32API socket(int af, int type, int protocol) {
  (void)af;
  (void)type;
  (void)protocol;
  return (uintptr_t)unavailable(OFF_SOCKET);
}
int W32API closesocket(uintptr_t s) {
  (void)s;
  return unavailable(OFF_CLOSE);
}
int W32API bind(uintptr_t s, const void *address, int n) {
  (void)s;
  (void)address;
  (void)n;
  return unavailable(OFF_BIND);
}
int W32API connect(uintptr_t s, const void *address, int n) {
  (void)s;
  (void)address;
  (void)n;
  return unavailable(OFF_CONNECT);
}
int W32API getpeername(uintptr_t s, void *address, int *n) {
  (void)s;
  (void)address;
  (void)n;
  return unavailable(OFF_PEER);
}
int W32API getsockname(uintptr_t s, void *address, int *n) {
  (void)s;
  (void)address;
  (void)n;
  return unavailable(OFF_NAME);
}
int W32API setsockopt(uintptr_t s, int level, int option, const char *value,
                      int n) {
  (void)s;
  (void)level;
  (void)option;
  (void)value;
  (void)n;
  return unavailable(OFF_OPTION);
}
int W32API ioctlsocket(uintptr_t s, int cmd, ULONG *arg) {
  (void)s;
  (void)cmd;
  (void)arg;
  return unavailable(OFF_IOCTL);
}
int W32API recv(uintptr_t s, char *data, int n, int flags) {
  (void)s;
  (void)data;
  (void)n;
  (void)flags;
  return unavailable(OFF_RECV);
}
int W32API send(uintptr_t s, const char *data, int n, int flags) {
  (void)s;
  (void)data;
  (void)n;
  (void)flags;
  return unavailable(OFF_SEND);
}
int W32API recvfrom(uintptr_t s, char *data, int n, int flags, void *address,
                    int *len) {
  (void)s;
  (void)data;
  (void)n;
  (void)flags;
  (void)address;
  (void)len;
  return unavailable(OFF_RECVFROM);
}
int W32API sendto(uintptr_t s, const char *data, int n, int flags,
                  const void *address, int len) {
  (void)s;
  (void)data;
  (void)n;
  (void)flags;
  (void)address;
  (void)len;
  return unavailable(OFF_SENDTO);
}
void *W32API gethostbyname(const char *name) {
  (void)name;
  unavailable(OFF_HOSTBYNAME);
  return 0;
}
int W32API gethostname(char *out, int n) {
  (void)out;
  (void)n;
  return unavailable(OFF_HOSTNAME);
}
uint16_t W32API htons(uint16_t n) { return (uint16_t)((n << 8) | (n >> 8)); }
uint16_t W32API ntohs(uint16_t n) { return htons(n); }
ULONG W32API inet_addr(const char *s) {
  if (!s)
    return ~0u;
  ULONG result = 0;
  for (unsigned i = 0; i < 4; i++) {
    unsigned n = 0, digits = 0;
    while (*s >= '0' && *s <= '9') {
      n = n * 10 + (unsigned)(*s++ - '0');
      if (n > 255)
        return ~0u;
      digits++;
    }
    if (!digits)
      return ~0u;
    result |= n << (i * 8);
    if (i < 3) {
      if (*s++ != '.')
        return ~0u;
    } else if (*s)
      return ~0u;
  }
  return result;
}
