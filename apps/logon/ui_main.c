/* SPDX-License-Identifier: MIT
 * Sealed SYSTEM LOGON_UI image (/System/Services/logon-ui). PID 1 installs
 * its private console and the trusted display/input pair before publication
 * and names them in CIUKI_DISPLAY_FD/CIUKI_INPUT_FD (sec-k07); it registers
 * no service and owns no authority. It paints only while the trusted domain
 * owns the console: a present answers EBUSY otherwise (or while PID 1's
 * neutral frame is still owed) and is retried. Input reads come only from
 * the trusted domain; RESYNC or ATTENTION after a domain switch wakes it.
 */
#include "ui.h"
#include "sys_ciuki.h"
#include "../desktop/theme.h"
#include "../libdeskui/keymap.h"
#include <ciuki/surface.h>
#include <ciuki/channel.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CALL_TIMEOUT_MS 30000u

static struct lui ui;
static struct lgn_client logon_link, auth_link;
static uint32_t design[LUI_W * LUI_H];
static uint32_t portrait_pixels[256 * 256];
static struct { int display, input, surface; unsigned width, height; size_t mapped; uint32_t *pixels; int mouse_x, mouse_y; } N;

static uint64_t now_ms(void *ctx) { (void)ctx; return lgn_client_ciuki.now_ms(NULL); }
static int call(void *ctx, unsigned service, uint32_t op, const void *body, size_t n, void *reply, size_t cap,
                size_t *used, int *status)
{
    (void)ctx;
    struct lgn_client *c=service==LGN_SERVICE_AUTH ? &auth_link : &logon_link;
    int e=0;
    for (unsigned attempt=0;attempt<2;attempt++) {   /* one reconnect after a LOGON/ACCOUNT restart */
        e=lgn_client_call(c,op,0,body,n,reply,cap,used,status,CALL_TIMEOUT_MS);
        if (!e || (e!=-ESRCH && e!=-EPIPE)) break;
    }
    return e;
}
static void log_line(void *ctx, const char *line) { (void)ctx; lgn_log_ciuki(line); }
static const struct lui_host host={NULL,now_ms,call,log_line};

static int descriptor(const char *name)
{
    const char *v=getenv(name);
    if (!v || !*v) return -1;
    char *end; long n=strtol(v,&end,10);
    return *end || n<3 || n>=64 ? -1 : (int)n;
}
static void surface_close(void)
{
    if (N.pixels) munmap(N.pixels,N.mapped);
    if (N.surface>=0) close(N.surface);
    N.pixels=NULL; N.surface=-1;
}
/* The framebuffer may change size while a session owns it (display modes). */
static int surface_open(void)
{
    struct ciuki_framebuffer_info fb={.size=sizeof(fb)};
    if (ciuki_display_info(N.display,&fb)) return -errno;
    if (fb.width<LUI_W || fb.height<LUI_H || fb.width>2048 || fb.height>2048) return -ENOTSUP;
    if (N.pixels && fb.width==N.width && fb.height==N.height) return 0;
    surface_close();
    N.surface=ciuki_surface_create(fb.width,fb.height,CIUKI_SURFACE_XRGB8888);
    if (N.surface<0) return -errno;
    N.mapped=((size_t)fb.width*fb.height*4+4095)&~(size_t)4095;
    void *p=ciuki_surface_map(N.surface,PROT_READ|PROT_WRITE);
    if (p==MAP_FAILED) { int e=-errno; surface_close(); return e; }
    N.pixels=p; N.width=fb.width; N.height=fb.height;
    return 0;
}
static int present(void)
{
    if (surface_open()) return -EAGAIN;
    struct du_canvas c={design,LUI_W,LUI_H,{0,0,LUI_W,LUI_H}};
    lui_paint(&ui,&c);
    unsigned ox=(N.width-LUI_W)/2, oy=(N.height-LUI_H)/2;
    for (unsigned y=0;y<N.height;y++) {
        uint32_t *row=N.pixels+(size_t)y*N.width;
        if (y<oy || y>=oy+LUI_H) { for (unsigned x=0;x<N.width;x++) row[x]=LUI_BACKGROUND; continue; }
        for (unsigned x=0;x<ox;x++) row[x]=LUI_BACKGROUND;
        memcpy(row+ox,design+(size_t)(y-oy)*LUI_W,LUI_W*4);
        for (unsigned x=ox+LUI_W;x<N.width;x++) row[x]=LUI_BACKGROUND;
    }
    struct ciuki_rect r={.src_x=0,.src_y=0,.dst_x=0,.dst_y=0,.width=N.width,.height=N.height};
    return ciuki_present(N.display,N.surface,&r) ? -errno : 0;
}
static void load_portrait(const uint32_t **out)
{
    FILE *f=fopen("/system/assets/ciuki-portrait.xrgb","rb");
    *out=NULL;
    if (!f) return;
    if (fread(portrait_pixels,1,sizeof(portrait_pixels),f)==sizeof(portrait_pixels)) *out=portrait_pixels;
    fclose(f);
}
/* The machine keyboard layout (desktop.cfg keyboard=us|it), US otherwise. */
static int machine_layout(void)
{
    FILE *f=fopen("/system/settings/desktop.cfg","r");
    if (!f) return DU_LAYOUT_US;
    char line[128]; int layout=DU_LAYOUT_US;
    while (fgets(line,sizeof(line),f)) {
        if (strncmp(line,"keyboard=",9)) continue;
        line[strcspn(line,"\r\n")]=0;
        int found=du_layout_find(line+9);
        if (found>=0) layout=found;
    }
    fclose(f);
    return layout;
}
static void sleep_ms(unsigned ms)
{ struct timespec t={.tv_sec=(time_t)(ms/1000u),.tv_nsec=(long)(ms%1000u)*1000000L}; nanosleep(&t,NULL); }

int main(void)
{
    memset(&N,0,sizeof(N)); N.surface=-1;
    N.display=descriptor("CIUKI_DISPLAY_FD"); N.input=descriptor("CIUKI_INPUT_FD");
    if (N.display<0 || N.input<0 || N.display==N.input) {
        lgn_log_ciuki("[logon-ui] trusted display/input descriptors missing; idle");
        for (;;) sleep_ms(60000);
    }
    int flags=fcntl(N.input,F_GETFL);
    if (flags>=0) (void)fcntl(N.input,F_SETFL,flags|O_NONBLOCK);
    const uint32_t *portrait;
    load_portrait(&portrait);
    lgn_client_init(&logon_link,&lgn_client_ciuki,LGN_SERVICE_LOGON,LGN_ROLE_LOGON);
    lgn_client_init(&auth_link,&lgn_client_ciuki,LGN_SERVICE_AUTH,LGN_ROLE_ACCOUNT);
    lui_init(&ui,&host,portrait,machine_layout());
    N.mouse_x=LUI_W/2; N.mouse_y=LUI_H/2;
    lgn_log_ciuki("[logon-ui] trusted logon screen starting");
    int owed=1;
    for (;;) {
        struct ciuki_input_event events[32];
        int n=ciuki_input_read(N.input,events,32);
        for (int i=0;i<n;i++) {
            struct ciuki_input_event e=events[i];
            if (e.type==CIUKI_INPUT_MOTION) {
                N.mouse_x+=e.value; N.mouse_y+=e.value2;
                if (N.mouse_x<0) N.mouse_x=0;
                if (N.mouse_x>=LUI_W) N.mouse_x=LUI_W-1;
                if (N.mouse_y<0) N.mouse_y=0;
                if (N.mouse_y>=LUI_H) N.mouse_y=LUI_H-1;
                e.value=N.mouse_x; e.value2=N.mouse_y;
            }
            if (e.type==CIUKI_INPUT_RESYNC || e.type==CIUKI_INPUT_ATTENTION) owed=1;
            lui_input(&ui,&e);
        }
        int was_visible=ui.visible;
        lui_tick(&ui);
        if (ui.visible && !was_visible) owed=1;
        if (ui.visible && (ui.dirty || owed)) owed=present()!=0;
        sleep_ms(ui.visible ? (owed ? 10u : 20u) : 100u);
    }
}
