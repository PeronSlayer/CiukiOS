/* Win32 communications on retained native COM descriptions.
 * Microsoft communications APIs / CiukiOS COM v1. LGPL-2.1-or-later. */
#include <w32/kernel32.h>
static BOOL control(HANDLE h,DWORD op,void *p,DWORD n,DWORD *outputs) {
  struct ciuki_w32_file_args a={.action=W32_F_COMM,.handle=K32_PTR(h),
      .flags=op,.buffer=K32_PTR(p),.bytes=n};
  if(!k32_file(&a)) return 0;
  if(outputs) *outputs=a.high;
  return 1;
}
static BOOL parameter(void) {SetLastError(87);return 0;}
BOOL W32API GetCommState(HANDLE h,void *dcb) {
  if(!dcb) return parameter();
  struct ciuki_com_line l={.version=1,.size=sizeof(l)};DWORD outputs;
  if(!control(h,COM_GET_LINE,&l,sizeof(l),&outputs)) return 0;
  uint8_t *p=dcb;
  if(k32_u32(p)!=28) return parameter();
  w32_fill(p,0,28);w32_put32(p,28);w32_put32(p+4,l.baud);
  w32_put32(p+8,1u | (l.parity ? 2u : 0) |
      ((outputs&CIUKI_COM_DTR) ? 0x10u : 0) | ((outputs&CIUKI_COM_RTS) ? 0x1000u : 0));
  p[18]=(uint8_t)l.data_bits;p[19]=(uint8_t)l.parity;p[20]=l.stop_bits==2 ? 2 : 0;
  p[21]=17;p[22]=19;return 1;
}
BOOL W32API SetCommState(HANDLE h,const void *dcb) {
  if(!dcb) return parameter();
  const uint8_t *p=dcb;DWORD f=k32_u32(p+8);
  if(k32_u32(p)!=28 || p[18]<5 || p[18]>8 || p[19]>4 || p[20]>2 || p[21]==p[22] || !(f&1)) return parameter();
  /* UART ABI v1 cannot express handshaking, 1.5 stop bits or XON/XOFF.
   * Refuse these settings instead of silently claiming to have applied them. */
  if((f&~0x1013u) || ((f>>4)&3)>1 || ((f>>12)&3)>1 || p[20]==1) {SetLastError(50);return 0;}
  struct ciuki_com_line l={.version=1,.size=sizeof(l)};
  if(!control(h,COM_GET_LINE,&l,sizeof(l),0)) return 0;
  l.baud=k32_u32(p+4);l.data_bits=p[18];l.parity=(f&2) ? p[19] : 0;l.stop_bits=p[20]==2 ? 2 : 1;
  if(!control(h,COM_SET_LINE,&l,sizeof(l),0)) return 0;
  struct ciuki_com_modem m={.version=1,.size=sizeof(m),
      .outputs=((f&0x10) ? CIUKI_COM_DTR : 0)|((f&0x1000) ? CIUKI_COM_RTS : 0)};
  return control(h,COM_SET_MODEM,&m,sizeof(m),0);
}
BOOL W32API GetCommTimeouts(HANDLE h,void *timeouts) {
  return timeouts ? control(h,1,timeouts,20,0) : parameter();
}
BOOL W32API SetCommTimeouts(HANDLE h,const void *timeouts) {
  if(!timeouts) return parameter();
  const uint8_t *p=timeouts;
  DWORD interval=k32_u32(p),read=k32_u32(p+8),write=k32_u32(p+16);
  if(interval==~0u && k32_u32(p+4)==~0u && read==~0u) return parameter();
  if(k32_u32(p+4) || k32_u32(p+12) || (interval && interval!=UINT32_MAX) ||
      (!interval && !read) || read>60000 || write>60000) {SetLastError(50);return 0;}
  struct ciuki_com_line l={.version=1,.size=sizeof(l)};
  if(!control(h,COM_GET_LINE,&l,sizeof(l),0)) return 0;
  l.read_timeout_ms=interval==~0u && !read ? 0 : read;
  l.write_timeout_ms=write;
  if(!control(h,COM_SET_LINE,&l,sizeof(l),0)) return 0;
  return control(h,2,(void *)timeouts,20,0);
}
BOOL W32API SetupComm(HANDLE h,DWORD in,DWORD out) {
  (void)in;(void)out;return control(h,5,0,0,0);
}
BOOL W32API ClearCommError(HANDLE h,DWORD *errors,void *stat) {
  struct ciuki_com_status s={.version=1,.size=sizeof(s)};
  if(!control(h,COM_CLEAR_ERRORS,&s,sizeof(s),0)) return 0;
  if(errors) *errors=((s.errors&2) ? 2u : 0)|((s.errors&4) ? 4u : 0)|
      ((s.errors&8) ? 8u : 0)|((s.errors&16) ? 16u : 0);
  if(stat) {
    w32_put32(stat,0);w32_put32((uint8_t *)stat+4,s.rx_queued);w32_put32((uint8_t *)stat+8,s.tx_queued);
  }
  return 1;
}
BOOL W32API GetCommMask(HANDLE h,DWORD *mask) {
  return mask ? control(h,3,mask,4,0) : parameter();
}
BOOL W32API SetCommMask(HANDLE h,DWORD mask) {
  if(mask&~0x1fffu) return parameter();
  return control(h,4,&mask,4,0);
}
BOOL W32API GetCommModemStatus(HANDLE h,DWORD *status) {
  if(!status) return parameter();
  struct ciuki_com_status s={.version=1,.size=sizeof(s)};
  if(!control(h,COM_GET_STATUS,&s,sizeof(s),0)) return 0;
  *status=s.modem&0xf0u;return 1;
}
BOOL W32API EscapeCommFunction(HANDLE h,DWORD action) {
  struct ciuki_com_line l={.version=1,.size=sizeof(l)};DWORD outputs;
  if(action<1 || action>9 || action==7) return parameter();
  if(action==1 || action==2) {SetLastError(50);return 0;}
  if(!control(h,COM_GET_LINE,&l,sizeof(l),&outputs)) return 0;
  DWORD bit=action==3 || action==4 ? CIUKI_COM_RTS : action==5 || action==6 ? CIUKI_COM_DTR : CIUKI_COM_BREAK;
  struct ciuki_com_modem m={.version=1,.size=sizeof(m),.outputs=outputs};
  if(action==3 || action==5 || action==8) m.outputs|=bit;else m.outputs&=~bit;
  return control(h,COM_SET_MODEM,&m,sizeof(m),0);
}
BOOL W32API SetCommBreak(HANDLE h) {return EscapeCommFunction(h,8);}
BOOL W32API ClearCommBreak(HANDLE h) {return EscapeCommFunction(h,9);}
