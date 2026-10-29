/* Native conformance client linked to the real H1 request encoder. No PE
 * execution, i386 syscall filter changes or replacement encoder.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "h1_services.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/w32_core.h>
#pragma clang diagnostic pop
struct host_state host;
#define PTR(p) ((uint32_t)(uintptr_t)(p))
#define HEADER(op,a) (struct ciuki_w32_header){1,sizeof(a),op,0}
LONG host_syscall(ULONG n,ULONG a,ULONG b,ULONG c,ULONG d,ULONG e,ULONG f){
    (void)d;(void)e;(void)f;
    /* Force both partial transfers and EINTR in the actual C framing loop. */
    static unsigned calls;if(!(++calls%17))return -EINTR;
    if(c>113)c=113;
    ssize_t r=n==3?read((int)a,(void *)(uintptr_t)b,c):n==4?write((int)a,(void *)(uintptr_t)b,c):-1;
    return r<0?-errno:(LONG)r;
}
void W32API RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *p){
    unsigned n=0;while(p[n])n++;*s=(UNICODE_STRING){n*2,n*2+2,(WCHAR *)p};
}
static void call(void *p,uint32_t n){
    int32_t status=-999;assert(h1_installer_kernel(p,n,&status) && !status);
}
static void close_handle(uint32_t handle){
    struct ciuki_w32_object_args a={.action=W32_OBJ_CLOSE,.handle=handle};
    a.header=HEADER(W32_OBJECT,a);call(&a,sizeof(a));assert(!a.error);
}
static void files(void){
    char path[]="C:\\WINDOWS\\TEMP\\protocol.txt",moved[]="C:\\WINDOWS\\TEMP\\moved.txt";
    char directory[]="C:\\WINDOWS\\TEMP\\protocol-dir",pattern[]="C:\\WINDOWS\\TEMP\\*.txt";
    uint8_t buffer[832]={0};
    struct ciuki_w32_file_args a={.action=W32_F_OPEN,.path=PTR(path),.access=0xc0000000,.share=7,.disposition=1};
    a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(!a.error && a.path==PTR(path));
    uint32_t handle=a.result;
    for(unsigned action=W32_F_READ;action<=W32_F_TIMES;action++){
        memset(buffer,0,sizeof(buffer));if(action==W32_F_WRITE)memcpy(buffer,"Ciuki",5);
        a=(struct ciuki_w32_file_args){.action=action,.handle=handle,.buffer=PTR(buffer),.bytes=sizeof(buffer)};
        if(action==W32_F_TIMES)a.flags=7;
        a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(!a.error && a.buffer==PTR(buffer));
    }
    a=(struct ciuki_w32_file_args){.action=W32_F_MOVE,.path=PTR(path),.path2=PTR(moved)};
    a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(!a.error && a.path2==PTR(moved));
    a.action=W32_F_DELETE;a.path=PTR(moved);a.path2=0;call(&a,sizeof(a));assert(!a.error);
    /* Descriptor metadata survives rename, share-delete unlink and name reuse. */
    for(unsigned action=W32_F_STAT;action<=W32_F_TIMES;action++){
        a=(struct ciuki_w32_file_args){.action=action,.handle=handle,.buffer=PTR(buffer),.bytes=sizeof(buffer),.flags=7};
        a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(!a.error);
    }
    close_handle(handle);
    for(unsigned action=W32_F_MKDIR;action<=W32_F_UNLOCK;action++){
        a=(struct ciuki_w32_file_args){.action=action,.buffer=PTR(buffer),.bytes=sizeof(buffer)};
        if(action==W32_F_MKDIR || action==W32_F_RMDIR || action==W32_F_CHDIR)a.path=PTR(directory);
        if(action==W32_F_FULLPATH || action==W32_F_DISK)a.path=PTR(path);
        if(action==W32_F_FIRST)a.path=PTR(pattern);
        a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));
        if(action==W32_F_CHDIR)assert(a.error==3); /* directory was removed */
        else if(action==W32_F_FIRST)assert(a.error==2);
        else if(action==W32_F_NEXT)assert(a.error==6);
        else if(action>=W32_F_LOCK)assert(a.error==120);
        else assert(!a.error);
    }
    char protected[]="C:\\System\\protected.txt";
    a=(struct ciuki_w32_file_args){.action=W32_F_OPEN,.path=PTR(protected),.access=0x80000000,.share=7,.disposition=3};
    a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(!a.error);handle=a.result;
    for(unsigned action=W32_F_READ;action<=W32_F_TIMES;action++){
        if(action!=W32_F_WRITE && action!=W32_F_TRUNCATE && action!=W32_F_ATTR && action!=W32_F_TIMES)continue;
        a=(struct ciuki_w32_file_args){.action=action,.handle=handle,.buffer=PTR(buffer),.bytes=sizeof(buffer),.flags=7};
        a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(a.error==5);
    }
    close_handle(handle);
    a=(struct ciuki_w32_file_args){.action=W32_F_OPEN,.path=PTR(path),.access=0x40000000,.disposition=2,.flags=0x04000000};
    a.header=HEADER(W32_FILE,a);call(&a,sizeof(a));assert(a.error==120);
}
static void objects(void){
    WCHAR name[]={'C','i','u','k','i',0};
    struct ciuki_w32_object_args a={.action=W32_OBJ_CREATE,.type=W32_OBJ_EVENT,.name=PTR(name),.value=1,.maximum=1};
    a.header=HEADER(W32_OBJECT,a);call(&a,sizeof(a));assert(!a.error && a.name==PTR(name));
    uint32_t event=a.result;
    for(unsigned action=W32_OBJ_OPEN;action<=W32_OBJ_TIMER_CANCEL;action++){
        if(action==W32_OBJ_CLOSE)continue;
        a=(struct ciuki_w32_object_args){.action=action,.type=W32_OBJ_EVENT,.handle=event,.name=PTR(name),.value=1};
        a.header=HEADER(W32_OBJECT,a);call(&a,sizeof(a));
        if(action==W32_OBJ_OPEN){assert(!a.error);close_handle(a.result);}
        else if(action==W32_OBJ_SET || action==W32_OBJ_RESET || action==W32_OBJ_RELEASE ||
                action==W32_OBJ_QUERY || action==W32_OBJ_SET_INHERIT)assert(!a.error);
        else assert(a.error==120);
    }
    uint32_t handles[]={event};
    struct ciuki_w32_wait_args w={.handles=PTR(handles),.count=1};
    w.header=HEADER(W32_WAIT,w);call(&w,sizeof(w));assert(!w.error && !w.result && w.handles==PTR(handles));
    a.action=W32_OBJ_RESET;call(&a,sizeof(a));assert(!a.error);
    call(&w,sizeof(w));assert(!w.error && w.result==258);
    handles[0]=0;call(&w,sizeof(w));assert(w.error==6);
    close_handle(event);
}
static void processes(void){
    char image[]="C:\\WINDOWS\\TEMP\\protocol-child.exe",command[]="protocol-child",cwd[]="C:\\WINDOWS\\TEMP";
    char env[]="TEMP=C:\\WINDOWS\\TEMP\0\0";
    /* fx-hlis5: the STARTUPINFO record is read (flags and show reach the
     * child, as the kernel copies it); the caller's pointer is preserved. */
    struct ciuki_w32_startup startup={.flags=1,.show=7};
    struct ciuki_w32_spawn_args a={.image=PTR(image),.command=PTR(command),.directory=PTR(cwd),
        .environment=PTR(env),.environment_bytes=sizeof(env),.startup=PTR(&startup)};
    a.header=HEADER(W32_PROCESS,a);call(&a,sizeof(a));
    assert(!a.error && a.image==PTR(image) && a.command==PTR(command) && a.directory==PTR(cwd) &&
           a.environment==PTR(env) && a.startup==PTR(&startup));
    uint32_t process=a.process_handle,thread=a.thread_handle,handles[]={process};
    struct ciuki_w32_wait_args w={.handles=PTR(handles),.count=1,.timeout=10000};
    w.header=HEADER(W32_WAIT,w);call(&w,sizeof(w));assert(!w.error && !w.result);
    for(unsigned action=W32_S_TIME;action<=W32_S_WRITE_MEMORY;action++){
        struct ciuki_w32_system_args s={.action=action};
        s.header=HEADER(W32_SYSTEM,s);call(&s,sizeof(s));
        if(action==W32_S_TIME || action==W32_S_MEMORY || action==W32_S_YIELD)assert(!s.error);
        else if(action==W32_S_TERMINATE_PROCESS)assert(s.error==6);
        else assert(s.error==120);
    }
    close_handle(process);close_handle(thread);
    a.inherit=1;call(&a,sizeof(a));assert(a.error==87);
}
static void registry(void){
    /* Use the real outer C RPC encoder for the existing w32srv wire ABI. */
    uint8_t input[256],out[4096];uint32_t used;
    const char path[]="Software\\CiukiProtocol",name[]="value";
    uint32_t key=0x80000002;
    for(unsigned op=1;op<=12;op++){
        uint32_t names=op==2?1:sizeof(name);
        uint32_t words[]={1,op,key,0,op==4?1u:0u,0x2001f,0,sizeof(path),names,0};
        memcpy(input,words,sizeof(words));memcpy(input+sizeof(words),path,sizeof(path));
        memcpy(input+sizeof(words)+sizeof(path),op==2?"":name,names);
        assert(!h1_installer_server(input,sizeof(words)+sizeof(path)+names,out,sizeof(out),&used));
        assert(used>=40);
        if(op==2){assert(!((uint32_t *)out)[1]);key=((uint32_t *)out)[2];}
    }
    /* The new fixture uses EnumRegKey/EnumRegValue. Their terminal error is
     * inside a normal w32srv reply, not a failed or missing outer RPC. */
    for(unsigned op=8;op<=9;op++){
        uint32_t words[]={1,op,0x80000002,0xffffffff,0,0,0,1,1,0};
        memcpy(input,words,sizeof(words));memset(input+sizeof(words),0,2);
        assert(!h1_installer_server(input,sizeof(words)+2,out,sizeof(out),&used));
        assert(used==40 && ((uint32_t *)out)[1]==259 && !((uint32_t *)out)[8] && !((uint32_t *)out)[9]);
    }
}
static void *exercise(void *argument){
    char **argv=argument;assert(h1_installer_init(argv[4]));
    if(!strcmp(argv[3],"protocol-child"))return 0;
    if(!strcmp(argv[3],"protocol-hang")){pause();return 0;}
    files();objects();processes();registry();
    puts("PASS actual C H1 encoder: startup, file 1-21, object 1-13, wait, process, system 1-11, registry 1-12 (including enumeration EOF)");
    return 0;
}
int main(int argc,char **argv){
    assert(argc==5);
    size_t bytes=2*1024*1024;
    void *stack=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(stack!=(void *)-1 && (uintptr_t)stack<=UINT32_MAX-bytes);
    pthread_attr_t attr;pthread_t thread;
    assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstack(&attr,stack,bytes));
    assert(!pthread_create(&thread,&attr,exercise,argv));assert(!pthread_join(thread,0));
    assert(!pthread_attr_destroy(&attr));assert(!munmap(stack,bytes));return 0;
}
