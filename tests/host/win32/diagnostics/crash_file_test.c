#define _POSIX_C_SOURCE 200809L
#include <ciuki/w32_diagnostics.h>
#include "../../../../apps/desktop/win32_crash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
    if(argc!=2&&argc!=3)return 1;
    struct ciuki_w32_fault f={.kind=W32_REPORT_CRASH,.code=0xc0000005,.eip=0x401234,.offset=0x1234,.valid=0x81};
    strcpy(f.exe,"fixture.exe");strcpy(f.module,"fixture.dll");f.stack[0]=0x12345678;f.stack[7]=0x87654321;
    for(unsigned i=0;i<12;i++){f.registers[0]=i;if(desk_w32_crash_store(argv[1],&f))return 2;}
    puts("PASS crash text rotation");
    if(argc==3){
        /* G13: native fault records share the rotation and the first line. */
        struct ciuki_w32_fault n={.kind=W32_REPORT_NATIVE_FAULT,.pid=7,.tid=9,.code=14u|6u<<8,.eip=0x401234,.offset=0x10,.valid=0x03};
        strcpy(n.exe,"crasher");n.registers[7]=0xbffffff0;n.stack[0]=0xdeadbeef;n.stack[1]=0x01020304;
        for(unsigned i=0;i<10;i++){n.pid=100+i;if(desk_w32_crash_store(argv[2],&n))return 3;}
        struct ciuki_w32_fault bad=n;bad.kind=W32_REPORT_REFUSE;if(!desk_w32_crash_store(argv[2],&bad))return 4;
        puts("PASS native crash text rotation");
    }
    return 0;
}
