#define _POSIX_C_SOURCE 200809L
#include <ciuki/w32_diagnostics.h>
#include "../../../../apps/desktop/win32_crash.h"
#include <unistd.h> /* sec-sysvc2 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* sec-sysvc: the same rotation over the SYSTEM log service's user scope
 * (an in-memory fake of ciuki_system_log_read/write as the desktop uses it). */
static char logs[16][32],texts[16][2048];static size_t sizes[16];static unsigned log_count,replaces;
static int fake_head(void *ctx,const char *name,char *out,size_t cap,size_t *got)
{
    (void)ctx;for(unsigned i=0;i<log_count;i++)if(!strcmp(logs[i],name)){*got=sizes[i]<cap?sizes[i]:cap;memcpy(out,texts[i],*got);return 0;}
    return -2;
}
static int fake_replace(void *ctx,const char *name,const char *text,size_t n)
{
    (void)ctx;if(n>=sizeof(texts[0]))return -27;replaces++;
    unsigned i=0;while(i<log_count&&strcmp(logs[i],name))i++;
    if(i==log_count){if(log_count==16)return -28;strcpy(logs[log_count++],name);}
    memcpy(texts[i],text,n);sizes[i]=n;return 0;
}
static int user_logs(void)
{
    struct desk_crash_io io={NULL,fake_head,fake_replace};
    struct ciuki_w32_fault f={.kind=W32_REPORT_CRASH,.code=0xc0000005,.eip=0x401234,.valid=1};strcpy(f.exe,"game.exe");
    for(unsigned i=0;i<10;i++){f.pid=200+i;if(desk_w32_crash_write(&io,"WCRASH",&f))return 5;}
    if(log_count!=8||replaces!=10)return 6;
    unsigned long long seen=0;
    for(unsigned i=0;i<8;i++){
        unsigned long long sequence=0;char expected[16];snprintf(expected,sizeof(expected),"WCRASH%u.TXT",i);
        if(strcmp(logs[i],expected)||sscanf(texts[i],"CiukiOS crash sequence=%llu",&sequence)!=1||sequence<3||sequence>10)return 7;
        seen|=1ull<<sequence;
    }
    if(seen!=0x7f8ull)return 8;
    if(desk_w32_crash_log(&f)!=-ENOSYS)return 9; /* host: no log service */
    puts("PASS user crash logs: WCRASHn.TXT rotation through the log service interface");
    return 0;
}
int main(int argc,char **argv){
    {int r=user_logs();if(r)return 10+r;}
    if(argc!=2&&argc!=3)return 1;
    struct ciuki_w32_fault f={.kind=W32_REPORT_CRASH,.code=0xc0000005,.eip=0x401234,.offset=0x1234,.valid=0x81};
    strcpy(f.exe,"fixture.exe");strcpy(f.module,"fixture.dll");f.stack[0]=0x12345678;f.stack[7]=0x87654321;
    for(unsigned i=0;i<12;i++){f.registers[0]=i;if(desk_w32_crash_store(argv[1],&f))return 2;}
    puts("PASS crash text rotation");
    {   /* sec-sysvc2: a profile's own log folder without the log service uses
         * the service's names (WCRASHn.TXT for Win32); a sibling folder,
         * removed again, keeps argv[1]'s eight files. */
        char dir[600],file[700];snprintf(dir,sizeof(dir),"%s.wcrash",argv[1]);
        for(unsigned i=0;i<3;i++)if(desk_w32_crash_store_as(dir,"WCRASH",&f))return 20;
        for(unsigned i=0;i<4;i++){
            snprintf(file,sizeof(file),"%s/WCRASH%u.TXT",dir,i);int present=!access(file,F_OK);
            if(present!=(i<3))return 21;
            if(present)unlink(file);
        }
        snprintf(file,sizeof(file),"%s/CRASH0.TXT",dir);if(!access(file,F_OK))return 22;
        if(rmdir(dir))return 23;
        if(desk_w32_crash_store_as(dir,NULL,&f)!=-EINVAL)return 24;
        puts("PASS profile log folder without the log service: WCRASHn.TXT");
    }
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
