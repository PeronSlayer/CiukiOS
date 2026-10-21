/* Amendment 5: real DOS lifecycle/FAT/VFS and IPX; guest memory and disk
 * transports use the existing DOS file fixture. No guest CPU claimed.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main unused_dos_file_suite_main
#include "../dos_file_api_test.c"
#undef main
#include "../../../src/kernel/vm/dosvm_ipx.h"

static struct dosvm_ipx ipx;
static unsigned ipx_pins,ipx_writes,esrs,removals;
static bool fail_final_close;
static unsigned final_close_errors;
int px_legacy_closed(struct vfs *v,struct vfs_node *node) {
    CHECK(v==&space && node);
    if (!fail_final_close) return 0;
    /* Exercise the real final-close error return, after IPX revocation. */
    CHECK(!ipx.socket[1].open && !ipx_pins);
    final_close_errors++; return -FS_EIO;
}
static int ipx_pin(void *arg,uint32_t at,unsigned n,bool write,uint32_t *token) {
    (void)arg; (void)write;
    CHECK(at<sizeof(guest) && n<=sizeof(guest)-at);
    *token=++ipx_pins; return 0;
}
static void ipx_unpin(void *arg,uint32_t token) {
    (void)arg; CHECK(token && ipx_pins); ipx_pins--;
}
static int ipx_read(void *arg,uint32_t at,void *out,unsigned n) {
    memory_read(arg,at,out,n); return 0;
}
static int ipx_write(void *arg,uint32_t at,const void *in,unsigned n) {
    ipx_writes++; memory_write(arg,at,in,n); return 0;
}
static int ipx_send(void *arg,const uint8_t *frame,unsigned n) {
    (void)arg; (void)frame; (void)n; CHECK(false); return -FS_EIO;
}
static int ipx_upcall(void *arg,const struct dosvm_ipx_upcall *call) {
    (void)arg; (void)call; esrs++; return 1;
}
static void process_removed(void *arg,uint16_t psp) {
    (void)arg; CHECK(psp==0x3000 && bridge.process[1].used);
    CHECK(bridge.process[1].psp==psp && bridge.owning==psp);
    removals++; dosvm_ipx_process_exit(&ipx,psp);
}
static void ipx_call(unsigned fn,unsigned ax,unsigned dx,unsigned at) {
    struct dosvm_ipx_regs r={.ax=ax,.bx=fn,.dx=dx,.es=at>>4,.si=at&15};
    CHECK(dosvm_ipx_interrupt(&ipx,0x7a,&r)==1 && !(r.ax&255));
}
static void post(unsigned at,unsigned buffer) {
    uint8_t *e=guest+at; memset(e,0,42);
    fs_wr16(e+4,0x30); fs_wr16(e+6,0x3000); /* child's conventional ESR */
    e[10]=0x86; e[11]=0x9c;
    fs_wr16(e+34,1); fs_wr16(e+38,buffer>>4); fs_wr16(e+40,64);
    ipx_call(4,0,0,at); CHECK(e[8]==0xfe);
}
static void packet(uint8_t out[64]) {
    memset(out,0,64); memcpy(out,ipx.mac,6); out[6]=2; out[7]=99;
    out[12]=0x81; out[13]=0x37; out[14]=out[15]=255; out[17]=50;
    memcpy(out+24,out,6); memcpy(out+36,out+6,6); out[30]=0x86; out[31]=0x9c;
}
static void removal_error(bool close_error,enum dos_file_event event) {
    setup(); unsigned removed_before=removals,esrs_before=esrs;
    struct dosvm_ipx_ops netops={.pin=ipx_pin,.unpin=ipx_unpin,.read=ipx_read,
        .write=ipx_write,.send=ipx_send,.upcall=ipx_upcall};
    const uint8_t mac[6]={2,1,2,3,4,5};
    OK(dosvm_ipx_init(&ipx,fixture_generation,mac,&netops));
    bridge.ops.process_exit=process_removed;
    ipx.psp=0x2000; ipx_call(0,0,0x9d86,0); /* parent transient */
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_COMMIT,0x3000,0,expanded(0x3000)));
    unsigned handle=create_file("C:/CHILD.DAT");
    struct dos_file_frame written=transfer_file(0x4000,handle,16); CHECK(!(written.flags&1));
    unsigned sft=guest[0x30018+handle]; int native_handle=bridge.sft[sft].handle;
    CHECK(table.handles[native_handle]);
    ipx.psp=0x3000; ipx_call(0,0,0x9c86,0); /* child transient */
    ipx_call(0,255,0x9e86,0); /* child long-lived */
    post(0x31000,0x32000); post(0x31100,0x32100);
    uint8_t wire_frame[64]; packet(wire_frame);
    CHECK(dosvm_ipx_receive(&ipx,wire_frame,sizeof(wire_frame))==1); /* queues ESR */
    CHECK(!guest[0x31008] && !guest[0x31009] && guest[0x31108]==0xfe);
    CHECK(ipx_pins && !esrs);
    bridge.personality.live=true; bridge.personality.code_seg=0x9000;
    const uint8_t args[8]={1,0,8,0,0,0,0,0}; memcpy(guest+0x51000,args,8);
    struct dos_file_frame f=frame(event); f.bx=0x3000; f.si=0x1000;
    /* Rejected caller, malformed request, absent PSP and dead binding must
     * preserve both pending requests and socket ownership. */
    CHECK(dos_file_hook_event(&bridge,&f,0x9001)<0);
    guest[0x51006]=1; CHECK(dos_file_hook_event(&bridge,&f,0x9000)<0); guest[0x51006]=0;
    f.bx=0x4000; CHECK(dos_file_hook_event(&bridge,&f,0x9000)<0); f.bx=0x3000;
    bridge.dead=true; CHECK(dos_file_hook_event(&bridge,&f,0x9000)<0); bridge.dead=false;
    CHECK(removals==removed_before && ipx.socket[1].open && ipx_pins);
    unsigned errors_before=final_close_errors;
    if (close_error) fail_final_close=true;
    else guest[0x20018]=255; /* parent JFT preflight fails after child release */
    int e=dos_file_hook_event(&bridge,&f,0x9000);
    fail_final_close=false;
    CHECK(e==(close_error ? -FS_EIO : -FS_EBADF));
    CHECK(removals==removed_before+1 && !bridge.process[1].used);
    CHECK(bridge.selected==0x2000 && bridge.owning==0x2000);
    CHECK(!table.handles[native_handle] && !bridge.sft[sft].refs);
    CHECK(close_error ? final_close_errors==errors_before+1 && bridge.cleanup_status==-FS_EIO :
        final_close_errors==errors_before && !bridge.cleanup_status);
    CHECK(ipx.socket[0].open && !ipx.socket[1].open && ipx.socket[2].open);
    CHECK(!ipx_pins && !guest[0x31008] && guest[0x31009]==DOSVM_IPX_CANCELLED);
    CHECK(!guest[0x31108] && guest[0x31109]==DOSVM_IPX_CANCELLED);
    for (unsigned i=0;i<DOSVM_IPX_ECBS;i++) CHECK(!ipx.ecb[i].state);
    /* Reused child memory must remain untouched by a packet, timer service
     * or ESR dispatch after the failing teardown has returned. */
    memset(guest+0x31000,0x5a,0x2000); unsigned writes_before=ipx_writes;
    CHECK(!dosvm_ipx_receive(&ipx,wire_frame,sizeof(wire_frame)));
    dosvm_ipx_service(&ipx,1000); CHECK(!dosvm_ipx_dispatch(&ipx));
    CHECK(ipx_writes==writes_before && esrs==esrs_before);
    for (unsigned i=0x31000;i<0x33000;i++) CHECK(guest[i]==0x5a);
    if (!close_error) guest[0x20018]=0;
    bridge.ops.process_exit=0; dosvm_ipx_destroy(&ipx); teardown();
    CHECK(!ipx_pins);
    printf("Amendment 5: %s with %s: sockets/listener/queued ESR cancelled, parent/long-lived preserved PASS\n",
        event==DOS_FILE_PROCESS_EXIT ? "process exit" : "EXEC rollback",
        close_error ? "file close error" : "parent preflight error");
}
int main(void) {
    removal_error(true,DOS_FILE_PROCESS_EXIT);
    removal_error(false,DOS_FILE_PROCESS_EXIT);
    removal_error(true,DOS_FILE_EXEC_ROLLBACK);
    removal_error(false,DOS_FILE_EXEC_ROLLBACK);
    CHECK(!blk_request_pool_host_cleanup() && !allocations_live);
    printf("network PSP removal: %u checks passed\n",checks); return 0;
}
