/* Path-delegated native servers and private channel endpoints per client.
 * f0-45 remains the only source of scheduling policy and input boosts.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/display.h>
#include <ciuki/security.h>
#include <ciuki/signal.h>
/* Private launch permission, not a registrable server role. Never inherited. */
enum { NATIVE_CONTROL_REGISTRY = 3 };
static bool registry_desktop(struct process *p)
{
    return p && p->state==PROC_LIVE && p->personality==PROC_NATIVE &&
        p->pid==supervisor_desktop_pid() && p->w32 &&
        p->w32->service==W32_SERVICE_DESKTOP && grants_display_owner(p);
}
struct service_link {
    struct service_link *next;
    struct process *client, *server;
    uint32_t service;
    int32_t client_fd, server_fd;
    struct w32_object *charge;
    struct proc_object *client_end, *server_end;
    bool accepted;
};
static struct service_link *links;
static uint32_t service_pid[2];
extern void display_present_process_exit(struct process *) __attribute__((weak));
#pragma weak secure_service_notify /* sec-k07: w32srv accept notification */
bool w32_service_desktop_client(struct process *desktop, uint32_t pid)
{
    if (!desktop || desktop->pid!=supervisor_desktop_pid() ||
        desktop->state!=PROC_LIVE || !desktop->w32 ||
        desktop->w32->service!=W32_SERVICE_DESKTOP ||
        (security_roles_active() ? security_role(desktop)!=CIUKI_SECURITY_ROLE_DESKTOP : service_pid[0]!=desktop->pid)) return false;
    for (struct service_link *l=links;l;l=l->next)
        if (l->server==desktop && l->client->pid==pid && l->accepted &&
            l->client->state==PROC_LIVE && security_same(desktop,l->client) &&
            security_process_live(l->client) && l->service==W32_SERVICE_DESKTOP &&
            desktop->fds && l->client->fds &&
            desktop->fds[l->server_fd].object==l->server_end &&
            l->client->fds[l->client_fd].object==l->client_end) return true;
    return false;
}
void w32_service_delegate(struct process *parent, struct process *child, const char *path)
{
    if (child->personality != PROC_NATIVE)
        return;
    uint32_t role = 0;
    if (security_role(child)==CIUKI_SECURITY_ROLE_DESKTOP) role=W32_SERVICE_DESKTOP;
    else if (security_role(child)==CIUKI_SECURITY_ROLE_W32SRV) role=W32_SERVICE_W32SRV;
    if (!role && !security_roles_active() && parent == proc_supervisor() && !strncmp(path, "/Programs/desktop", sizeof("/Programs/desktop")))
        role = W32_SERVICE_DESKTOP;
    else if (!role && !security_roles_active() && parent && parent->w32 && parent->w32->service == W32_SERVICE_DESKTOP
        && parent->pid == supervisor_desktop_pid()
        && !strncmp(path, "/System/Win32/w32srv", sizeof("/System/Win32/w32srv")))
        role = W32_SERVICE_W32SRV;
    else if (registry_desktop(parent) && !strncmp(path, "/Programs/control", sizeof("/Programs/control")))
        role = NATIVE_CONTROL_REGISTRY;
    if (role && !w32_process_init(child))
        child->w32->service = role;
    /* f1-98: the Win32 window server serves the desktop's windows; it keeps
     * the desktop's minimum CPU share beside a game. */
    if (role == W32_SERVICE_W32SRV && child->w32 && child->w32->service == role)
        proc_share_group(child);
}
static struct process *server_for(uint32_t service)
{
    if (service < 1 || service > 2)
        return 0;
    struct process *p = proc_find(service_pid[service - 1]);
    return p && p->state == PROC_LIVE && p->w32 && p->w32->service == service ? p : 0;
}
static uint32_t connect_service(struct process *client, uint32_t service, uint32_t *endpoint)
{
    const struct ciuki_credential *credential=security_credential(client);
    if (!credential || !security_process_live(client)) return 5;
    struct process *server=service==W32_SERVICE_W32SRV ?
        security_service_server(CIUKI_SECURITY_SERVICE_W32SRV,credential->session_id) :
        security_session_desktop(credential->session_id);
    /* Preserve interim boots until the new paired launch is installed. Once
     * LOGON exists, the global legacy registration is never authority. */
    if (!server && !security_roles_active()) server=server_for(service);
    if (!server) return 1062;
    if (!security_process_live(server) || !security_same(client,server)) return 5;
    for (struct service_link *l = links; l; l = l->next)
        if (l->client == client && l->server == server && l->service == service &&
            client->fds && client->fds[l->client_fd].object==l->client_end &&
            server->fds && server->fds[l->server_fd].object==l->server_end) {
            *endpoint = (uint32_t)l->client_fd;
            return 0;
        }
    struct service_link *l = w32_alloc(sizeof(*l));
    if (!l)
        return 8;
    l->charge = w32_object_new(W32_OBJ_ENDPOINT);
    if (!l->charge) {
        w32_free(l, sizeof(*l));
        return 8;
    }
    int32_t fds[2];
    int err = channel_connect(client, server, fds);
    if (err) {
        w32_release(l->charge);
        w32_free(l, sizeof(*l));
        return w32_last_error(w32_status(err));
    }
    l->client_end = client->fds[fds[0]].object;
    l->server_end = server->fds[fds[1]].object;
    l->client_end->inheritable = false;
    l->server_end->inheritable = false;
    l->client_end->retain(l->client_end);
    l->server_end->retain(l->server_end);
    l->client = client;
    l->server = server;
    l->service = service;
    l->client_fd = fds[0];
    l->server_fd = fds[1];
    l->next = links;
    links = l;
    w32_accounts.endpoints++;
    client->w32->endpoint[service - 1] = (uint32_t)fds[0] + 1;
    *endpoint = (uint32_t)fds[0];
    w32_changed_wake();
    /* sec-k07: a session W32SRV learns of the pending link on its
     * child_service_fd instead of only by polling ACCEPT. */
    if (service == W32_SERVICE_W32SRV && security_role(server) == CIUKI_SECURITY_ROLE_W32SRV &&
        secure_service_notify) secure_service_notify(credential->session_id);
    return 0;
}
/* A registered server whose authority is gone: a window server whose
 * desktop died (orphans are adopted by PID 1, so its parent is no longer the
 * desktop the supervisor runs) or a desktop PID 1 no longer recognises. Such
 * a process keeps serving its old clients but must not block a successor
 * (q3crash 2026-10-25: 323 w32srv restarts, each refused with error 5). */
static bool server_stale(const struct process *server, uint32_t service)
{
    uint32_t desktop = supervisor_desktop_pid();
    return service == W32_SERVICE_DESKTOP ? server->pid != desktop : server->ppid != desktop;
}
/* Retirement (lookup, stale check, proc_stop, publication) is one
 * non-yielding interval; the console record is written afterwards from
 * saved scalars, because klog may sleep on serial backpressure and the
 * retired process can be reaped, or a rival can register, meanwhile
 * (Codex review of wt/q3crash, P1). */
static void server_retired_log(uint32_t pid, uint32_t service, const char *why)
{
    klog("[win32] service server pid=%u service=%u %s; stopped", pid, service, why);
}
/* The supervisor's desktop died: its window server has no desktop to serve
 * and would otherwise survive as an orphan holding the registration. */
void w32_services_desktop_died(uint32_t desktop_pid)
{
    struct process *server = proc_find(service_pid[W32_SERVICE_W32SRV - 1]);
    if (!(server && server->state == PROC_LIVE && server->w32 && server->w32->service == W32_SERVICE_W32SRV &&
          (server->ppid == desktop_pid || server->ppid == proc_supervisor()->pid)))
        return;
    uint32_t pid = server->pid;
    proc_stop(server, 0, 0);
    server_retired_log(pid, W32_SERVICE_W32SRV, "desktop died");
}
void w32_services_cleanup(struct process *p)
{
    if (display_present_process_exit) display_present_process_exit(p);
    for (unsigned i = 0; i < 2; i++)
        if (service_pid[i] == p->pid)
            service_pid[i] = 0;
    struct service_link **at = &links;
    while (*at) {
        struct service_link *l = *at;
        if (l->client == p || l->server == p) {
            *at = l->next;
            if (l->accepted && l->server!=p && l->server->w32)
                l->server->w32->service_pending=true;
            if (l->client->fds && l->client->fds[l->client_fd].object == l->client_end)
                desktop_close(l->client, l->client_fd);
            /* A dying client's end is released with its table. An accepted
             * link's server end is never closed under a polling server: it
             * reads the hang-up (recv 0) and closes the descriptor itself.
             * Closing it here left the desktop polling an invalid descriptor
             * (POLLNVAL, exit 1) or a reused number (q3crash 2026-10-25). An
             * unaccepted link's server end was installed by channel_connect
             * but never reported by ACCEPT: the server cannot know its
             * number, so it is closed here (Codex review P2: repeated launch
             * failures leaked one server descriptor each). The server's own
             * death releases its table. */
            if (!l->accepted && l->server->fds && l->server->fds[l->server_fd].object == l->server_end)
                desktop_close(l->server, l->server_fd);
            l->client_end->release(l->client_end);
            l->server_end->release(l->server_end);
            if (l->client->w32)
                l->client->w32->endpoint[l->service - 1] = 0;
            w32_accounts.endpoints--;
            w32_release(l->charge);
            w32_free(l, sizeof(*l));
        } else
            at = &l->next;
    }
    w32_changed_wake();
}
/* A service wait observes only retained link identities, never an arbitrary fd
 * supplied by the caller. Enrollment and predicate recheck are atomic in kwait. */
bool w32_service_ready(struct proc_thread *t, bool desktop)
{
    if (t->w32->session_pending) return true;
    if (!desktop) return false;
    struct process *p=t->process;
    for (struct service_link *l=links;l;l=l->next) {
        if (l->client!=p || l->service!=W32_SERVICE_DESKTOP || !p->fds ||
            p->w32->endpoint[0]!=(uint32_t)l->client_fd+1 ||
            p->fds[l->client_fd].object!=l->client_end) continue;
        struct desktop_description *d=desktop_fd(p,l->client_fd);
        if (d && d->kind==DESKTOP_CHANNEL &&
            (channel_poll_events(d)&(POLLIN|POLLHUP))) return true;
    }
    return false;
}
struct service_wait { struct proc_thread *thread; uint32_t service; };
static bool service_work_ready(void *opaque)
{
    struct service_wait *w=opaque;
    struct process *p=w->thread->process;
    if (p->state!=PROC_LIVE || !security_process_live(p) ||
        w->thread->stopped || proc_signal_caught(w->thread)) return true;
    if (p->w32->service_pending) return true;
    if (w->service==W32_SERVICE_W32SRV && channel_service_pending(p,CIUKI_SECURITY_SERVICE_W32SRV)) return true;
    for (struct service_link *l=links;l;l=l->next) {
        if (l->server!=p || l->service!=w->service || !p->fds ||
            p->fds[l->server_fd].object!=l->server_end) continue;
        if (!l->accepted) {
            if (security_process_live(l->client) && security_same(p,l->client) &&
                l->client->fds && l->client->fds[l->client_fd].object==l->client_end) return true;
            continue;
        }
        struct desktop_description *d=desktop_fd(p,l->server_fd);
        if (d && d->kind==DESKTOP_CHANNEL && (channel_poll_events(d)&(POLLIN|POLLHUP))) return true;
    }
    return false;
}
uint32_t w32_service_call(struct proc_thread *t, struct ciuki_w32_service_args *a)
{
    struct process *p = t->process;
    if (!p->w32) {
        if (a->action==W32_SVC_CONNECT)
            klog("[win32] service pid=%u action=connect service=%u error=5",p->pid,a->service);
        a->error = 5;
        return 0;
    }
    if (a->service < 1 || a->service > 2) {
        a->error = 87;
        return 0;
    }
    if (a->action == W32_SVC_SESSION) {
        if (p->personality!=PROC_WIN32 || a->service!=W32_SERVICE_W32SRV || a->pid || a->endpoint) {
            a->error=87; return 0;
        }
        uint32_t f=irq_save();
        a->result=t->w32->session_pending;
        t->w32->session_pending=false;
        irq_restore(f);
    } else if (a->action == W32_SVC_WAIT || a->action == W32_SVC_NOTIFY) {
        const struct ciuki_credential *credential=security_credential(p);
        bool registered=security_roles_active() ? credential &&
            (a->service==W32_SERVICE_W32SRV ? security_service_server(CIUKI_SECURITY_SERVICE_W32SRV,credential->session_id)==p :
                security_session_desktop(credential->session_id)==p) : server_for(a->service)==p;
        if (p->personality!=PROC_NATIVE || !registered || p->w32->service!=a->service) {
            a->error=5; return 0;
        }
        if (a->action==W32_SVC_WAIT) {
            if (a->endpoint) { a->error=87; return 0; }
            struct service_wait w={t,a->service};
            uint64_t deadline=a->pid==W32_INFINITE ? UINT64_MAX : w32_now()+a->pid;
            a->result=kwait_wait_until(&w32_changed,service_work_ready,&w,deadline);
            p->w32->service_pending=false;
            if (p->state!=PROC_LIVE || !security_process_live(p) || t->stopped || proc_signal_caught(t)) a->error=995;
        } else {
            struct process *client=proc_find(a->pid);
            struct proc_thread *target=client ? proc_thread_find(client,a->endpoint) : 0;
            bool connected=false;
            for (struct service_link *l=links;l;l=l->next)
                if (l->server==p && l->client==client && l->service==W32_SERVICE_W32SRV && l->accepted &&
                    p->fds && client->fds && p->fds[l->server_fd].object==l->server_end &&
                    client->fds[l->client_fd].object==l->client_end) connected=true;
            if (a->service!=W32_SERVICE_W32SRV || !connected || !target || !target->w32 ||
                target->stopped || !security_same(p,client) || !security_process_live(client)) { a->error=5; return 0; }
            target->w32->session_pending=true;
            a->result=1;
            w32_changed_wake();
        }
    } else if (a->action == W32_SVC_REGISTER) {
        if (p->personality != PROC_NATIVE || p->w32->service != a->service) {
            a->error = 5;
            return 0;
        }
        if (security_roles_active()) {
            if (security_role(p)!=(a->service==W32_SERVICE_W32SRV ? CIUKI_SECURITY_ROLE_W32SRV : CIUKI_SECURITY_ROLE_DESKTOP)) {
                a->error=5; return 0;
            }
            a->result=1; return 0; /* registration was completed before publication */
        }
        struct process *old = server_for(a->service);
        uint32_t retired = 0;
        if (old && old != p) {
            uint32_t holder = old->pid;
            if (!server_stale(old, a->service)) {
                a->error = 5;
                klog("[win32] service pid=%u action=register service=%u error=5 holder=%u", p->pid, a->service, holder);
                return 0;
            }
            retired = holder;
            proc_stop(old, 0, 0);
        }
        service_pid[a->service - 1] = p->pid;
        a->result = 1;
        if (retired) server_retired_log(retired, a->service, "stale registration");
    } else if (a->action == W32_SVC_CONNECT) {
        /* Endpoint slot 1 is the registry (wire service id 2). Native callers
         * gain no desktop endpoint or general personality authority. */
        if (p->personality != PROC_WIN32 &&
            !(a->service==W32_SERVICE_W32SRV &&
              (registry_desktop(p) ||
               (p->personality==PROC_NATIVE && p->state==PROC_LIVE &&
                p->w32->service==NATIVE_CONTROL_REGISTRY &&
                registry_desktop(proc_find(p->ppid)))))) {
            klog("[win32] service pid=%u action=connect service=%u error=5",p->pid,a->service);
            a->error = 5;
            return 0;
        }
        a->error = connect_service(p, a->service, &a->endpoint);
        a->result = !a->error;
    } else if (a->action == W32_SVC_ACCEPT) {
        const struct ciuki_credential *credential=security_credential(p);
        bool session_server=credential && (a->service==W32_SERVICE_W32SRV ?
            security_service_server(CIUKI_SECURITY_SERVICE_W32SRV,credential->session_id)==p :
            security_session_desktop(credential->session_id)==p);
        if (!session_server && (security_roles_active() || server_for(a->service) != p)) {
            a->error = 5;
            return 0;
        }
        for (struct service_link *l = links; l; l = l->next)
            if (l->server == p && l->service == a->service && !l->accepted) {
                if (!security_same(p,l->client) || !security_process_live(l->client) ||
                    !p->fds || p->fds[l->server_fd].object!=l->server_end ||
                    !l->client->fds || l->client->fds[l->client_fd].object!=l->client_end) continue;
                l->accepted = true;
                a->pid = l->client->pid;
                a->endpoint = (uint32_t)l->server_fd;
                a->result = 1;
                break;
            }
    } else if (a->action == W32_SVC_PRIORITY) {
        /* class in pid, signed relative in endpoint; only own normal/idle
         * mappings are unprivileged, HIGH is desktop-authorized. */
        if (p->personality != PROC_WIN32) {
            a->error = 5;
            return 0;
        }
        if (a->pid == 0x80 || a->pid == 0x100 || a->endpoint == 15) {
            a->error = 5;
            return 0;
        }
        int level = proc_priority_win32(a->pid, (int32_t)a->endpoint, false);
        if (level < 0)
            a->error = 87;
        else {
            task_set_base(t->task, P_NORMAL, (unsigned)level);
            a->result = 1;
        }
    } else if (a->action == W32_SVC_FOCUS || a->action == W32_SVC_INPUT) {
        bool registered=security_roles_active() ? security_session_desktop(security_credential(p)->session_id)==p :
            server_for(W32_SERVICE_DESKTOP)==p;
        if (p->w32->service != W32_SERVICE_DESKTOP || !registered
            || p->pid != supervisor_desktop_pid()) {
            a->error = 5;
            return 0;
        }
        struct process *target = proc_find(a->pid);
        if (!target || target->personality != PROC_WIN32 || !security_same(p,target)) {
            a->error = 6;
            return 0;
        }
        struct proc_thread *ui = proc_thread_find(target, a->endpoint);
        if (!ui || !ui->task) {
            a->error = 6;
            return 0;
        }
        uint32_t f = irq_save();
        if (a->action == W32_SVC_FOCUS)
            task_focus(ui->task);
        else
            task_input_delivered(ui->task);
        irq_restore(f);
        a->result = 1;
    } else
        a->error = 87;
    return 0;
}

int w32_services_attach(struct process *p, uint32_t endpoints[2])
{
    for (unsigned i = 0; i < 2; i++) {
        endpoints[i] = UINT32_MAX;
        uint32_t error = connect_service(p, i + 1, &endpoints[i]);
        if (error && error != W32_ERROR_SERVICE_NOT_ACTIVE)
            return error == 8 ? -ENOMEM : -EACCES;
    }
    return 0;
}
