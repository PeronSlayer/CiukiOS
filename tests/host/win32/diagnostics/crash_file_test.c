#define _POSIX_C_SOURCE 200809L
#include <ciuki/w32_diagnostics.h>
#include "../../../../apps/desktop/win32_crash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
    if(argc!=2)return 1;
    struct ciuki_w32_fault f={.kind=W32_REPORT_CRASH,.code=0xc0000005,.eip=0x401234,.offset=0x1234,.valid=0x81};
    strcpy(f.exe,"fixture.exe");strcpy(f.module,"fixture.dll");f.stack[0]=0x12345678;f.stack[7]=0x87654321;
    for(unsigned i=0;i<12;i++){f.registers[0]=i;if(desk_w32_crash_store(argv[1],&f))return 2;}
    puts("PASS crash text rotation");return 0;
}
