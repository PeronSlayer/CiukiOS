/* Eight bounded text files; no dump or user buffers. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32_crash.h"
#include "fault_text.h"
int desk_w32_crash_store(const char *directory,const struct ciuki_w32_fault *f)
{
    if(!directory||!f||(f->kind!=W32_REPORT_CRASH&&f->kind!=W32_REPORT_NATIVE_FAULT))return -EINVAL;
    if(mkdir(directory,0777)&&errno!=EEXIST)return -errno;
    unsigned slot=0;unsigned long long oldest=~0ull,maximum=0;
    char path[300];
    for(unsigned i=0;i<8;i++){
        int n=snprintf(path,sizeof(path),"%s/CRASH%u.TXT",directory,i);if(n<0||n>=(int)sizeof(path))return -ENAMETOOLONG;
        FILE *in=fopen(path,"rb");unsigned long long sequence=0;
        if(in){if(fscanf(in,"CiukiOS crash sequence=%llu",&sequence)!=1)sequence=0;fclose(in);}
        if(sequence<oldest){oldest=sequence;slot=i;}if(sequence>maximum)maximum=sequence;
    }
    snprintf(path,sizeof(path),"%s/CRASH%u.TXT",directory,slot);
    FILE *out=fopen(path,"wb");if(!out)return -errno;
    /* Fixed format and bounded names keep the entire file below 2 KiB. */
    int error=0;
    if(f->kind==W32_REPORT_NATIVE_FAULT){
        /* G13: native program; same first line, so rotation and the log viewer treat both alike. */
        char text[640];
        if(desk_native_fault_text(f,text,sizeof(text))<0)error=EIO;
        else if(fprintf(out,"CiukiOS crash sequence=%llu\nExecutable: %.63s\nPID: %u TID: %u\nNative fault\n%s\n",maximum+1,f->exe,f->pid,f->tid,text)<0)error=EIO;
        if(fflush(out)||fsync(fileno(out)))error=EIO;
        if(fclose(out))error=EIO;
        return -error;
    }
    if(fprintf(out,"CiukiOS crash sequence=%llu\nExecutable: %.63s\nPID: %u TID: %u\nException: %08x\nModule: %.63s + %08x\nEIP: %08x\n"
        "EAX=%08x EBX=%08x ECX=%08x EDX=%08x\nESI=%08x EDI=%08x EBP=%08x ESP=%08x EFLAGS=%08x\nStack:",
        maximum+1,f->exe,f->pid,f->tid,f->code,f->module,f->offset,f->eip,f->registers[0],f->registers[1],f->registers[2],f->registers[3],f->registers[4],f->registers[5],f->registers[6],f->registers[7],f->registers[8])<0)error=EIO;
    for(unsigned i=0;i<8;i++){
        int n=f->valid&(1u<<i)?fprintf(out," %08x",f->stack[i]):fprintf(out," ????????");if(n<0)error=EIO;
    }
    if(fputc('\n',out)==EOF||fflush(out)||fsync(fileno(out)))error=EIO;
    if(fclose(out))error=EIO;
    return -error;
}
