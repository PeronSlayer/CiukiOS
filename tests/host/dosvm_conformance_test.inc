/* H51 production INT13 fixture and bounded trace exporter; host transport only.
 * SPDX-License-Identifier: GPL-2.0-only */
bool dos_file_trace_pop(struct dos_file *d,struct dos_file_trace *out)
{
    if (!d->trace_count) return false;
    *out=d->trace[d->trace_head];d->trace_head=(d->trace_head+1)%DOS_FILE_TRACE;d->trace_count--;return true;
}
static struct dosvm *trace_export_vm;
static void trace_export_reentry(void) { dosvm_file_dump(trace_export_vm); }
static unsigned direct_trace_returns;
static bool direct_trace_check;
void dosvm_file_return(struct dosvm *v,const struct v86_frame *f)
{
    if (!direct_trace_check) return;
    CHECK(v==trace_export_vm && (uint16_t)f->tf.eax==0 && f->tf.cs==0x1000 && f->tf.eip==0x200);
    direct_trace_returns++;
}
static void test_d48_bios_and_export(void)
{
    struct dosvm *v=create();
    trace_export_vm=v;direct_trace_check=true;
    v->dpmi_enabled=true;
    struct v86_frame discovery={.tf={.eax=0x1687,.cs=0x1000,.eip=0x200,.user_ss=0x2000,.user_esp=0x8000}};
    CHECK(!interrupt_service(v,&discovery,0x2f) && direct_trace_returns==1);
    discovery.tf.eax=0x1100;
    CHECK(interrupt_service(v,&discovery,0x2f)==-ENOSYS && direct_trace_returns==1);
    direct_trace_check=false;
    d48_reset(&v->conformance_media,1,v->info.key.generation);
    struct v86_frame f=disk_frame(0x0800);f.tf.edx=0x80;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1) && (f.tf.edx&255)==2);
    uint8_t dap[16]={16,0};vm_w16(dap+2,1);vm_w16(dap+6,0x3000);
    CHECK(!dosvm_write(v,0x20000,dap,16));
    f=disk_frame(0x4200);f.tf.edx=0x81;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1));
    uint8_t sector[512];CHECK(!dosvm_read(v,0x30000,sector,512));
    CHECK(vm_u32(sector+454)==63 && vm_u32(sector+470)==D48_E_START);
    vm_w32(dap+8,D48_E_START+D48_DATA_SECTOR);
    struct dos_file_frame entry={.ax=0x3d00,.cs=0x1000,.ip=0x200,.ss=0x2000,.sp=0x300};
    struct dos_file_frame returned=entry;returned.ax=5;
    d48_open_enter(&v->conformance_media,&entry,true);d48_open_return(&v->conformance_media,&returned,true);
    for (unsigned i=0;i<6;i++) {
        vm_w16(dap+2,1);CHECK(!dosvm_write(v,0x20000,dap,16));
        f=disk_frame(0x4200);f.tf.edx=0x81;
        CHECK(!dosvm_bios(v,&f,0x13) && !!(f.tf.eflags&1)==(i<5));
        struct v86_frame reset=disk_frame(0);reset.tf.edx=0x81;
        CHECK(!dosvm_bios(v,&reset,0x13) && !(reset.tf.eflags&1));
    }
    CHECK(v->conformance_media.failures==5 && !v->conformance_media.armed);
    CHECK(!dosvm_read(v,0x30000,sector,512) && sector[0]=='F' && sector[511]=='F');
    d48_reset(&v->conformance_media,0,v->info.key.generation+1);
    f=disk_frame(0x4200);f.tf.edx=0x81;
    CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1));

    static struct dos_file trace_fixture;memset(&trace_fixture,0,sizeof(trace_fixture));
    v->files=&trace_fixture;
    trace_export_vm=v;evidence_hook=trace_export_reentry;
    unsigned before=records;
    for (unsigned i=0;i<1000;i++) {
        struct dos_file_trace row={.sequence=UINT64_MAX,.wall_us=UINT64_MAX,.monitor_us=UINT64_MAX,
            .generation=UINT32_MAX,.bytes=UINT32_MAX,.psp=0xffff,.origin=4,.vector=0x21};
        memset(&row.frame,0xff,sizeof(row.frame));
        row.terminated=i%3;
        trace_fixture.trace[0]=row;trace_fixture.trace_head=0;trace_fixture.trace_count=1;
        dosvm_file_dump(v);
    }
    CHECK(records==before+4000 && !trace_fixture.trace_count && !trace_fixture.dropped && v->file_trace_exported==1000 && !v->file_trace_busy);
    CHECK(strstr(last_record,"monitor_us=18446744073709551615") && strstr(last_record,"wall_us=18446744073709551615"));
    CHECK(max_record<=240);v->files=0;destroy(v,true);empty();
    puts("H51 VMM: PASS actual INT13 removable fixture/retry/reset and 1000 bounded lossless multipart trace exports");
}
