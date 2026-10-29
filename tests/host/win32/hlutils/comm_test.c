/* Native transport fixture; compile the real facade without pointer narrowing.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include <assert.h>
#include <stdio.h>
static const void *pointers[128];static unsigned np;
static DWORD pointer(const void *p) {assert(np<128);pointers[++np]=p;return np;}
#undef K32_PTR
#define K32_PTR(p) pointer(p)
#include "../../../../sdk/win32/kernel32/comm.c"
static DWORD error,outputs=3,mask,timeout[5];
static struct ciuki_com_line line={.version=1,.size=40,.baud=9600,.data_bits=8,.stop_bits=1};
static int fail;
void W32API SetLastError(DWORD e) {error=e;}
int k32_file(struct ciuki_w32_file_args *a) {
  assert(a->action==W32_F_COMM);assert(pointers[a->handle]==(void *)0x1234);
  if(fail) {error=170;return 0;}
  void *p=(void *)pointers[a->buffer];
  switch(a->flags) {
  case COM_GET_LINE:assert(a->bytes==40);w32_copy(p,&line,40);break;
  case COM_SET_LINE:assert(a->bytes==40);w32_copy(&line,p,40);break;
  case COM_SET_MODEM:outputs=((struct ciuki_com_modem *)p)->outputs;break;
  case COM_GET_STATUS: case COM_CLEAR_ERRORS: {
    struct ciuki_com_status *s=p;assert(a->bytes==sizeof(*s));
    s->rx_queued=7;s->tx_queued=9;s->errors=14;s->modem=0xb0;break;
  }
  case 1:w32_copy(p,timeout,20);break;
  case 2:w32_copy(timeout,p,20);break;
  case 3:w32_put32(p,mask);break;
  case 4:mask=w32_u32(p);break;
  case 5:assert(!a->bytes);break;
  default:assert(0);
  }
  a->high=outputs;return 1;
}
int main(void) {
  HANDLE h=(void *)0x1234;uint8_t dcb[28]={0};w32_put32(dcb,28);
  assert(GetCommState(h,dcb) && w32_u32(dcb+4)==9600 && dcb[18]==8 && dcb[21]==17);
  w32_put32(dcb+4,19200);dcb[19]=2;w32_put32(dcb+8,0x1013);
  assert(SetCommState(h,dcb) && line.baud==19200 && line.parity==2);
  dcb[22]=dcb[21];assert(!SetCommState(h,dcb) && error==87);dcb[22]=19;
  w32_put32(dcb+8,0x1113);assert(!SetCommState(h,dcb) && error==50);w32_put32(dcb+8,0x1013);
  assert(EscapeCommFunction(h,4) && outputs==1);
  assert(SetCommBreak(h) && outputs==5 && ClearCommBreak(h) && outputs==1);
  uint32_t t[5]={~0u,0,25,0,50};assert(SetCommTimeouts(h,t));
  uint32_t got[5]={0};assert(GetCommTimeouts(h,got) && got[2]==25 && got[4]==50);
  assert(line.read_timeout_ms==25 && line.write_timeout_ms==50);
  assert(SetCommMask(h,0x19) && GetCommMask(h,&mask) && mask==0x19);
  assert(SetupComm(h,4096,4096));uint32_t errors,stat[3];
  assert(ClearCommError(h,&errors,stat) && errors==14 && stat[1]==7 && stat[2]==9);
  uint32_t modem;assert(GetCommModemStatus(h,&modem) && modem==0xb0);
  fail=1;assert(!GetCommState(h,dcb) && error==170);
  puts("PASS UART state, framing, modem, timeouts, masks, queues, invalid and busy paths");
}
