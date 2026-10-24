/* G24: NLS snapshots the same bounded settings file as native setlocale.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef W32_HOST_MODEL
void k32_regional_host(struct regional *) __attribute__((weak));
static void k32_regional(struct regional *r)
{ reg_defaults(r,0); if(k32_regional_host) k32_regional_host(r); reg_sanitize(r); }
#else
static void k32_regional(struct regional *r)
{
  reg_defaults(r,0);
  DWORD saved=GetLastError();
  /* C is the system volume even when a program changes its current drive. */
  HANDLE file=CreateFileA("C:\\system\\settings\\desktop.cfg",0x80000000u,7,0,3,0,0);
  if(file!=K32_INVALID) {
    char bytes[256],line[160]; unsigned used=0,total=0; int long_line=0,ok=1;
    for(;;) {
      DWORD got=0;
      if(!ReadFile(file,bytes,sizeof(bytes),&got,0)) { ok=0; break; }
      if(!got) break;
      total+=got; if(total>32768) { ok=0; break; }
      for(DWORD i=0;i<got;i++) {
        if(bytes[i]=='\n') {
          if(!long_line) { line[used]=0; reg_line(r,line); }
          used=0; long_line=0;
        } else if(used+1<sizeof(line)) line[used++]=bytes[i]; else long_line=1;
      }
    }
    if(ok && used && !long_line) { line[used]=0; reg_line(r,line); }
    if(!ok) reg_defaults(r,0);
    CloseHandle(file);
  }
  reg_sanitize(r); SetLastError(saved);
}
#endif
