/* SPDX-License-Identifier: MIT */
#include "terminal.h"
#include <ciuki/net.h>
#include <ciuki/audio.h>
#include <ciuki/proc.h>
#include <ciuki/display.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct native_terminal {
    struct terminal app; struct du_ui ui; struct du_window window;
    uint32_t serial;
    struct { uint32_t serial, opcode; } pending[16];
    uint64_t next_pump;
};
static int device(const char *path,uint32_t op,void *record,unsigned bytes)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0)return -errno;
    int r=ciuki_devctl(fd,op,record,bytes); int error=errno; close(fd); return r ? -error : 0;
}
static int net(void *ctx,struct ciuki_net_state *s) { (void)ctx; return device("/dev/net",NET_GET_STATE,s,sizeof(*s)); }
static int mixer(void *ctx,struct ciuki_mixer_master *s) { (void)ctx; return device("/dev/mixer",MIXER_GET_MASTER,s,sizeof(*s)); }
static int processes(void *ctx,void *s,unsigned bytes) { (void)ctx; int n=ciuki_proc_list(s,bytes); return n<0 ? -errno : n; }
static int display(void *ctx,struct ciuki_display_info *s) { (void)ctx; return ciuki_display_query(s); }
static int send(struct native_terminal *x,struct ciuki_message *m)
{
    unsigned i; for(i=0;i<16 && x->pending[i].serial;i++) {} if(i==16)return -EAGAIN;
    if(!++x->serial)++x->serial; desk_put32(m->data+4,x->serial);
    if(du_window_queue(&x->window,m))return -errno;
    x->pending[i].serial=x->serial; x->pending[i].opcode=m->data[1]; return 0;
}
static int volume(void *ctx,uint32_t l,uint32_t r,uint32_t mute)
{
    struct ciuki_message m; desk_message(&m,DESK_MIXER_SET,20);
    desk_put32(m.data+8,l); desk_put32(m.data+12,r); desk_put32(m.data+16,mute); return send(ctx,&m);
}
static int launch(void *ctx,const char *name,const char *path)
{
    unsigned n=(unsigned)strlen(name),a=path ? (unsigned)strlen(path) : 0;
    if(n>DESK_TITLE_MAX || a>DESK_ARG_MAX)return -ENAMETOOLONG;
    struct ciuki_message m; desk_message(&m,DESK_LAUNCH,12+n+(a ? 4+a : 0)); desk_put32(m.data+8,n); memcpy(m.data+12,name,n);
    if(a) { desk_put32(m.data+12+n,a); memcpy(m.data+16+n,path,a); } return send(ctx,&m);
}
static int kill(void *ctx,uint32_t pid)
{ struct ciuki_message m; desk_message(&m,DESK_END_TASK,16); desk_put32(m.data+12,pid); return send(ctx,&m); }
static int power(void *ctx,unsigned action)
{ struct ciuki_message m; desk_message(&m,DESK_TERMINAL_POWER,12); desk_put32(m.data+8,action); return send(ctx,&m); }
static int cwd(void *ctx,const char *path) { (void)ctx; return chdir(path) ? -errno : 0; }
static time_t now(void *ctx) { (void)ctx; return time(NULL); }
static void quit(void *ctx) { struct native_terminal *x=ctx; x->app.quit=1; x->app.interrupted=1; du_window_close(&x->window); }
static int close_request(struct du_window *w,void *ctx) { (void)w; quit(ctx); return 1; }
/* G3: agree without interrupting the running command; END_SESSION(1) closes. */
static int query_end_session(struct du_window *w,uint32_t flags,void *ctx) { (void)w; (void)flags; (void)ctx; return DU_SESSION_AGREE; }
static void pump(void *ctx)
{
    struct native_terminal *x=ctx; uint64_t tick=du_now(); if(tick<x->next_pump)return; x->next_pump=tick+10000000ull;
    if(du_window_turn(&x->window,tick)<=0) { x->app.quit=1; x->app.interrupted=1; }
}
static void message(struct du_window *w,const struct desk_packet *p,void *ctx)
{
    (void)w; struct native_terminal *x=ctx;
    if(p->opcode!=DESK_TASK_RESULT)return;
    for(unsigned i=0;i<16;i++)if(x->pending[i].serial==p->serial && x->pending[i].opcode==p->request) {
        x->pending[i].serial=0;
        char *capture=x->app.capture; x->app.capture=NULL; /* desktop replies are independent grid output */
        if(p->result<0)term_printf(&x->app,"Desktop: %s (error %d)\n",strerror(-p->result),-p->result);
        else if(p->request==DESK_LAUNCH)term_printf(&x->app,"Started PID %d.\n",p->result);
        else term_write(&x->app,"Desktop request accepted.\n"); x->app.capture=capture; return;
    }
}
int main(int argc,char **argv)
{
    int channel=3; const char *script=NULL;
    for(int i=1;i<argc;i++) {
        if(!strncmp(argv[i],"--channel-fd=",13)) { char *end; long fd=strtol(argv[i]+13,&end,10); if(!argv[i][13] || *end || fd<3 || fd>=CIUKI_OPEN_MAX)return 2; channel=(int)fd; }
        else if(script || argv[i][0]=='-')return 2; else script=argv[i];
    }
    struct native_terminal *x=calloc(1,sizeof(*x)); if(!x)return 1;
    if(du_window_open_resizable(&x->window,&x->ui,channel,TERM_WIDTH,TERM_HEIGHT,"Terminal",304,168)) { free(x); return 1; }
    du_help_topic(&x->ui, "terminal");   /* G15: F1 opens this program's Help topic */
    struct term_host h={.fs=*fm_native_vfs(),.ctx=x,.net=net,.mixer=mixer,.volume=volume,.processes=processes,.display=display,
        .launch=launch,.kill=kill,.power=power,.cwd=cwd,.now=now,.pump=pump,.quit=quit};
    if(term_attach(&x->app,&x->ui,&h)) { du_window_destroy(&x->window); free(x); return 1; }
    x->window.user=x; x->window.message=message; x->window.close_request=close_request;
    x->window.query_end_session=query_end_session;
    if(chdir("/Documents"))term_printf(&x->app,"Working directory: %s\n",strerror(errno));
    if(script) { x->app.busy=1; int r=term_lua(&x->app,script,0,NULL); x->app.busy=0; if(r)term_printf(&x->app,"Lua: %s\n",strerror(-r)); }
    int r=du_run(&x->window,NULL,NULL); term_detach(&x->app); du_window_destroy(&x->window); free(x); return r<0 ? 1 : 0;
}
