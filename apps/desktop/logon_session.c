/* SPDX-License-Identifier: MIT
 * sec-user-03: the desktop of a session launched by the trusted LOGON
 * service (SESSION_LAUNCH, sec-k01/k07). Native only; the shell model sees
 * d->logon_session, d->session_lock and desk_session_action (logon.inc).
 *
 * - Detection: the kernel-generated CIUKI_LAUNCH_* labels (descriptive only;
 *   the kernel checks the readiness sender and the installed descriptors).
 * - Readiness: one 16-byte message {1, launch id, launch generation, 0} on
 *   CIUKI_LAUNCH_READY_FD once the profile is loaded and the first frame is
 *   composed (it stays concealed until LOGON's activation).
 * - Profile: the committed profile from the kernel environment (HOME,
 *   CIUKI_PROFILE, CIUKI_PROFILE_SETTINGS); the account name from LOGON.
 * - Lock: SESSION_LOCK; Ctrl+Alt+Del choices: SESSION_EVENT after the RESYNC
 *   of the activation; end of session: SESSION_EVENT notice, then the
 *   session's w32srv is asked to flush and exit (SIGTERM) and the desktop
 *   exits; the kernel ends the session and LOGON collects it.
 */
#include "desktop.h"
#include "shell.h"
#include "logon_session.h"
#include "profile.h"
#include "../logon/logon_proto.h"
#include "../logon/sys_ciuki.h"
#include <ciuki/channel.h>
#include <ciuki/proc.h>
#include <ciuki/security.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CALL_MS 1500u
#define W32_EXIT_MS 10000u

static struct lgn_client logon_link;
static int ready_fd=-1, ready_sent;
static uint32_t launch_id, launch_generation, session_id;

static int env_u32(const char *name, uint32_t lo, uint32_t hi, uint32_t *out)
{
    const char *v=getenv(name);
    if (!v || !*v) return -1;
    char *end; unsigned long n=strtoul(v,&end,10);
    if (*end || n<lo || n>hi) return -1;
    *out=(uint32_t)n;
    return 0;
}
int desk_logon_detect(int *display, int *input)
{
    uint32_t purpose, ready, id, generation, disp, in, session;
    if (env_u32("CIUKI_LAUNCH_PURPOSE",CIUKI_SECURITY_LAUNCH_DESKTOP,CIUKI_SECURITY_LAUNCH_DESKTOP,&purpose) ||
        env_u32("CIUKI_LAUNCH_READY_FD",3,CIUKI_OPEN_MAX-1,&ready) ||
        env_u32("CIUKI_LAUNCH_ID",1,UINT32_MAX,&id) || env_u32("CIUKI_LAUNCH_GENERATION",1,UINT32_MAX,&generation) ||
        env_u32("CIUKI_DISPLAY_FD",3,CIUKI_OPEN_MAX-1,&disp) || env_u32("CIUKI_INPUT_FD",3,CIUKI_OPEN_MAX-1,&in) ||
        env_u32("CIUKI_SESSION_ID",1,UINT32_MAX,&session) || disp==in) return 0;
    ready_fd=(int)ready; launch_id=id; launch_generation=generation; session_id=session;
    *display=(int)disp; *input=(int)in;
    lgn_client_init(&logon_link,&lgn_client_ciuki,LGN_SERVICE_LOGON,LGN_ROLE_LOGON);
    /* The session's W32SRV learns which process is this session's desktop
     * (its Win32 end-session writer) from this link's kernel record. The
     * link stays open: an unaccepted link closed early is reclaimed. */
    struct ciuki_security_link request={.version=1,.size=64,.service=CIUKI_SECURITY_SERVICE_W32SRV}, link;
    memset(&link,0,sizeof(link));
    struct ciuki_security_args a={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_CONNECT};
    int e=lgn_ciuki_security(NULL,&a,&request,64,&link,64);
    if (!e && link.server_role!=CIUKI_SECURITY_ROLE_W32SRV) { lgn_ciuki_close(NULL,link.endpoint_fd); e=-EPERM; }
    if (e) fprintf(stderr,"[desktop] window server link unavailable: %d\n",e);
    return 1;
}
static int call(uint32_t op, const void *body, size_t n, void *reply, size_t cap, size_t *used)
{
    int status=0;
    int e=lgn_client_call(&logon_link,op,0,body,n,reply,cap,used,&status,CALL_MS);
    if (e) { lgn_client_close(&logon_link); return e; }
    return status;
}
static int event(uint32_t notice, struct lgn_session_event *ev)
{
    struct lgn_session_notice n={notice,session_id}; uint8_t body[LGN_SESSION_NOTICE_BYTES], reply[LGN_SESSION_EVENT_BYTES];
    size_t used=0;
    if (lgn_notice_encode(&n,body,sizeof(body))) return -EINVAL;
    int e=call(LGN_OP_SESSION_EVENT,body,sizeof(body),reply,sizeof(reply),&used);
    if (!e && (used!=sizeof(reply) || lgn_event_decode(ev,reply,used) || ev->session!=session_id)) e=-EIO;
    return e;
}
static int copy_path(char *out, size_t cap, const char *a, const char *b)
{
    int n=snprintf(out,cap,"%s%s",a,b);
    return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
void desk_logon_profile(struct desktop *d)
{
    struct user_profile *p=&d->sh.profile;
    memset(p,0,sizeof(*p));
    const char *root=getenv("CIUKI_PROFILE"), *home=getenv("HOME"), *settings=getenv("CIUKI_PROFILE_SETTINGS");
    if (!root || strncmp(root,"/Windows/Profiles/",18) || strstr(root,"/..")) root=NULL;
    if (root) (void)copy_path(p->root,sizeof(p->root),root,"");
    if (home && !strncmp(home,"/Windows/Profiles/",18) && !strstr(home,"/..")) (void)copy_path(p->documents,sizeof(p->documents),home,"");
    else if (root) (void)copy_path(p->documents,sizeof(p->documents),root,"/Documents");
    else strcpy(p->documents,"/Documents");
    /* sec-user-04: the user's own Desktop and Start Menu (Windows XP
     * CSIDL_DESKTOPDIRECTORY <profile>/Documents/Desktop, CSIDL_PROGRAMS
     * <profile>/Start Menu/Programs, contract 8.2); sec-user-05's merges show
     * the All Users items (/Documents/Desktop, /Windows/Start Menu) beside
     * them. Without a committed root the common folders stay. */
    if (!root || copy_path(p->desktop,sizeof(p->desktop),root,"/Documents/Desktop")) strcpy(p->desktop,"/Documents/Desktop");
    if (!root || copy_path(p->programs,sizeof(p->programs),root,"/Start Menu/Programs")) strcpy(p->programs,"/Windows/Start Menu/Programs");
    if (settings && root && !strncmp(settings,root,strlen(root)) && !strstr(settings,"/.."))
        (void)copy_path(p->settings,sizeof(p->settings),settings,"");
    else if (root) (void)copy_path(p->settings,sizeof(p->settings),root,"/Settings/desktop.cfg");
    else strcpy(p->settings,"/system/settings/desktop.cfg");
    char dir[USERS_PATH]; snprintf(dir,sizeof(dir),"%s",p->settings);
    char *slash=strrchr(dir,'/');
    if (slash) { *slash=0; (void)mkdir(dir,0700); (void)copy_path(p->temporary,sizeof(p->temporary),dir,"/desktop.new"); }
    /* fx-hlis5: the session's TEMP (profile.h profile_temp_ready_at). */
    const char *temp_root=profile_root_path();
    if (temp_root && profile_temp_ready_at(temp_root,profile_temp()))
        fprintf(stderr,"[desktop] profile temp unavailable error=%d\n",-errno);
    struct lgn_session_event ev;
    if (!event(LGN_NOTICE_QUERY,&ev)) {
        memcpy(p->name,ev.name,sizeof(p->name)-1);
    } else fprintf(stderr,"[desktop] logon service unavailable for the account name\n");
    fprintf(stderr,"[desktop] logon session %u profile=%s settings=%s\n",session_id,p->root,p->settings);
}
void desk_logon_ready(struct desktop *d)
{
    (void)d;
    if (ready_sent || ready_fd<0) return;
    struct ciuki_message m; memset(&m,0,sizeof(m));
    uint32_t words[4]={1,launch_id,launch_generation,0};
    memcpy(m.data,words,16); m.length=16;
    int r=ciuki_channel_send(ready_fd,&m,DONTWAIT);
    ready_sent=1;
    fprintf(stderr,"[desktop] session ready launch=%u result=%d\n",launch_id,r<0 ? -errno : 0);
}
int desk_logon_ready_sent(void) { return ready_sent; }
void desk_logon_poll(struct desktop *d)
{
    if (!d->logon_session || !d->logon_query) return;
    d->logon_query=0;
    struct lgn_session_event ev;
    if (event(LGN_NOTICE_QUERY,&ev)) return;
    if (ev.name[0] && strcmp(ev.name,d->sh.profile.name)) memcpy(d->sh.profile.name,ev.name,sizeof(d->sh.profile.name)-1);
    if (ev.action) desk_session_action(d,ev.action);
}
int desk_logon_lock(struct desktop *d)
{
    (void)d;
    uint8_t body[32], reply[LGN_PROGRESS_BYTES]; size_t used=0; memset(body,0,sizeof(body));
    uint32_t words[3]={1,1,session_id};   /* SESSION_LOCK: action lock, own session, console 0 */
    memcpy(body,words,12);
    struct lgn_progress p;
    int e=call(LGN_OP_SESSION_LOCK,body,sizeof(body),reply,sizeof(reply),&used);
    if (!e && (used!=sizeof(reply) || lgn_progress_decode(&p,reply,used))) e=-EIO;
    return e ? e : p.status;
}
/* sec-user-04: SAVER/STANDBY/PASSWORD notices (logon_proto.h). */
int desk_logon_secure(struct desktop *d, unsigned notice, unsigned *flags)
{
    (void)d;
    struct lgn_session_event ev;
    int e=event(notice,&ev);
    if (!e && flags) *flags=ev.flags;
    if (e) fprintf(stderr,"[desktop] logon notice %u failed: %d\n",notice,e);
    return e;
}
static struct { struct ciuki_proc_system system; struct ciuki_proc_record records[CIUKI_PROCESS_MAX]; } snapshot;
/* The session's LOGON-launched window server: same credential, fixed image. */
static uint32_t session_w32srv(void)
{
    int n=ciuki_proc_list(&snapshot,sizeof(snapshot));
    for (int i=0;i<n && i<CIUKI_PROCESS_MAX;i++) {
        const struct ciuki_proc_record *r=&snapshot.records[i];
        struct ciuki_credential c;
        if (r->state!=CIUKI_PROC_LIVE || strncmp(r->name,"/System/Win32/w32srv",sizeof(r->name))) continue;
        if (ciuki_security_process(r->pid,&c) || c.session_id!=session_id) continue;
        return r->pid;
    }
    return 0;
}
void desk_logon_end(struct desktop *d, unsigned notice)
{
    (void)d;
    struct lgn_session_event ev;
    int e=event(notice,&ev);
    fprintf(stderr,"[desktop] ending logon session %u notice=%u result=%d\n",session_id,notice,e);
    /* w32srv flushes the user's registry on SIGTERM; its exit ends the
     * session (the kernel then stops this desktop too). */
    uint32_t pid=session_w32srv();
    if (pid && !ciuki_proc_terminate((ciuki_pid_t)pid,SIGTERM,-1)) {
        struct timespec step={.tv_sec=0,.tv_nsec=50000000L};
        for (unsigned waited=0;waited<W32_EXIT_MS && session_w32srv()==pid;waited+=50) nanosleep(&step,NULL);
    }
    exit(0);
}
