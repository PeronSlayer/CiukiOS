/* SPDX-License-Identifier: MIT */
#include <w32/base.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t error,hits;
void w32r_last_error(uint32_t e){error=e;}
uint32_t w32_shim_stub(volatile uint32_t *once,const char *dll,const char *name,uint32_t result){
    (void)once;assert(!strcmp(dll,"advapi32.dll") && !strcmp(name,"SetFileSecurityA") && !result);error=120;hits++;return 0;
}
extern int W32API SetFileSecurityA(const char *,uint32_t,void *);
int main(void){
    uint8_t descriptor[20];memset(descriptor,0x5a,sizeof(descriptor));
    assert(!SetFileSecurityA("C:\\payload.txt",4,descriptor) && error==120 && hits==1);
    for(unsigned i=0;i<sizeof(descriptor);i++)assert(descriptor[i]==0x5a);
    assert(!SetFileSecurityA(NULL,~0u,(void *)(uintptr_t)1) && error==120);
    puts("PASS SetFileSecurityA typed W21 refusal, untouched descriptor, no invalid pointer access");return 0;
}
