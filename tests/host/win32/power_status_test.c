/* G11 GetSystemPowerStatus (docs/design/power-policy.md 24) over the Win32
 * system action, against sdk/win32/kernel32/time.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/kernel32.h>
#include <w32/shims.h>
static DWORD last,ac,flag,percent,seconds,full,error,calls;
static int32_t transport;
static struct ciuki_w32_compat shims;
const struct ciuki_w32_compat *w32_shim_current(void){return &shims;}
static struct w32_teb teb;
_Thread_local struct w32_teb *w32_model_teb=&teb;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"power status %u: %s\n",__LINE__,#c);exit(1); } } while (0)
DWORD W32API GetLastError(void){return last;}
void W32API SetLastError(DWORD n){last=n;}
int k32_status(int32_t s){if(s)SetLastError(31);return s==0;}
void *w32_copy(void *a,const void *b,ULONG n){return memmove(a,b,n);}
void *w32_fill(void *a,uint8_t b,ULONG n){return memset(a,b,n);}
ULONG w32_strlen(const char *s){return s?(ULONG)strlen(s):0;}
void w32_diag(const char *e,const char *m,const char *n){(void)e;(void)m;(void)n;}
int32_t w32_ciuki_call(void *record,uint32_t n) {
  CHECK(n==sizeof(struct ciuki_w32_system_args));struct ciuki_w32_system_args *a=record;
  CHECK(a->header.operation==W32_SYSTEM&&a->action==W32_S_POWER);calls++;
  a->low=ac;a->high=flag;a->value=percent;a->extra=seconds;a->result=full;a->error=error;return transport;
}
int main(void) {
  CHECK(sizeof(SYSTEM_POWER_STATUS)==12);
  SYSTEM_POWER_STATUS s;
  CHECK(!GetSystemPowerStatus(NULL)&&GetLastError()==87&&!calls);
  /* T23 on battery: APM unit, 78 %, 1 h 25 min. */
  ac=0;flag=1;percent=78;seconds=5100;full=0xffffffffu;memset(&s,0xa5,sizeof(s));
  CHECK(GetSystemPowerStatus(&s)&&s.ac_line_status==0&&s.battery_flag==1&&s.battery_life_percent==78&&!s.reserved1);
  CHECK(s.battery_life_time==5100&&s.battery_full_life_time==0xffffffffu);
  /* Desktop on mains, no battery; unknown everything. */
  ac=1;flag=128;percent=255;seconds=0xffffffffu;CHECK(GetSystemPowerStatus(&s)&&s.ac_line_status==1&&s.battery_flag==128&&s.battery_life_percent==255&&s.battery_life_time==0xffffffffu);
  ac=255;flag=255;percent=101;CHECK(GetSystemPowerStatus(&s)&&s.ac_line_status==255&&s.battery_flag==255&&s.battery_life_percent==255);
  ac=999;flag=999;CHECK(GetSystemPowerStatus(&s)&&s.ac_line_status==255&&s.battery_flag==255);
  error=50;memset(&s,0x5a,sizeof(s));SYSTEM_POWER_STATUS saved=s;CHECK(!GetSystemPowerStatus(&s)&&GetLastError()==50&&!memcmp(&s,&saved,sizeof(s)));
  error=0;transport=-1;CHECK(!GetSystemPowerStatus(&s)&&GetLastError()==31);
  puts("PASS GetSystemPowerStatus: SYSTEM_POWER_STATUS layout, battery and mains, unknown values, errors");return 0;
}
