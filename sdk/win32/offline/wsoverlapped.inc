/* Eight bounded pending vector operations, event or issuer-thread APC. */
struct ws_overlapped_job {
    uintptr_t socket;
    OVERLAPPED *overlapped;
    void (W32API *completion)(DWORD,DWORD,OVERLAPPED *,DWORD);
    struct ws_buffer buffers[16];
    unsigned count,bytes,offset,flags,result_flags,serial;
    int sending,error,busy,cancelled,finished;
    unsigned slot;
    uint32_t thread;
    struct ciuki_sockaddr_in address;
    void *receive_address;
    int *receive_length;
    int address_bytes;
    char data[];
};
static struct ws_overlapped_job *overlapped_jobs[8];
static unsigned overlapped_serial;
static int overlapped_attempt(struct ws_overlapped_job *j)
{
    unsigned moved=0,flags=0;
    void *address=j->sending?(j->address_bytes?&j->address:0):j->receive_address?&j->address:0;
    int length=16;
    int r=transfer_ex(j->socket,j->data+j->offset,(int)(j->bytes-j->offset),(int)j->flags,address,
        j->sending?j->address_bytes:address?(intptr_t)&length:0,j->sending,MSG_DONTWAIT,&moved,&flags);
    if(r<0 && WSAGetLastError()==10035)return 0;
    j->offset+=moved;j->result_flags=flags&MSG_TRUNC?0x8000u:0;
    j->error=r<0?WSAGetLastError():0;
    if(j->sending && !j->error && j->offset<j->bytes)return 0;
    return 1;
}
static void overlapped_free(struct ws_overlapped_job *j)
{
    if(j->thread) {
        struct ciuki_w32_object_args a={.action=W32_OBJ_CLOSE,.handle=j->thread};a.header=K32_HEADER(W32_OBJECT,a);
        (void)w32_ciuki_call(&a,sizeof(a));
    }
    const struct w32_services *s=w32_get_services();s->free(s->opaque,j,(uint32_t)sizeof(*j)+(j->bytes?j->bytes:1));
}
static void W32API overlapped_apc(uintptr_t arg)
{
    struct ws_overlapped_job *j=(void *)arg;
    void (W32API *completion)(DWORD,DWORD,OVERLAPPED *,DWORD)=j->completion;
    DWORD error=(DWORD)j->error,bytes=j->offset,flags=j->result_flags;OVERLAPPED *ov=j->overlapped;
    lock();if(overlapped_jobs[j->slot]==j)overlapped_jobs[j->slot]=0;int cancelled=j->cancelled;unlock();
    overlapped_free(j);if(!cancelled)completion(error,bytes,ov,flags);
}
static void overlapped_complete(struct ws_overlapped_job *j)
{
    /* Guard serializes cancellation against all application-owned outputs. */
    unsigned at=0;
    if(!j->sending) {
        for(unsigned i=0;i<j->count && at<j->offset;i++) {
            unsigned n=j->buffers[i].length;if(n>j->offset-at)n=j->offset-at;
            w32_copy(j->buffers[i].buffer,j->data+at,n);at+=n;
        }
        if(j->receive_address && (!j->error || j->error==10040)) {
            w32_copy(j->receive_address,&j->address,16);*j->receive_length=16;
        }
    }
    j->overlapped->internal_high=j->offset;
    __atomic_store_n(&j->overlapped->internal,(uintptr_t)j->error,__ATOMIC_RELEASE);
    if(j->completion) {
        struct ciuki_w32_apc_args a={.action=W32_APC_QUEUE,.thread=j->thread,.routine=WP(overlapped_apc),.argument=WP(j)};
        a.header=K32_HEADER(W32_APC,a);
        j->busy=2;
        if(!w32_ciuki_call(&a,sizeof(a)) && !a.error)return;
        /* Bounded retry when the issuer APC queue is full. Keep this slot
         * reserved until the callback runs; queued completions cannot grow
         * outside the eight-operation admission budget. */
        j->busy=0;__atomic_store_n(&j->overlapped->internal,997,__ATOMIC_RELEASE);return;
    } else if(j->overlapped->event && async_set)async_set((HANDLE)((uintptr_t)j->overlapped->event&~(uintptr_t)1));
    overlapped_jobs[j->slot]=0;overlapped_free(j);
}
static void overlapped_cleanup(void)
{
    for(unsigned i=0;i<8;i++)if(overlapped_jobs[i]) {
        struct ws_overlapped_job *j=overlapped_jobs[i];j->cancelled=1;
        if(!j->busy) { overlapped_jobs[i]=0;overlapped_free(j); }
    }
}
static void overlapped_step(void)
{
    for(unsigned i=0;i<8;i++) {
        lock();struct ws_overlapped_job *j=overlapped_jobs[i];int earlier=0;
        if(j)for(unsigned k=0;k<8;k++)if(overlapped_jobs[k] && overlapped_jobs[k]->socket==j->socket &&
            overlapped_jobs[k]->sending==j->sending && !overlapped_jobs[k]->finished && overlapped_jobs[k]->serial<j->serial)earlier=1;
        if(!j || j->busy || earlier) { unlock();continue; }
        j->busy=1;unlock();int complete=j->finished?1:overlapped_attempt(j);
        lock();j->busy=0;
        if(j->cancelled || !starts) { overlapped_jobs[i]=0;overlapped_free(j); }
        else if(complete) { j->finished=1;overlapped_complete(j); }
        unlock();
    }
}
static int overlapped_io(uintptr_t socket_,struct ws_buffer *buffers,unsigned count,ULONG *done,
    ULONG *receive_flags,unsigned send_flags,void *address,intptr_t length,void *overlapped,void *completion,int sending)
{
    if(!buffers || !count || count>16 || (!sending && !receive_flags))return fail(10014);
    if((!address && length) || (address && !length) || (address && (sending?length:*(int *)length)!=16))return fail(10014);
    unsigned bytes=0;for(unsigned i=0;i<count;i++) {
        if((buffers[i].length && !buffers[i].buffer) || buffers[i].length>CIUKI_SOCKET_IO_MAX-bytes)return fail(10040);
        bytes+=buffers[i].length;
    }
    if(async_initialize())return -1;
    const struct w32_services *s=w32_get_services();
    struct ws_overlapped_job *j=s && s->alloc?s->alloc(s->opaque,(uint32_t)sizeof(*j)+(bytes?bytes:1)):0;
    if(!j)return fail(10055);w32_fill(j,0,sizeof(*j));
    j->socket=socket_;j->overlapped=overlapped;j->completion=completion;j->count=count;j->bytes=bytes;
    j->flags=sending?send_flags:*receive_flags;j->sending=sending;
    j->address_bytes=address?16:0;if(sending && address)w32_copy(&j->address,address,16);
    else if(address) { j->receive_address=address;j->receive_length=(void *)length; }
    unsigned at=0;for(unsigned i=0;i<count;i++) {
        j->buffers[i]=buffers[i];if(sending && buffers[i].length)w32_copy(j->data+at,buffers[i].buffer,buffers[i].length);at+=buffers[i].length;
    }
    if(completion) {
        struct ciuki_w32_object_args a={.action=W32_OBJ_DUPLICATE,.handle=0xfffffffeu,.source=0xffffffffu,.target=0xffffffffu,.flags=2};
        a.header=K32_HEADER(W32_OBJECT,a);
        if(w32_ciuki_call(&a,sizeof(a)) || a.error) { overlapped_free(j);return fail(10055); }j->thread=a.result;
    }
    lock();unsigned slot=8;
    if(starts && overlapped_serial!=UINT32_MAX)for(unsigned i=0;i<8;i++)if(!overlapped_jobs[i]) { slot=i;break; }
    if(slot==8) { unlock();overlapped_free(j);return fail(10055); }
    for(unsigned i=0;i<8;i++)if(overlapped_jobs[i] && overlapped_jobs[i]->overlapped==overlapped) {
        unlock();overlapped_free(j);return fail(10022);
    }
    j->serial=++overlapped_serial;j->slot=slot;j->busy=1;overlapped_jobs[slot]=j;
    j->overlapped->internal=997;j->overlapped->internal_high=0;
    if(j->overlapped->event && !completion && async_reset)async_reset((HANDLE)((uintptr_t)j->overlapped->event&~(uintptr_t)1));
    int earlier=0;for(unsigned i=0;i<8;i++)if(overlapped_jobs[i] && overlapped_jobs[i]!=j &&
        overlapped_jobs[i]->socket==socket_ && overlapped_jobs[i]->sending==sending && !overlapped_jobs[i]->finished)earlier=1;
    unlock();int complete=earlier?0:overlapped_attempt(j);
    lock();j->busy=0;
    if(j->cancelled || !starts) { overlapped_jobs[slot]=0;overlapped_free(j);unlock();return fail(995); }
    if(complete) {
        int error=j->error;unsigned transferred=j->offset,flags=j->result_flags;int callback=j->completion!=0;j->finished=1;
        overlapped_complete(j);int deferred=callback && !j->busy;unlock();if(deferred)return fail(997);if(done)*done=transferred;if(receive_flags)*receive_flags=flags;
        return error?fail(error):0;
    }
    unlock();return fail(997);
}
BOOL W32API WSAGetOverlappedResult(uintptr_t socket_,OVERLAPPED *ov,DWORD *bytes,BOOL wait,DWORD *flags)
{
    int fd=acquire(socket_,0);if(fd<0)return 0;release(fd);
    if(!ov || !bytes || !flags) { fail(10014);return 0; }
    for(;;) {
        uintptr_t status=__atomic_load_n(&ov->internal,__ATOMIC_ACQUIRE);
        if(status!=997) { *bytes=(DWORD)ov->internal_high;*flags=status==10040?0x8000u:0;if(status) { fail((int)status);return 0; }return 1; }
        if(!wait) { fail(996);return 0; }
        if(async_initialize())return 0;async_sleep(1);
    }
}
