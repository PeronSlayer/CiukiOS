/* Real copied wire/validator/BCI emitter to independent fake silicon.
 * GPL-2.0-only. Fixture callbacks model only hardware and the lease boundary. */
#include "savage3d_backend_fixture.inc"
#include "../../../../src/kernel/drivers/video/render3d_transport.h"
static unsigned wire_pid=7, lease_token, lease_generation, wire_presented;
static uint32_t frame_pixels[64*64];
static int present_wire(void *ctx,uint32_t pid,const struct ciuki_w32_render3d_args *a,
                        const void *data,uint32_t bytes)
{
    (void)ctx;
    if (pid!=7 || a->token!=lease_token || a->generation!=lease_generation) return -EACCES;
    memcpy(frame_pixels,data,bytes); wire_presented++; return 0;
}
void gl_kernel_setup(int qualified)
{
    CHECK(!allocations3d);
    prepare(32); m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d)); draw3d=copy3d=writes3d=memory3d=count3d=0;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.qualified=qualified};
    test_engine.render3d_qualified=qualified;
    test_engine.io.write32=write_engine3d;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    render3d_init(&scratch3d,&host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    gpu3d.consumer=&scratch3d; wire_pid=7;
    lease_token=1234; lease_generation=42; wire_presented=0;
}
void gl_kernel_teardown(void)
{
    CHECK(!render3d_close(&scratch3d)); CHECK(!allocations3d && !scratch3d.ram_bytes);
    finish(); CHECK(!failures);
}
void gl_kernel_revoke(void) { lease_token++; }
void gl_kernel_process_exit(void) { render3d_process_exit(&scratch3d,7); }
unsigned gl_kernel_draws(void) { return draw3d; }
unsigned gl_kernel_copies(void) { return copy3d; }
unsigned gl_kernel_writes(void) { return writes3d; }
unsigned gl_kernel_presents(void) { return wire_presented; }
const uint32_t *gl_kernel_pixels(void) { return frame_pixels; }
int w32_render3d_host_exchange(struct ciuki_w32_render3d_args *a,void *payload)
{
    /* Independent RAM snapshot is the syscall staging seam. No process
     * pointer reaches the service; READ/QUERY publish only on success. */
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
