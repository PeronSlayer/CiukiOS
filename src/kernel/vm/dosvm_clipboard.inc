/* G5b WINOLDAP: transient request/completion, never a kernel clipboard.
 * Included by dosvm.c; register contract: Microsoft Q67675 and RBIL 17xx.
 * SPDX-License-Identifier: GPL-2.0-only */
/* kheap blocks stop at 2040 bytes. Transfers use the existing PMM page
 * allocation boundary (like the CD reservoir), released at every exit. */
static uint8_t *clipboard_alloc(uint32_t *physical)
{
    *physical=pmm_alloc_contig(CIUKI_DOSVM_CLIP_MAX/PAGE_SIZE);
    return *physical ? P2V(*physical) : 0;
}
static void clipboard_free(uint32_t physical)
{
    if (physical) for (unsigned i=0;i<CIUKI_DOSVM_CLIP_MAX/PAGE_SIZE;i++) pmm_free(physical+i*PAGE_SIZE);
}
static bool clipboard_ready(void *arg)
{
    struct dosvm *v=arg;
    struct process *p=proc_find(v->owner_pid);
    return v->clipboard.done || v->info.state>=DOSVM_STOPPING || !p || p->state>=PROC_STOPPING;
}
static int clipboard_upcall(struct dosvm *v,unsigned op,unsigned format,uint8_t *data,uint32_t *bytes)
{
    if (!v->owner_pid || v->clipboard.pending) return -EACCES;
    uint32_t flags=irq_save();
    if (!++v->clipboard.serial) ++v->clipboard.serial;
    v->clipboard.op=op; v->clipboard.format=format; v->clipboard.bytes=*bytes;
    v->clipboard.data=data; v->clipboard.done=false; v->clipboard.pending=true;
    v->clipboard.result=-ETIMEDOUT;
    irq_restore(flags);
    dosvm_present_notify(v);
    uint32_t hold=v->memory.pool.hold; v->memory.pool.hold=0;
    kwait_wait_until(&v->clipboard.wait,clipboard_ready,v,deadline_after_ms(2000));
    v->memory.pool.hold=hold;
    flags=irq_save();
    int result=v->clipboard.done ? v->clipboard.result : -ETIMEDOUT;
    if (!result) *bytes=v->clipboard.bytes;
    v->clipboard.pending=false; v->clipboard.data=0;
    irq_restore(flags);
    return result;
}
int dosvm_clipboard_interrupt(struct dosvm *v,struct v86_frame *f)
{
    unsigned ax=(uint16_t)f->tf.eax;
    if (ax<0x1700 || ax>0x1709 || ax==0x1706 || ax==0x1707 || !v->owner_pid) return -ENOSYS;
    if (ax==0x1700) { vm_low(&f->tf.eax,0x0104); return 0; } /* WINOLDAP compatibility version 4.01: AL major, AH minor */
    unsigned op=ax&255,format=(uint16_t)f->tf.edx;
    uint32_t bytes=op==3 || op==9 ? ((uint32_t)(uint16_t)f->tf.esi<<16)|(uint16_t)f->tf.ecx : 0;
    uint32_t address=((uint32_t)(uint16_t)f->es<<4)+(uint16_t)f->tf.ebx;
    int error=0;
    uint8_t *data=0; uint32_t physical=0;
    if ((op>=3 && op<=5 && format!=1 && format!=7) || bytes>CIUKI_DOSVM_CLIP_MAX) error=-EINVAL;
    if (!error && (op==3 || op==5)) {
        data=clipboard_alloc(&physical);
        if (!data) error=-ENOMEM;
        else if (op==3 && (!bytes || !dosvm_range(v,address,bytes,false) || dosvm_read(v,address,data,bytes))) error=-EFAULT;
        if (!error && op==3 && data[bytes-1]) error=-EINVAL;
    }
    if (!error) error=clipboard_upcall(v,op,format,data,&bytes);
    if (!error && op==5 && (!bytes || !dosvm_range(v,address,bytes,true) || dosvm_write(v,address,data,bytes))) error=-EFAULT;
    clipboard_free(physical);
    if (op==4 || op==9) {
        vm_low(&f->tf.eax,error ? 0 : bytes&65535);
        vm_low(&f->tf.edx,error ? 0 : bytes>>16);
    } else vm_low(&f->tf.eax,error ? 0 : 1);
    return 0; /* API failure is a register result, never a VM fault */
}
int dosvm_clipboard_reflect(struct dosvm *v,struct v86_frame *f,uint16_t image)
{
    struct v86_frame returned=*f;
    int e=dosvm_clipboard_interrupt(v,&returned);
    if (e) return e;
    returned.tf.cs=0xf000; returned.tf.eip=DPMI_RM_RETURN;
    vm_low(&returned.tf.user_esp,(uint16_t)(returned.tf.user_esp+6));
    returned.tf.eflags=(returned.tf.eflags&~0x0cd5u)|(image&0x0cd5u)|V86_VM|V86_IF|2u;
    v->monitor.vif=(image&V86_IF)!=0; *f=returned;
    return 0;
}
int dosvm_clipboard_client(struct dosvm *v,unsigned op,struct ciuki_dosvm_command *c)
{
    bool complete=op==CIUKI_DOSVM_CLIP_COMPLETE;
    unsigned n=complete && !c->clip.result ? c->clip.bytes : 0;
    if (complete && n>c->clip.capacity && v->clipboard.op==5) return -EINVAL;
    /* User copies may sleep. Snapshot into a temporary buffer before/after
     * the atomic slot access so a timed-out VM never frees an in-use buffer. */
    uint32_t physical=0;
    uint8_t *temp=clipboard_alloc(&physical);
    if (!temp) return -ENOMEM;
    int e=0;
    if (complete && n && v->clipboard.op==5) e=copy_from_user(temp,c->clip.buffer,n);
    uint32_t flags=irq_save();
    if (!e && (!v->clipboard.pending || v->clipboard.done ||
        (complete && c->clip.serial!=v->clipboard.serial))) e=-ESRCH;
    if (!e && complete) {
        if (v->clipboard.op==5 && !c->clip.result && n) memcpy(v->clipboard.data,temp,n);
        v->clipboard.bytes=c->clip.bytes; v->clipboard.result=c->clip.result; v->clipboard.done=true;
    } else if (!e) {
        c->clip.serial=v->clipboard.serial; c->clip.op=v->clipboard.op;
        c->clip.format=v->clipboard.format; c->clip.bytes=v->clipboard.bytes;
        n=(v->clipboard.op==3 || v->clipboard.op==CIUKI_DOSVM_CLIP_SCREEN) ? v->clipboard.bytes : 0;
        if (n>c->clip.capacity) e=-ENOSPC;
        else if (n) memcpy(temp,v->clipboard.data,n);
    }
    irq_restore(flags);
    if (!e && !complete && n) e=copy_to_user(c->clip.buffer,temp,n);
    clipboard_free(physical);
    if (!e && complete) kwait_wake_all(&v->clipboard.wait);
    return e;
}
void dosvm_clipboard_checkpoint(struct dosvm *v)
{
    if (!v->clipboard_screen || v->clipboard.pending) return;
    v->clipboard_screen=false;
    if (!v->fullscreen || v->vbe_mode || !v->owner_pid || !v->window_input) return;
    uint8_t mode=0,rows=0; uint16_t columns=0,page=0,height=0;
    if (dosvm_read(v,0x449,&mode,1) || (mode>3 && mode!=7) ||
        dosvm_read(v,0x44a,&columns,2) || dosvm_read(v,0x44e,&page,2) ||
        dosvm_read(v,0x484,&rows,1) || dosvm_read(v,0x485,&height,2) ||
        !columns || columns>160 || rows>=100 || !height || height>32 || (page&1) ||
        page>32768 || (unsigned)columns*(rows+1)>16384-page/2) return;
    uint32_t physical=0;
    uint8_t *data=clipboard_alloc(&physical);
    if (!data) return;
    uint32_t n=0; int e=0;
    dosvm_fullscreen_model_begin(v);
    for (unsigned y=0;y<=rows && !e;y++) {
        uint8_t raw[320]; unsigned used=columns;
        e=dosvm_read(v,(mode==7 ? 0xb0000 : 0xb8000)+page+y*columns*2,raw,columns*2);
        if (e) break;
        while (used && (!raw[2*(used-1)] || raw[2*(used-1)]==' ')) used--;
        for (unsigned x=0;x<used;x++) data[n++]=raw[2*x] ? raw[2*x] : ' ';
        if (y<rows) { data[n++]='\r'; data[n++]='\n'; }
    }
    dosvm_fullscreen_model_end(v);
    data[n++]=0;
    if (!e) (void)clipboard_upcall(v,CIUKI_DOSVM_CLIP_SCREEN,7,data,&n);
    clipboard_free(physical);
}
