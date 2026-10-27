/* f7-14: the kernel side of the native GL replay. The copied W32_RENDER3D
 * wire, the production render3d service, validator, Savage BCI emitter and
 * the present path run against the independent fake silicon of
 * savage3d_backend_test.c; only the syscall copy and the lease are modelled.
 * GPL-2.0-only (it includes the Savage fixture). */
#include "savage3d_backend_fixture.inc"
#include "../../src/kernel/drivers/video/render3d_transport.h"
#include <ciuki/present_abi.h>
static unsigned wire_pid=7, lease_token, lease_generation, gpu_presents, cpu_presents;
static unsigned mode_width=640, mode_height=480, transport_missing, exchanges, texture_full;
static unsigned target_uploads, target_reads;
static uint32_t frame_pixels[1024*768];
void gl_native_mode(unsigned w,unsigned h) { mode_width=w; mode_height=h; }
void gl_native_transport(int present) { transport_missing=!present; }
/* The kernel's texture heap is exhausted: every texture creation fails. */
void gl_native_texture_full(int full) { texture_full=full; }
void gl_native_lease(unsigned token,unsigned generation) { lease_token=token; lease_generation=generation; }
void gl_native_revoke(void) { lease_token++; }
void gl_native_process_exit(void) { render3d_process_exit(&scratch3d,wire_pid); }
unsigned gl_native_draws(void) { return draw3d; }
unsigned gl_native_copies(void) { return copy3d; }
unsigned gl_native_writes(void) { return writes3d; }
unsigned gl_native_gpu_presents(void) { return gpu_presents; }
unsigned gl_native_cpu_presents(void) { return cpu_presents; }
unsigned gl_native_exchanges(void) { return exchanges; }
/* Full colour/depth target transfers (CIUKI_R3D_UPLOAD / CIUKI_R3D_READ). */
unsigned gl_native_uploads(void) { return target_uploads; }
unsigned gl_native_reads(void) { return target_reads; }
const uint32_t *gl_native_frame(void) { return frame_pixels; }
static int present_wire(void *ctx,uint32_t pid,const struct ciuki_w32_render3d_args *a,
                        const void *data,uint32_t bytes)
{
    (void)ctx;
    if (pid!=wire_pid || !lease_token || a->token!=lease_token || a->generation!=lease_generation) return -EACCES;
    /* f7-15 transport: NULL pixels ask for the direct native scanout only.
     * This model has none (the H1/T23 GL fixture covers it), so the kernel
     * reads the completed linear copy back and calls again with it. */
    if (!data) return -EOPNOTSUPP;
    CHECK(bytes<=sizeof(frame_pixels) && bytes==a->width*a->height*4);
    memcpy(frame_pixels,data,bytes); gpu_presents++; return 0;
}
/* qualified: 0 = no positive boot probe (QEMU and every unqualified
 * device), 2 = probe and full-size qualification at the current mode. */
void gl_native_setup(int qualified)
{
    CHECK(!allocations3d);
    prepare(32); m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d)); draw3d=copy3d=writes3d=memory3d=count3d=0;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.qualified=qualified};
    test_engine.render3d_qualified=qualified;
    test_engine.io.write32=write_engine3d;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    if (qualified==2) {
        gpu3d.texture_bytes=480*1024;
        m.exact_clock=true;
        gpu3d.mode_width=mode_width; gpu3d.mode_height=mode_height; gpu3d.fullsize_testing=true;
        fullsize_resources3d(&scratch3d,&host,mode_width,mode_height,true);
        fullsize_resources3d(&reference3d,&host,mode_width,mode_height,false);
        gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
        CHECK(!savage3d_fullsize_probe(&gpu3d,&reference3d));
        CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d)); gpu3d.consumer=0;
    }
    render3d_init(&scratch3d,&host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    gpu3d.consumer=&scratch3d;
    lease_token=lease_generation=gpu_presents=cpu_presents=exchanges=0; transport_missing=texture_full=0;
}
void gl_native_teardown(void)
{
    CHECK(!render3d_close(&scratch3d)); CHECK(!allocations3d && !scratch3d.ram_bytes);
    finish(); CHECK(!failures);
}
/* The CIUKI_SYS_W32 copy: an independent RAM snapshot of the payload, as
 * w32_render3d_call stages it; no process pointer reaches the service. */
int ciuki_gl_host_exchange(struct ciuki_w32_render3d_args *a,void *payload)
{
    if (transport_missing) return -ENOSYS;
    exchanges++;
    if (texture_full && a->action==CIUKI_R3D_TEXTURE) return -ENOMEM;
    target_uploads+=a->action==CIUKI_R3D_UPLOAD; target_reads+=a->action==CIUKI_R3D_READ;
    CHECK(a->header.operation==W32_RENDER3D && a->header.size==sizeof(*a));
    struct ciuki_w32_render3d_args copied=*a;
    void *data=copied.bytes ? calloc(1,copied.bytes) : 0;
    bool output=a->action==CIUKI_R3D_QUERY || a->action==CIUKI_R3D_READ;
    if (copied.bytes && !data) return -ENOMEM;
    if (copied.bytes && !output && a->action!=CIUKI_R3D_PRESENT) {
        CHECK(payload); memcpy(data,payload,copied.bytes);
    }
    struct render3d_transport t={&scratch3d,0,present_wire};
    int e=render3d_transport_dispatch(&t,wire_pid,&copied,data,copied.bytes);
    if (!e && output) memcpy(payload,data,copied.bytes);
    if (!e) *a=copied;
    free(data); return e;
}
/* W32_DISPLAY_FRAME of the lease holder: token and generation as the kernel
 * checks them (display_present.c); the XRGB surface is the lease size. */
int32_t ciuki_gl_host_display_frame(struct ciuki_w32_display_args *a,const void *payload)
{
    CHECK(a->header.operation==W32_DISPLAY && a->header.size==sizeof(*a) && a->action==W32_DISPLAY_FRAME);
    if (!lease_token || a->token!=lease_token || a->generation!=lease_generation) { a->error=EACCES; return 0; }
    CHECK(payload && (uint32_t)(uintptr_t)payload==a->address);
    memcpy(frame_pixels,payload,(size_t)mode_width*mode_height*4);
    cpu_presents++; a->result=1; return 0;
}
