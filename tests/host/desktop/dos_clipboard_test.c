/* SPDX-License-Identifier: MIT. Production DOS window policy; kernel boundary
 * is independently tested by dosvm_video_test.inc. */
#include "../../../apps/desktop/protocol.h"
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
int ciuki_dosvm(uint32_t,struct ciuki_dosvm_command *);
#include <unistd.h>
#include <sys/mman.h>
#define DOSBOX_SETTINGS_DIR "build/host/g5/clipboard-settings"
#include "../../../apps/dosbox/clipboard_settings.h"
static int test_close(int);
static int test_unmap(void *,size_t);
#define close test_close
#define munmap test_unmap
#include "../../../apps/dosbox/clipboard.c"
#undef close
#undef munmap
#include <assert.h>
#include <stdio.h>
static struct clip_store store;
static struct ciuki_message replies[128]; static unsigned head,tail;
static struct { void *p; struct ciuki_surface_info info; } surfaces[32];
static int paused,text_mode=1,backpressure;
static unsigned enters,alts,keys;
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t f)
{
    for (unsigned i=1;i<32;i++) if (!surfaces[i].p) {
        unsigned n=(w*h*4+4095)&~4095u; surfaces[i].p=calloc(1,n); assert(surfaces[i].p);
        surfaces[i].info=(struct ciuki_surface_info){sizeof(struct ciuki_surface_info),w,h,w*4,f,n}; return i;
    }
    return -1;
}
int ciuki_surface_info(int fd,struct ciuki_surface_info *out) { assert(surfaces[fd].p); *out=surfaces[fd].info; return 0; }
void *ciuki_surface_map(int fd,int flags) { (void)flags; return surfaces[fd].p; }
static int test_close(int fd) { assert(surfaces[fd].p); free(surfaces[fd].p); surfaces[fd].p=NULL; return 0; }
static int test_unmap(void *p,size_t n) { (void)p;(void)n; return 0; }
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)fd;(void)flags; struct desk_packet p; assert(desk_parse(m,1,NULL,&p));
    if (p.opcode==DESK_PONG) return 0;
    struct desk_clip_wire r; void *out=NULL,*data=m->fd_count ? surfaces[m->fds[0]].p : NULL;
    clip_request(&store,1,&p.clip,data,&r,&out); int attached=-1;
    if (!r.result && r.op==DESK_CLIP_GET && r.bytes) { uint32_t w,h; desk_clip_dims(r.bytes,&w,&h); attached=ciuki_surface_create(w,h,1); memcpy(surfaces[attached].p,out,r.bytes); }
    free(out); assert(tail-head<128); assert(!desk_clip_message(&replies[tail%128],2,0,&r,attached)); replies[tail++%128].sender_pid=9; return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{ (void)fd;(void)flags; if (head==tail) { errno=EAGAIN; return -1; } *m=replies[head++%128]; return 1; }
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *c)
{
    if (op==CIUKI_DOSVM_MARK) { paused=c->focused; return 0; }
    if (op==CIUKI_DOSVM_TEXT) {
        if (!text_mode) { errno=EOPNOTSUPP; return -1; }
        c->text.columns=4; c->text.rows=2; c->text.cell_height=16; memcpy(state.cells,"dog x   ",8); return 0;
    }
    assert(op==CIUKI_DOSVM_INPUT);
    if (backpressure) { backpressure=0; errno=EAGAIN; return -1; }
    keys++; if (c->event.value && c->event.code==CIUKI_KEY_ENTER) enters++;
    if (c->event.value && c->event.code==CIUKI_KEY_LEFTALT) alts++;
    return 0;
}
int main(void)
{
    uint32_t *pixels=malloc(640*480*4); assert(pixels); for (unsigned i=0;i<640*480;i++) pixels[i]=i;
    clip_init(&store); dos_clip_init(3,pixels); dos_clip_vm(1,1,9);
    assert(!dos_clip_action(1) && paused && dos_clip_marking());
    state.x0=state.y0=0; state.x1=3; state.y1=1; assert(dos_clip_dirty());
    assert(!dos_clip_action(2) && !paused && !dos_clip_marking()); assert(!strcmp(du_system_clipboard_text_get(),"dog\nx"));
    assert(!du_system_clipboard_text_set("A\n\xc3\xa9",4)); assert(!dos_clip_action(3)); backpressure=1;
    dos_clip_tick(1); assert(!state.key_at && !keys);
    for (unsigned i=0;i<100 && state.paste;i++) { state.next_key=0; dos_clip_tick(1); }
    assert(!state.paste && enters==1 && alts==1 && keys==14);
    text_mode=0; assert(!dos_clip_action(1)); state.x0=1; state.x1=2; state.y0=1; state.y1=2; assert(!dos_clip_action(2));
    void *dib=NULL; uint32_t bytes; assert(!du_clip_open() && !du_clip_get(8,&dib,&bytes) && !du_clip_close());
    uint32_t *rgb=NULL; unsigned w,h; assert(!clip_dib_decode(dib,bytes,&rgb,&w,&h) && w==2 && h==2 && rgb[0]==pixels[641]); free(rgb); free(dib);
    assert(!dos_clip_action(4)); struct desk_packet event={.opcode=DESK_BUTTON,.event={.code=1,.value=1}};
    assert(dos_clip_input(&event) && paused); event=(struct desk_packet){.opcode=DESK_KEY,.event={.code=CIUKI_KEY_ESCAPE,.value=1}};
    assert(dos_clip_input(&event) && !paused);
    unsigned char buffer[128]="DOS\x82"; uint32_t size=5;
    assert(dos_clip_execute(3,7,buffer,&size)==-EPERM);
    assert(!dos_clip_execute(1,0,buffer,&size)); assert(dos_clip_execute(1,0,buffer,&size)==-EBUSY);
    assert(!dos_clip_execute(2,0,buffer,&size) && !dos_clip_execute(3,7,buffer,&size));
    size=0; assert(!dos_clip_execute(4,13,buffer,&size) && size==10);
    size=0; assert(!dos_clip_execute(5,1,buffer,&size) && size==5 && buffer[3]==0xe9);
    assert(!dos_clip_execute(8,0,buffer,&size)); assert(!strcmp(du_system_clipboard_text_get(),"DOS\xc3\xa9"));
    assert(!dos_clip_execute(9,0,buffer,&size) && size==65536);
    strcpy((char *)buffer,"screen\r\ntext"); size=strlen((char *)buffer)+1;
    assert(!dos_clip_execute(CIUKI_DOSVM_CLIP_SCREEN,7,buffer,&size));
    assert(!strcmp(du_system_clipboard_text_get(),"screen\ntext"));
    assert(dosbox_clipboard_key("  C:/Game/GAME.EXE arg")==dosbox_clipboard_key("\"c:\\game\\game.exe\" other"));
    assert(dosbox_clipboard_key("C:/A/GAME.EXE")!=dosbox_clipboard_key("C:/B/GAME.EXE"));
    char previous[160]; assert(!dosbox_clipboard_path(dosbox_clipboard_key("G5b fixture"),previous,sizeof(previous)));
    (void)unlink(previous);
    dos_clip_settings("G5b fixture"); assert(!state.quick_edit && !state.exclusive);
    assert(!dos_clip_action(4) && state.quick_edit); assert(!dos_clip_action(5) && state.exclusive);
    event=(struct desk_packet){.opcode=DESK_BUTTON,.event={.code=1,.value=1}};
    assert(!dos_clip_input(&event) && !paused); /* Exclusive overrides automatic selection */
    struct dosbox_clipboard_settings settings;
    dosbox_clipboard_load(state.settings_key,&settings); assert(settings.quick_edit && settings.exclusive);
    settings.quick_edit=0; assert(!dosbox_clipboard_save(state.settings_key,&settings));
    dos_clip_settings("G5b fixture"); assert(!state.quick_edit && state.exclusive);
    char path[160]; assert(!dosbox_clipboard_path(state.settings_key,path,sizeof(path)));
    FILE *bad=fopen(path,"wb"); assert(bad); assert(fputs("DOS clipboard=1\nQuickEdit=1\nExclusive=1\nextra",bad)>=0); fclose(bad);
    dosbox_clipboard_load(state.settings_key,&settings); assert(!settings.quick_edit && !settings.exclusive);
    assert(!unlink(path) && !rmdir(DOSBOX_SETTINGS_DIR));
    dos_clip_destroy(); clip_destroy(&store); free(pixels);
    for (unsigned i=1;i<32;i++) assert(!surfaces[i].p);
    puts("PASS G5b DOS programmatic shared-store execution, OEM/ANSI synthesis, full-screen text publication, persisted QuickEdit/Exclusive round-trip and malformed settings");
    puts("PASS G5 DOS Mark/Copy text trimming, graphics DIB, QuickEdit, Esc resume, Unicode to OEM paste, CRLF once, Alt-numpad, queue retry"); fflush(stdout);
    return 0;
}
