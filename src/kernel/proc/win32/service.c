/* Path-delegated native servers and private channel endpoints per client.
 * f0-45 remains the only source of scheduling policy and input boosts.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/display.h>
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
bool w32_service_desktop_client(struct process *desktop, uint32_t pid)
{
    if (!desktop || desktop->pid!=supervisor_desktop_pid() ||
        desktop->state!=PROC_LIVE || !desktop->w32 ||
        desktop->w32->service!=W32_SERVICE_DESKTOP || service_pid[0]!=desktop->pid) return false;
    for (struct service_link *l=links;l;l=l->next)
        if (l->server==desktop && l->client->pid==pid && l->accepted &&
            l->client->state==PROC_LIVE && l->service==W32_SERVICE_DESKTOP &&
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
    if (parent == proc_supervisor() && !strncmp(path, "/Programs/desktop", sizeof("/Programs/desktop")))
        role = W32_SERVICE_DESKTOP;
    else if (parent && parent->w32 && parent->w32->service == W32_SERVICE_DESKTOP
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
    struct process *server = server_for(service);
    if (!server)
        return 1062;
    for (struct service_link *l = links; l; l = l->next)
        if (l->client == client && l->server == server && l->service == service) {
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
    return 0;
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
            if (l->client->fds && l->client->fds[l->client_fd].object == l->client_end)
                desktop_close(l->client, l->client_fd);
            if (l->server->fds && l->server->fds[l->server_fd].object == l->server_end)
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
    if (a->action == W32_SVC_REGISTER) {
        if (p->personality != PROC_NATIVE || p->w32->service != a->service) {
            a->error = 5;
            return 0;
        }
        struct process *old = server_for(a->service);
        if (old && old != p) {
            a->error = 5;
            return 0;
        }
        service_pid[a->service - 1] = p->pid;
        a->result = 1;
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
        if (server_for(a->service) != p) {
            a->error = 5;
            return 0;
        }
        for (struct service_link *l = links; l; l = l->next)
            if (l->server == p && l->service == a->service && !l->accepted) {
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
        if (p->w32->service != W32_SERVICE_DESKTOP || server_for(W32_SERVICE_DESKTOP) != p
            || p->pid != supervisor_desktop_pid()) {
            a->error = 5;
            return 0;
        }
        struct process *target = proc_find(a->pid);
        if (!target || target->personality != PROC_WIN32) {
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
