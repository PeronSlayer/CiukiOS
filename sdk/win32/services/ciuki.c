/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Original CIUKI_SYS_W32 backend. */
#include <w32/ldr.h>
#include <ciuki/abi.h>
#include <w32/shims.h>
#include <w32/server.h>
#include <ciuki/w32_transport.h>
static struct ciuki_w32_header header(uint32_t op, uint32_t size) {
    return (struct ciuki_w32_header){1, size, op, 0};
}
int32_t w32_ciuki_call(void *record, uint32_t size) {
#ifdef W32_KERNEL_MODEL
    extern int32_t w32_kernel_model_call(void *, uint32_t);
    return w32_kernel_model_call(record, size);
#else
    int32_t result;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp"
                     : "=a"(result)
                     : "a"(CIUKI_SYS_W32), "b"(record), "c"(size), "d"(0), "S"(0), "D"(0)
                     : "memory", "cc");
    return result;
#endif
}
static void *map(void *o, uint32_t at, uint32_t n, int exact) {
    (void)o;
    struct ciuki_w32_vm a = {.header = header(W32_VM_ALLOC, sizeof(a)),
                             .address = at,
                             .bytes = n,
                             .flags = W32_MEM_RESERVE | W32_MEM_COMMIT,
                             .protect = W32_PAGE_READWRITE};
    if (!at)
        a.flags |= W32_MEM_TOP_DOWN;
    int32_t status = w32_ciuki_call(&a, sizeof(a));
    if (status && !exact) {
        a.address = 0;
        a.bytes = n;
        a.flags |= W32_MEM_TOP_DOWN;
        status = w32_ciuki_call(&a, sizeof(a));
    }
    return status ? 0 : (void *)(uintptr_t)a.address;
}
static void *alloc(void *o, uint32_t n) { return n ? map(o, 0, n, 0) : 0; }
static void release(void *o, void *p, uint32_t n) {
    (void)o;
    (void)n;
    if (!p)
        return;
    struct ciuki_w32_vm a = {.header = header(W32_VM_FREE, sizeof(a)),
                             .address = (uint32_t)(uintptr_t)p,
                             .flags = W32_MEM_RELEASE};
    if (w32_ciuki_call(&a, sizeof(a))) {
        struct ciuki_w32_loader_args b = {.header = header(W32_LOADER, sizeof(b)),
                                          .action = W32_LDR_UNMAP,
                                          .address = (uint32_t)(uintptr_t)p};
        w32_ciuki_call(&b, sizeof(b));
    }
}
static void unmap(void *o, void *p, uint32_t n) {
    /* Builtins include the currently executing ntdll. The kernel releases
     * these views after the thread has left user mode during teardown. */
    uintptr_t address = (uintptr_t)p;
    if (address >= CIUKI_W32_APP_LIMIT && address < CIUKI_W32_BUILTIN_LIMIT)
        return;
    release(o, p, n);
}
static int protect(void *o, void *p, uint32_t n, uint32_t mode) {
    (void)o;
    uint32_t prot = !mode ? W32_PAGE_NOACCESS
                    : mode & W32_PROT_EXEC
                        ? mode & W32_PROT_WRITE ? W32_PAGE_EXECUTE_READWRITE : W32_PAGE_EXECUTE_READ
                    : mode & W32_PROT_WRITE ? W32_PAGE_READWRITE
                                            : W32_PAGE_READONLY;
    struct ciuki_w32_vm a = {.header = header(W32_VM_PROTECT, sizeof(a)),
                             .address = (uint32_t)(uintptr_t)p,
                             .bytes = n,
                             .protect = prot};
    return !w32_ciuki_call(&a, sizeof(a));
}
static int open_file(void *o, const char *path, struct w32_file *f) {
    (void)o;
    /* A low-address copy also gives the H0 kernel model the real wire path. */
    uint32_t n = w32_strlen(path) + 1;
    /* MAX_PATH counts ACP characters, not their expanded UTF-8 bytes. */
    if (n > 3 * 259 + 1)
        return 0;
    char *copy = alloc(0, n);
    if (!copy)
        return 0;
    w32_copy(copy, path, n);
    struct ciuki_w32_loader_args a = {.header = header(W32_LOADER, sizeof(a)),
                                      .action = W32_LDR_OPEN,
                                      .path = (uint32_t)(uintptr_t)copy};
    int32_t status = w32_ciuki_call(&a, sizeof(a));
    release(0, copy, n);
    if (status)
        return 0;
    *f = (struct w32_file){(void *)(uintptr_t)a.address, a.bytes, (void *)(uintptr_t)a.address};
    return 1;
}
static void close_file(void *o, struct w32_file *f) { release(o, f->cookie, f->size); }
static void *image(void *o, const struct w32_file *f, uint32_t at, int exact, int builtin) {
    (void)o;
    struct ciuki_w32_loader_args a = {.header = header(W32_LOADER, sizeof(a)),
                                      .action = W32_LDR_IMAGE,
                                      .source = (uint32_t)(uintptr_t)f->bytes,
                                      .bytes = f->size,
                                      .address = at,
                                      .flags = (exact ? W32_LDR_EXACT : 0) |
                                               ((builtin & 1) ? W32_LDR_BUILTIN : 0) |
                                               ((builtin & 2) ? W32_LDR_DATAFILE : 0)};
    return w32_ciuki_call(&a, sizeof(a)) ? 0 : (void *)(uintptr_t)a.address;
}
static uint32_t tid(void *o) {
    (void)o;
    return (uint32_t)(uintptr_t)NtCurrentTeb()->ClientId.UniqueThread;
}
static void wait_word(void *o, volatile uint32_t *p, uint32_t value) {
    (void)o;
    struct ciuki_w32_word_args a = {.header = header(W32_WORD, sizeof(a)),
                                    .address = (uint32_t)(uintptr_t)p,
                                    .expected = value};
    w32_ciuki_call(&a, sizeof(a));
}
static void wake_word(void *o, volatile uint32_t *p) {
    (void)o;
    struct ciuki_w32_word_args a = {
        .header = header(W32_WORD, sizeof(a)), .address = (uint32_t)(uintptr_t)p, .wake = 1};
    w32_ciuki_call(&a, sizeof(a));
}
static void diagnostic(void *o, const char *event, const char *module, const char *detail) {
    (void)o;
    struct ciuki_w32_diagnostic a = {.header = header(W32_DIAGNOSTIC, sizeof(a)),
                                     .kind = !w32_strcmp(event, "stub")    ? 3
                                             : !w32_strcmp(event, "crash") ? 2
                                                                           : 1};
    if(!w32_strcmp(event,"debug")||!w32_strcmp(event,"dialog"))return;
    const char *base=module?module:"";
    for(const char *p=base;*p;p++)if(*p=='/'||*p=='\\')base=p+1;
    const char *parts[] = {event, " check=", detail, " module=", base};
    if(a.kind==3){parts[1]=" ";parts[2]=module;parts[3]="!";parts[4]=detail;}
    uint32_t at = 0;
    for (unsigned i = 0; i < 5; i++)
        for (const unsigned char *p = (const void *)parts[i]; p && *p; p++) {
            if (at + 1 >= sizeof(a.payload))
                break;
            a.payload[at++] = *p >= 32 && *p <= 126 ? *p : '?';
        }
    w32_ciuki_call(&a, sizeof(a));
}
static int fingerprint(void *o,struct w32_shim_identity *id){
    (void)o;struct w32_file f={0};char *hash=alloc(0,65);if(!hash)return 0;
    /* Hash the retained spawn snapshot without consuming its one-shot OPEN.
     * The later loader must bind exactly the bytes which selected the shims. */
    struct ciuki_w32_compat_args a={.header=header(W32_COMPAT,sizeof(a)),.action=1,.settings=(uint32_t)(uintptr_t)hash,.bytes=65};
    int ok=!w32_ciuki_call(&a,sizeof(a));
    if(!ok&&open_file(0,id->exe,&f)){
        a=(struct ciuki_w32_compat_args){.header=header(W32_COMPAT,sizeof(a)),.action=1,.settings=(uint32_t)(uintptr_t)hash,.bytes=65,
            .source=(uint32_t)(uintptr_t)f.bytes,.source_bytes=f.size};
        ok=!w32_ciuki_call(&a,sizeof(a));
    }
    if(ok){id->timestamp=a.timestamp;id->image_size=a.image_size;id->file_size=a.file_size;w32_copy(id->sha256,hash,65);}
    release(0,hash,65);if(f.cookie)close_file(0,&f);return ok;
}
static int configure(void *o,const struct ciuki_w32_compat *settings,const struct w32_shim_identity *id){
    (void)o;(void)id;
    void *p=alloc(0,sizeof(*settings));if(!p)return 0;w32_copy(p,settings,sizeof(*settings));
    struct ciuki_w32_compat_args a={.header=header(W32_COMPAT,sizeof(a)),.action=2,.settings=(uint32_t)(uintptr_t)p,.bytes=sizeof(*settings)};
    int ok=!w32_ciuki_call(&a,sizeof(a));release(0,p,sizeof(*settings));return ok;
}
static void report(void *o,uint32_t action,uint32_t code,struct w32_context *context,const char *module,uint32_t base){
    (void)o;
    uint32_t n=context?sizeof(*context):0;char *p=alloc(0,n+64);if(!p)return;
    if(context)w32_copy(p,context,n);w32_copy_string(p+n,64,module?module:"");
    struct ciuki_w32_report_args a={.header=header(W32_REPORT,sizeof(a)),.action=action,.code=code,
        .context=context?(uint32_t)(uintptr_t)p:0,.module=(uint32_t)(uintptr_t)(p+n),.base=base};
    w32_ciuki_call(&a,sizeof(a));release(0,p,n+64);
}
/* One read-only registry request. No advapi dependency in ntdll/bootstrap. */
static int compat_packet(uint32_t fd,struct w32r_packet *p,struct w32r_packet *r){
#ifdef W32_KERNEL_MODEL
    (void)fd;(void)p;(void)r;return 0;
#else
    struct ciuki_message m={.length=256};w32_copy(m.data,p,256);
    int32_t result;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp":"=a"(result):"a"(CIUKI_SYS_CHANNEL_SEND),"b"(fd),"c"(&m),"d"(0),"S"(0),"D"(0):"memory","cc");
    if(result<0)return 0;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp":"=a"(result):"a"(CIUKI_SYS_CHANNEL_RECV),"b"(fd),"c"(&m),"d"(0),"S"(0),"D"(0):"memory","cc");
    if(result!=1||m.length!=256||m.fd_count)return 0;w32_copy(r,m.data,256);
    return r->version==1&&r->id==p->id&&r->kind==p->kind&&!r->error;
#endif
}
static int compat_database(void *o,void *out,uint32_t cap,uint32_t *used){
    (void)o;*used=0;
    struct ciuki_w32_service_args a={.header=header(W32_SERVICE,sizeof(a)),.action=W32_SVC_CONNECT,.service=W32_SERVICE_W32SRV};
    if(w32_ciuki_call(&a,sizeof(a))||a.error||!a.result)return -1;
    struct {struct w32r_request request;char empty[2];} q={.request={.version=1,.op=W32R_COMPAT,.path_bytes=1,.name_bytes=1}};
    struct w32r_packet p={.version=1,.id=0xf516,.kind=W32R_BEGIN,.total=sizeof(q.request)+2},r;
    w32_copy(p.bytes,&q,p.total);if(!compat_packet(a.endpoint,&p,&r)||r.offset!=p.total)return -1;
    uint32_t total=r.total;if(total<40||total>40+cap)return -1;
    uint8_t *response=alloc(0,total);if(!response)return -1;
    uint32_t at=0;int ok=1;
    while(at<total){p=(struct w32r_packet){.version=1,.id=0xf516,.kind=W32R_READ,.total=total,.offset=at};
        unsigned n=total-at;if(n>232)n=232;
        if(!compat_packet(a.endpoint,&p,&r)||r.total!=total||r.offset!=at+n){ok=0;break;}
        w32_copy(response+at,r.bytes,n);at+=n;
    }
    const struct w32r_reply *reply=(const void *)response;int result=-1;
    if(ok&&reply->version==1){
        if(reply->error==2)result=0;
        else if(!reply->error&&reply->type==3&&!reply->name_bytes&&reply->data_bytes==total-40){*used=reply->data_bytes;w32_copy(out,response+40,*used);result=1;}
    }
    release(0,response,total);return result;
}
static void resume(void *o, struct w32_context *context) {
    (void)o;
    struct ciuki_w32_continue a = {.header = header(W32_CONTINUE, sizeof(a)),
                                   .context = (uint32_t)(uintptr_t)context};
    w32_ciuki_call(&a, sizeof(a));
    w32_terminate(STATUS_INVALID_PARAMETER);
}
static int raise_exception(void *o, struct w32_exception *exception, struct w32_context *context) {
    (void)o;
    struct ciuki_w32_raise a = {.header = header(W32_RAISE, sizeof(a)),
                                .exception = (uint32_t)(uintptr_t)exception,
                                .context = (uint32_t)(uintptr_t)context};
    return !w32_ciuki_call(&a, sizeof(a));
}
static void terminate(void *o, uint32_t code) {
    (void)o;
#ifdef W32_KERNEL_MODEL
    w32_ciuki_call(&code, 0);
#else
    __asm__ volatile("int $0x80"
                     :
                     : "a"(CIUKI_SYS_EXIT), "b"(code), "c"(0), "d"(0), "S"(0), "D"(0)
                     : "memory", "cc");
#endif
    __builtin_trap();
}
const struct w32_services w32_ciuki_services = {.alloc = alloc,
                                                .free = release,
                                                .map = map,
                                                .protect = protect,
                                                .unmap = unmap,
                                                .open = open_file,
                                                .close = close_file,
                                                .thread_id = tid,
                                                .wait_word = wait_word,
                                                .wake_word = wake_word,
                                                .diagnostic = diagnostic,
                                                .continue_context = resume,
                                                .terminate = terminate,
                                                .image = image,
                                                .raise = raise_exception,
                                                .fingerprint=fingerprint,
                                                .compat_database=compat_database,
                                                .configure=configure,
                                                .report=report};
