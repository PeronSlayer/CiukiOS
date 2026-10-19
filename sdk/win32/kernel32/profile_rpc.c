/* Global INI transaction lease; w32srv releases it on client death.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include <w32/server.h>
static DWORD request(DWORD op) {
  struct { struct w32r_request r; char empty[2]; } a={
      .r={.version=1,.op=op,.path_bytes=1,.name_bytes=1}};
  struct w32r_reply reply;
  DWORD used=0,e=w32_server_exchange(&a,sizeof(a.r)+2,&reply,sizeof(reply),&used);
  if (e) return e;
  if (used!=sizeof(reply) || reply.version!=1) return 13;
  return reply.error;
}
int k32_profile_begin(void) {
  DWORD e;
  while ((e=request(W32R_PROFILE_ACQUIRE))==170) Sleep(1);
  if (e) { SetLastError(e); return 0; }
  return 1;
}
void k32_profile_end(void) {
  DWORD old=GetLastError(),e=request(W32R_PROFILE_RELEASE);
  SetLastError(e ? e : old);
}
