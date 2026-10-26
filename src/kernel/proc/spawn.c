/* Bounded one-time argument snapshots and atomic spawn preparation.
 * SysV stack: https://www.sco.com/developers/devspecs/abi386-4.pdf
 * Ciuki fixes the auxv tags, registers and ARG_MAX accounting in abi.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/security.h>
#include <ciuki/desktop.h>
#include <ciuki/tzrule.h>
#pragma weak desktop_close
#pragma weak channel_pair
#pragma weak channel_endpoint_handoff
#pragma weak channel_control_bind
#pragma weak desktop_fd
#pragma weak tzr_parse
extern int security_profile_timezone(const struct security_subject *,const struct ciuki_security_profile *,char *,uint32_t) __attribute__((weak));
#include <ciuki/supervisor.h>
#include <ciuki/signal.h>

extern void w32_service_delegate(struct process *,struct process *,const char *) __attribute__((weak));

static const struct ciuki_file_ops *file_ops;
void proc_set_file_ops(const struct ciuki_file_ops *ops) { file_ops = ops; }
const struct ciuki_file_ops *proc_get_file_ops(void) { return file_ops; }

struct proc_strings *proc_strings_new(void)
{
    _Static_assert(sizeof(struct proc_strings) <= PAGE_SIZE, "argument metadata page");
    uint32_t phys = pmm_alloc();
    if (!phys)
        return 0;
    memset(P2V(phys), 0, PAGE_SIZE);
    return P2V(phys);
}

void proc_strings_free(struct proc_strings *s)
{
    if (!s)
        return;
    for (unsigned i = 0; i < ARRAY_SIZE(s->pages); i++)
        if (s->pages[i])
            pmm_free(s->pages[i]);
    pmm_free(V2P(s));
}

static uint32_t stack_words(const struct proc_strings *s)
{
    return 1 + s->argc + 1 + s->envc + 1 + 12;
}

static int append_byte(struct proc_strings *s, uint8_t value)
{
    if (s->used == CIUKI_ARG_MAX)
        return -E2BIG;
    unsigned page = s->used / PAGE_SIZE, off = s->used % PAGE_SIZE;
    if (!s->pages[page]) {
        s->pages[page] = pmm_alloc();
        if (!s->pages[page])
            return -ENOMEM;
        memset(P2V(s->pages[page]), 0, PAGE_SIZE);
    }
    ((uint8_t *)P2V(s->pages[page]))[off] = value;
    s->used++;
    return 0;
}

static int start_string(struct proc_strings *s, bool env)
{
    if (env) {
        if (s->envc == CIUKI_ENVP_MAX)
            return -E2BIG;
        s->envp[s->envc++] = s->used;
    } else {
        if (s->argc == CIUKI_ARGV_MAX)
            return -E2BIG;
        s->argv[s->argc++] = s->used;
    }
    return 0;
}

static bool fits(const struct proc_strings *s)
{
    return ((s->used + 3) & ~3u) + stack_words(s) * sizeof(uint32_t) <= CIUKI_ARG_MAX;
}

int proc_strings_add(struct proc_strings *s, const char *text, uint32_t bytes, bool env)
{
    if (!bytes || text[bytes - 1])
        return -EINVAL;
    int err = start_string(s, env);
    if (err)
        return err;
    for (uint32_t i = 0; i < bytes; i++) {
        err = append_byte(s, (uint8_t)text[i]);
        if (err)
            return err;
    }
    return fits(s) ? 0 : -E2BIG;
}

static int copy_vector(struct proc_strings *s, uint32_t vector, bool env)
{
    if (!vector)
        return env ? 0 : -EINVAL;
    unsigned limit = env ? CIUKI_ENVP_MAX : CIUKI_ARGV_MAX;
    for (unsigned i = 0; i <= limit; i++) {
        uint32_t address;
        if (vector > UINT32_MAX - i * sizeof(uint32_t))
            return -EFAULT;
        int err = copy_from_user(&address, vector + i * sizeof(uint32_t), sizeof(address));
        if (err)
            return err;
        if (!address)
            return !env && !i ? -EINVAL : 0;
        if ((err = start_string(s, env)))
            return err;
        for (;;) {
            uint8_t c;
            if ((err = copy_from_user(&c, address, sizeof(c))))
                return err;
            if ((err = append_byte(s, c)))
                return err;
            if (!fits(s))
                return -E2BIG;
            if (!c)
                break;
            if (address == UINT32_MAX)
                return -EFAULT;
            address++;
        }
    }
    return -E2BIG;
}

int proc_strings_copy(struct proc_strings *s, uint32_t argv, uint32_t envp)
{
    int err = copy_vector(s, argv, false);
    return err ? err : copy_vector(s, envp, true);
}

static void stack_word(struct uaddr *u, uint32_t *at, uint32_t word)
{
    if (ua_write(u, *at, &word, sizeof(word)))
        panic("spawn: reserved stack disappeared");
    *at += sizeof(word);
}

int proc_stack_build(struct uaddr *u, const struct proc_strings *s,
                     uint32_t entry, uint32_t tls, uint32_t *esp)
{
    if (!s->argc)
        return -EINVAL;
    if (!fits(s))
        return -E2BIG;
    uint32_t text_base = CIUKI_MAIN_STACK_LIMIT - s->used;
    *esp = (text_base & ~3u) - stack_words(s) * sizeof(uint32_t);
    if (!ua_range(u, *esp, CIUKI_MAIN_STACK_LIMIT - *esp, PROT_READ | PROT_WRITE))
        return -EFAULT;
    for (uint32_t off = 0; off < s->used;) {
        uint32_t n = s->used - off;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        int err = ua_write(u, text_base + off, P2V(s->pages[off / PAGE_SIZE]), n);
        if (err)
            return err;
        off += n;
    }
    uint32_t at = *esp;
    stack_word(u, &at, s->argc);
    for (unsigned i = 0; i < s->argc; i++)
        stack_word(u, &at, text_base + s->argv[i]);
    stack_word(u, &at, 0);
    for (unsigned i = 0; i < s->envc; i++)
        stack_word(u, &at, text_base + s->envp[i]);
    stack_word(u, &at, 0);
    const uint32_t aux[] = { AT_PAGESZ, CIUKI_PAGE_SIZE, AT_ENTRY, entry,
        AT_CIUKI_TLS, tls, AT_CIUKI_TLS_SIZE, CIUKI_TLS_SIZE,
        AT_CIUKI_ABI, CIUKI_ABI_VERSION, AT_NULL, 0 };
    for (unsigned i = 0; i < ARRAY_SIZE(aux); i++)
        stack_word(u, &at, aux[i]);
    return 0;
}

extern bool w32_is_pe(const struct ciuki_file *) __attribute__((weak));
extern int w32_spawn(struct process *, const struct ciuki_file *, const struct proc_strings *, uint64_t) __attribute__((weak));
extern bool w32_refuse_image_name(struct process *,const char *) __attribute__((weak));

static int prepare_image(struct process *p, const struct ciuki_file *file,
                          const struct proc_strings *strings, uint64_t mask)
{
    if(w32_refuse_image_name&&w32_refuse_image_name(p,p->name))return -ENOSYS;
    if (w32_is_pe && w32_is_pe(file)) {
        int err = w32_spawn ? w32_spawn(p, file, strings, mask) : -ENOSYS;
        if (!err) proc_launch_elf(p); /* image and ntdll mapped: end of the spawn stage */
        return err;
    }
    struct elf_image image;
    int err = elf_validate(file, &image);
    if (err)
        return err;
    err = elf_load(file, &image, p->memory);
    if (!err) proc_launch_elf(p);
    struct proc_thread *t = 0;
    if (!err)
        err = proc_thread_prepare(p, image.entry, 0, 0, CIUKI_THREAD_STACK_DEFAULT,
                                  0, mask, true, &t);
    uint32_t esp;
    if (!err)
        err = proc_stack_build(p->memory, strings, image.entry, t->tls, &esp);
    if (!err)
        task_native_frame(t->task, esp);
    return err;
}

/* Callers without a path name the child after argv[0]'s basename. */
static void name_from_argv0(struct process *p, const struct proc_strings *s)
{
    char name[CIUKI_PROC_NAME_MAX];
    unsigned n = 0;
    if (!s->argc)
        return;
    for (uint32_t at = s->argv[0]; at < s->used; at++) {
        char c = ((const char *)P2V(s->pages[at / PAGE_SIZE]))[at % PAGE_SIZE];
        if (!c)
            break;
        if (c == '/' || c == '\\')
            n = 0;
        else if (n < sizeof(name) - 1)
            name[n++] = c;
    }
    name[n] = 0;
    proc_set_name(p, name);
}

int proc_spawn_file(struct process *parent, const struct ciuki_file *file,
                    const struct proc_strings *strings, const struct ciuki_spawn_fd *fds,
                    uint32_t fd_count, uint32_t flags, uint64_t mask, struct process **out)
{
    if (flags & ~CIUKI_SPAWN_NEW_GROUP)
        return -EINVAL;
    struct process *p;
    int err = proc_prepare(parent, &p);
    if (err)
        return err;
    err = proc_inherit(p, parent, fds, fd_count);
    if (!err)
        name_from_argv0(p, strings);
    if (!err) {
        uint64_t now = proc_launch_enabled() ? proc_launch_now() : 0;
        proc_launch_start(parent, p, now, now);
    }
    if (!err)
        err = prepare_image(p, file, strings, mask);
    struct proc_thread *self = proc_thread_for(g_current);
    if (self && self->process == parent) self->launch_loading = 0;
    if (!err && (!security_process_live(p) || parent->state != PROC_LIVE))
        err = -EINTR;
    if (err) {
        proc_discard(p);
        return err;
    }
    proc_publish(p, flags);
    *out = p;
    return (int)p->pid;
}

/* At most 517 input spans, stored in two independently allocated pages.
 * Strings use proc_strings pages; no allocation exceeds the kernel heap class. */
struct credential_span_entry { uint32_t at, bytes; };
struct credential_copy {
    uint32_t span_pages[2];
    unsigned count;
    char text[CIUKI_PATH_MAX];
};
_Static_assert(sizeof(struct credential_copy) <= 2040, "credential snapshot fits kernel heap");
static struct credential_span_entry *credential_entry(struct credential_copy *c, unsigned index)
{
    return &((struct credential_span_entry *)P2V(c->span_pages[index / 512]))[index % 512];
}
struct spawn_operation {
    struct proc_thread *thread;
    struct process *child;
    struct proc_strings *strings;
    struct ciuki_spawn_fd *fds;
    char *path;
    struct ciuki_file file;
    struct credential_copy *security_copy;
    bool control_owned;
    int32_t control_fd;
    struct process *controller;
    struct proc_object *control_object;
    uint32_t preparing_session;
};

static void spawn_cleanup(void *operation)
{
    struct spawn_operation *op = operation;
    op->thread->launch_loading = 0;
    if (op->control_owned && desktop_close && op->controller->fds &&
        op->controller->fds[op->control_fd].object==op->control_object)
        desktop_close(op->controller,op->control_fd);
    if (op->control_object) op->control_object->release(op->control_object);
    if (op->file.close)
        op->file.close(op->file.cookie);
    if (op->child)
        proc_discard(op->child);
    if (op->preparing_session) security_launch_preparation_done(op->preparing_session);
    proc_strings_free(op->strings);
    kfree(op->fds);
    kfree(op->path);
    if (op->security_copy)
        for (unsigned i = 0; i < 2; i++)
            if (op->security_copy->span_pages[i]) pmm_free(op->security_copy->span_pages[i]);
    kfree(op->security_copy);
    kfree(op);
}

static int credential_span(struct credential_copy *c, uint32_t at, uint32_t bytes)
{
    if (!at || at > UINT32_MAX - bytes) return -EFAULT;
    for (unsigned i = 0; i < c->count; i++) {
        const struct credential_span_entry *entry = credential_entry(c, i);
        if (at < entry->at + entry->bytes && entry->at < at + bytes) return -EINVAL;
    }
    if (c->count == CIUKI_ARGV_MAX + CIUKI_ENVP_MAX + 5) return -E2BIG;
    unsigned page = c->count / 512;
    if (!c->span_pages[page] && !(c->span_pages[page] = pmm_alloc())) return -ENOMEM;
    *credential_entry(c, c->count++) = (struct credential_span_entry){ at, bytes };
    return 0;
}
static int credential_string(struct credential_copy *c, uint32_t address, unsigned limit, unsigned *bytes)
{
    for (unsigned n = 0; n < limit; n++) {
        if (address > UINT32_MAX - n || copy_from_user(c->text + n, address + n, 1)) return -EFAULT;
        if (!c->text[n]) {
            *bytes = n + 1;
            return credential_span(c, address, n + 1);
        }
    }
    return -E2BIG;
}
static int credential_vector(struct credential_copy *c, struct proc_strings *s, uint32_t address, bool env)
{
    if (!address) return env ? 0 : -EINVAL;
    if (address & 3) return -EINVAL;
    unsigned limit = env ? CIUKI_ENVP_MAX : CIUKI_ARGV_MAX;
    /* Snapshot the vector before reading strings. A later vector mutation
     * cannot change either overlap checks or the selected strings. */
    uint32_t entries[CIUKI_ARGV_MAX + 1];
    unsigned n = 0;
    for (; n <= limit; n++) {
        if (address > UINT32_MAX - 4 * (n + 1) ||
            copy_from_user(&entries[n], address + 4 * n, 4)) return -EFAULT;
        if (!entries[n]) break;
    }
    if (n > limit) return -E2BIG;
    if (!n && !env) return -EINVAL;
    int err = credential_span(c, address, 4 * (n + 1));
    if (err) return err;
    for (unsigned i = 0; i < n; i++) {
        if ((err = start_string(s, env))) return err;
        unsigned bytes = 0;
        for (;;) {
            uint8_t value;
            if (entries[i] > UINT32_MAX - bytes || copy_from_user(&value, entries[i] + bytes, 1)) return -EFAULT;
            bytes++;
            if ((err = append_byte(s, value))) return err;
            if (!fits(s)) return -E2BIG;
            if (!value) break;
        }
        if ((err = credential_span(c, entries[i], bytes))) return err;
    }
    return 0;
}
static void credential_text(char *out, const char *key, const char *root, const char *suffix)
{
    unsigned at = 0;
    if (key) { at = (unsigned)strlen(key); memcpy(out, key, at); out[at++] = '='; }
    unsigned n = (unsigned)strlen(root); memcpy(out + at, root, n); at += n;
    memcpy(out + at, suffix, strlen(suffix) + 1);
}
static int credential_environment(struct proc_strings **strings, struct credential_copy *c,
                                   const struct ciuki_security_profile *profile, struct process *child)
{
    static const char *const keys[] = {"USER", "LOGNAME", "HOME", "USERPROFILE", "CIUKI_PROFILE", "TEMP", "TMP", "TMPDIR", "PATH", "LC_ALL", "TZ", "CIUKI_PROFILE_SETTINGS"};
    char timezone[CIUKI_TIME_RULE_MAX]={0};
    int tzerr=security_profile_timezone ? security_profile_timezone(child->subject,profile,timezone,sizeof(timezone)) : -ENOENT;
    if (tzerr && tzerr!=-ENOENT) return tzerr;
    if (tzerr==-ENOENT) memcpy(timezone,"UTC0",5);
    unsigned tn=0; while (tn<sizeof(timezone) && timezone[tn]) tn++;
    if (tn==sizeof(timezone)) return -EINVAL;
    struct tzr rule;
    if (tzr_parse ? !tzr_parse(timezone,&rule) : strncmp(timezone,"UTC0",5)!=0) return -EINVAL;
    bool seen[ARRAY_SIZE(keys)] = {0};
    struct proc_strings *old = *strings, *clean = proc_strings_new();
    if (!clean) return -ENOMEM;
    int err = 0;
    for (unsigned i = 0; i < old->argc + old->envc && !err; i++) {
        bool env = i >= old->argc;
        unsigned at = env ? old->envp[i - old->argc] : old->argv[i];
        if (!env) {
            err = start_string(clean, false);
            uint8_t value;
            while (!err) {
                value = ((const uint8_t *)P2V(old->pages[at / PAGE_SIZE]))[at % PAGE_SIZE];
                at++;
                err = append_byte(clean, value);
                if (!value) break;
            }
            continue;
        }
        unsigned n = 0;
        do {
            c->text[n] = ((const char *)P2V(old->pages[at / PAGE_SIZE]))[at % PAGE_SIZE];
            at++;
        } while (c->text[n++] && n < sizeof(c->text));
        if (c->text[n - 1]) { err = -EINVAL; break; }
        if (env) {
            unsigned key = 0;
            for (; key < ARRAY_SIZE(keys); key++) {
                unsigned k = (unsigned)strlen(keys[key]);
                if (!strncmp(c->text, keys[key], k) && c->text[k] == '=') break;
            }
            if (key == ARRAY_SIZE(keys) || seen[key]) { err = -EINVAL; break; }
            seen[key] = true;
            const char *value = c->text + strlen(keys[key]) + 1;
            if (key < 2) {
                unsigned length = (unsigned)strlen(value);
                if (!length || length > 31) { err = -EINVAL; break; }
                for (unsigned j = 0; j < length; j++)
                    if (!((value[j] >= 'a' && value[j] <= 'z') || (value[j] >= 'A' && value[j] <= 'Z') ||
                          (value[j] >= '0' && value[j] <= '9') || value[j] == '_' || value[j] == '-' || value[j] == '.')) err = -EINVAL;
            } else {
                char expected[128];
                const char *suffix = key == 2 ? "/Documents" : key >= 5 && key <= 7 ? "/Local Settings/Temp" : "";
                if (key == 8) memcpy(expected, "/Programs:/System", sizeof("/Programs:/System"));
                else if (key==9) memcpy(expected,"C",2);
                else if (key==10) memcpy(expected,timezone,tn+1);
                else if (key==11) credential_text(expected,0,profile->path,"/Settings/desktop.cfg");
                else credential_text(expected, 0, profile->path, suffix);
                if (strncmp(value, expected, strlen(expected) + 1)) err = -EINVAL;
            }
        }
        if (!err) err = proc_strings_add(clean, c->text, n, env);
    }
    for (unsigned i = 2; i < ARRAY_SIZE(keys) && !err; i++) if (!seen[i]) {
        const char *suffix = i == 2 ? "/Documents" : i >= 5 && i <= 7 ? "/Local Settings/Temp" : "";
        if (i == 8) credential_text(c->text, keys[i], "/Programs:/System", "");
        else if (i==9) credential_text(c->text,keys[i],"C","");
        else if (i==10) credential_text(c->text,keys[i],timezone,"");
        else if (i==11) credential_text(c->text,keys[i],profile->path,"/Settings/desktop.cfg");
        else credential_text(c->text, keys[i], profile->path, suffix);
        err = proc_strings_add(clean, c->text, (uint32_t)strlen(c->text) + 1, true);
    }
    if (err) proc_strings_free(clean);
    else { *strings = clean; proc_strings_free(old); }
    return err;
}
extern int px_security_cwd(struct process *, const char *) __attribute__((weak));
/* k03 supplies resolved executable authorization under the prepared child.
 * Its absence must not open an executable as the SYSTEM caller. */
extern int security_exec_open(struct process *, const char *, struct ciuki_file *) __attribute__((weak));
int proc_spawn_credential(uint32_t spawn, uint32_t session, uint32_t outer, uint32_t *pid)
{
    struct process *parent = proc_current();
    if (!security_logon(parent)) return -EPERM;
    struct ciuki_spawn_args a;
    if (!spawn || spawn > UINT32_MAX - sizeof(a)) return -EFAULT;
    if (spawn & 3) return -EINVAL;
    if (spawn < outer + 64 && outer < spawn + sizeof(a)) return -EINVAL;
    int err = copy_from_user(&a, spawn, sizeof(a));
    if (err) return err;
    if (a.size != 32 || a.reserved || a.fd_count || a.fd_list || (a.flags & ~CIUKI_SPAWN_NEW_GROUP)) return -EINVAL;
    struct ciuki_security_profile profile;
    if ((err = security_session_profile(session, &profile))) return err;
    struct proc_thread *self = proc_thread_for(g_current);
    struct spawn_operation *op = kzalloc(sizeof(*op));
    if (!op) return -ENOMEM;
    op->thread = self;
    self->operation = op;
    self->cleanup = spawn_cleanup;
    struct credential_copy *c = op->security_copy = kzalloc(sizeof(*c));
    op->strings = proc_strings_new();
    op->path = kmalloc(CIUKI_PATH_MAX);
    if (!c || !op->strings || !op->path) { err = -ENOMEM; goto out; }
    if ((err = credential_span(c, outer, 64)) || (err = credential_span(c, spawn, 32))) goto out;
    unsigned bytes;
    err = credential_string(c, a.path, CIUKI_PATH_MAX, &bytes);
    if (err) { if (err == -E2BIG) err = -ENAMETOOLONG; goto out; }
    memcpy(op->path, c->text, bytes);
    if ((err = credential_vector(c, op->strings, a.argv, false)) ||
        (err = credential_vector(c, op->strings, a.envp, true))) goto out;
    if ((err = proc_prepare(parent, &op->child)) ||
        (err = security_session_attach(op->child, session)) ||
        (err = credential_environment(&op->strings, c, &profile, op->child))) goto out;
    proc_set_name(op->child, op->path);
    credential_text(c->text, 0, profile.path, "/Documents");
    if (!px_security_cwd) { err = -EIO; goto out; }
    if ((err = px_security_cwd(op->child, c->text))) goto out;
    if (!security_exec_open) { err = -EIO; goto out; }
    self->launch_loading = op->child;
    if ((err = security_exec_open(op->child, op->path, &op->file))) goto out;
    if (w32_is_pe && w32_is_pe(&op->file)) { err = -ENOEXEC; goto out; }
    if ((err = prepare_image(op->child, &op->file, op->strings, 0))) goto out;
    if (!security_process_live(op->child) || parent->state != PROC_LIVE || proc_signal_caught(self)) {
        err = -EACCES; goto out;
    }
    if (op->file.close) op->file.close(op->file.cookie);
    op->file.close = 0;
    /* Adapter close may yield, stop a parent or end this session. */
    if (!security_process_live(op->child) || op->child->state != PROC_PREPARING ||
        !security_process_live(parent) || parent->state != PROC_LIVE || proc_signal_caught(self)) {
        err = -EACCES; goto out;
    }
    proc_publish(op->child, a.flags);
    *pid = op->child->pid;
    op->child = 0;
out:
    proc_thread_cleanup(self);
    return err;
}

int security_launch_environment(struct proc_strings *strings, const struct ciuki_security_launch *l)
{
    static const char *const keys[]={"CIUKI_SESSION_ID","CIUKI_SESSION_GENERATION",
        "CIUKI_LAUNCH_ID","CIUKI_LAUNCH_GENERATION","CIUKI_LAUNCH_PURPOSE",
        "CIUKI_LAUNCH_READY_FD","CIUKI_CONSOLE_FD","CIUKI_DISPLAY_FD","CIUKI_INPUT_FD","CIUKI_SERVICE_FD",
        "CIUKI_GRANT_GENERATION","CIUKI_SERVICE_GENERATION","CIUKI_PROCESS_GENERATION"};
    int32_t values[]={(int32_t)l->session_id,(int32_t)l->session_generation,(int32_t)l->launch_id,
        (int32_t)l->launch_generation,(int32_t)l->purpose,l->child_ready_fd,l->child_console_fd,
        l->child_display_fd,l->child_input_fd,l->child_service_fd,
        (int32_t)l->grant_generation,(int32_t)l->service_generation,(int32_t)l->process_generation};
    for (unsigned i=0;i<ARRAY_SIZE(keys);i++) {
        char text[64],digits[10]; unsigned n=(unsigned)strlen(keys[i]),count=0;
        memcpy(text,keys[i],n); text[n++]='=';
        uint32_t value=(uint32_t)values[i];
        if (i>=5 && i<10 && values[i]<0) { text[n++]='-'; value=0u-value; }
        do { digits[count++]=(char)('0'+value%10); value/=10; } while (value);
        while (count) text[n++]=digits[--count];
        text[n]=0;
        int err=proc_strings_add(strings,text,n+1,true);
        if (err) return err;
    }
    return 0;
}
int proc_security_launch_prepare(struct process *parent, struct ciuki_security_launch *l, struct process **out)
{
    if (!security_logon(parent)) return -EPERM;
    if (!channel_pair || !channel_endpoint_handoff || !channel_control_bind || !desktop_fd || !px_security_cwd || !security_exec_open) return -EIO;
    struct proc_thread *self=proc_thread_for(g_current);
    if (!self || self->process!=parent) return -EPERM;
    struct spawn_operation *op=kzalloc(sizeof(*op));
    if (!op) return -ENOMEM;
    op->thread=self; op->controller=parent; op->preparing_session=l->session_id;
    self->operation=op; self->cleanup=spawn_cleanup;
    const char *path=security_role_image(l->purpose==1 ? CIUKI_SECURITY_ROLE_DESKTOP : CIUKI_SECURITY_ROLE_W32SRV);
    struct ciuki_security_profile profile;
    int err=security_session_profile(l->session_id,&profile);
    if (err) goto done;
    op->strings=proc_strings_new(); op->security_copy=kzalloc(sizeof(*op->security_copy));
    if (!op->strings || !op->security_copy) { err=-ENOMEM; goto done; }
    if ((err=proc_strings_add(op->strings,path,(uint32_t)strlen(path)+1,false)) ||
        (err=proc_prepare(parent,&op->child)) || (err=security_session_attach(op->child,l->session_id)) ||
        (err=credential_environment(&op->strings,op->security_copy,&profile,op->child))) goto done;
    if ((err=security_role_prepare(parent,op->child,l->purpose==1 ? 6 : 7,path))) goto done;
    l->service_generation=security_role_generation(op->child);
    l->pid=op->child->pid; l->process_generation=security_process_generation(op->child);
    proc_set_name(op->child,path); proc_share_group(op->child);
    if (w32_service_delegate) w32_service_delegate(parent,op->child,path);
    credential_text(op->security_copy->text,0,profile.path,"/Documents");
    if ((err=px_security_cwd(op->child,op->security_copy->text)) ||
        (err=security_launch_resources(op->child,l))) goto done;
    /* The adapter owns fresh private stdio, console/grant/notification fds.
     * No live SYSTEM description is inherited even when its flags change. */
    int32_t required[]={l->child_console_fd,l->purpose==1 ? l->child_display_fd : l->child_service_fd,
        l->purpose==1 ? l->child_input_fd : l->child_service_fd};
    for (unsigned i=0;i<ARRAY_SIZE(required);i++) if (required[i]<3 || required[i]>=CIUKI_OPEN_MAX ||
        !op->child->fds[required[i]].object) { err=-EIO; goto done; }
    if (required[0]==required[1] || required[0]==required[2] ||
        (l->purpose==1 && required[1]==required[2]) ||
        (l->purpose==1 ? l->child_service_fd!=-1 : l->child_display_fd!=-1 || l->child_input_fd!=-1)) {
        err=-EIO; goto done;
    }
    struct proc_object *console=op->child->fds[l->child_console_fd].object;
    for (unsigned i=0;i<3;i++) if (op->child->fds[i].object!=console) { err=-EIO; goto done; }
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) if (op->child->fds[i].object) {
        op->child->fds[i].object->inheritable=false;
        op->child->fds[i].flags|=FD_CLOEXEC;
    }
    int32_t pair[2];
    if ((err=channel_pair(parent,pair))) goto done;
    op->control_fd=pair[0]; op->control_owned=true;
    op->control_object=parent->fds[pair[0]].object; op->control_object->retain(op->control_object);
    err=channel_endpoint_handoff(parent,pair[1],op->child,&l->child_ready_fd,true);
    if (err) { desktop_close(parent,pair[1]); goto done; }
    if ((err=channel_control_bind(parent,pair[0]))) goto done;
    desktop_fd(parent,pair[0])->object.inheritable=false;
    l->control_fd=pair[0];
    if ((err=security_launch_environment(op->strings,l))) goto done;
    self->launch_loading=op->child;
    if ((err=security_exec_open(op->child,path,&op->file))) goto done;
    if (w32_is_pe && w32_is_pe(&op->file)) { err=-ENOEXEC; goto done; }
    if ((err=prepare_image(op->child,&op->file,op->strings,0))) goto done;
    if (op->file.close) op->file.close(op->file.cookie);
    op->file.close=0;
    if (!security_logon(parent) || parent->state!=PROC_LIVE || !security_process_live(op->child) ||
        op->child->state!=PROC_PREPARING || proc_signal_caught(self) ||
        parent->fds[op->control_fd].object!=op->control_object) { err=-EACCES; goto done; }
    *out=op->child; op->child=0; op->control_owned=false;
done:
    if (err) l->control_fd=-1; /* cleanup consumes the exact owned descriptor */
    proc_thread_cleanup(self);
    return err;
}

int proc_spawn(uint32_t args_va)
{
    uint64_t spawn_us = proc_launch_now();
    struct process *parent = proc_current();
    uint64_t click_us = parent->launch_pending ? parent->launch_click_us : spawn_us;
    parent->launch_pending = false; /* even an invalid or cancelled attempt consumes BEGIN */
    struct ciuki_spawn_args a;
    int err = copy_from_user(&a, args_va, sizeof(a));
    if (err)
        return err;
    if (a.size != sizeof(a) || a.reserved || (a.flags & ~CIUKI_SPAWN_NEW_GROUP) ||
        a.fd_count > CIUKI_OPEN_MAX || !a.argv)
        return -EINVAL;
    struct proc_thread *self = proc_thread_for(g_current);
    struct spawn_operation *op = kzalloc(sizeof(*op));
    if (!op)
        return -ENOMEM;
    op->thread = self;
    self->cleanup = spawn_cleanup;
    self->operation = op;
    char *path = op->path = kmalloc(CIUKI_PATH_MAX);
    struct ciuki_spawn_fd *fds = op->fds = a.fd_count ? kmalloc(a.fd_count * sizeof(*fds)) : 0;
    struct proc_strings *strings = op->strings = proc_strings_new();
    if (!path || (a.fd_count && !fds) || !strings) {
        err = -ENOMEM;
        goto out;
    }
    if (a.fd_count && (err = copy_from_user(fds, a.fd_list, a.fd_count * sizeof(*fds))))
        goto out;
    if ((err = proc_fd_validate(self->process, fds, a.fd_count)))
        goto out;
    unsigned n;
    for (n = 0; n < CIUKI_PATH_MAX; n++) {
        if (a.path > UINT32_MAX - n || (err = copy_from_user(path + n, a.path + n, 1))) {
            err = -EFAULT;
            goto out;
        }
        if (!path[n])
            break;
    }
    if (n == CIUKI_PATH_MAX) {
        err = -ENAMETOOLONG;
        goto out;
    }
    if ((err = proc_strings_copy(strings, a.argv, a.envp)))
        goto out;
    /* All copied ranges have passed validation. Reserve the child privately
     * and retain the inheritance snapshot before file operations can wait. */
    if ((err = proc_prepare(self->process, &op->child)))
        goto out;
    proc_set_name(op->child, path);
    proc_launch_start(parent, op->child, spawn_us, click_us);
    if ((err = proc_inherit(op->child, self->process, fds, a.fd_count)))
        goto out;
    if (proc_signal_caught(self)) {
        err = -EINTR;
        goto out;
    }
    if (!file_ops) {
        err = -ENOENT;                 /* volumes not yet mounted (f1-09) */
        goto out;
    }
    uint64_t mask = self->mask;
    err = file_ops->open(op->child->cwd, path, &op->file);
    if (err)
        goto out;
    if (self->process->state != PROC_LIVE || !security_process_live(op->child)) {
        err = -EINTR;
        goto out;
    }
    err = prepare_image(op->child, &op->file, strings, mask);
    if (!err && (self->process->state != PROC_LIVE || !security_process_live(op->child) || proc_signal_caught(self)))
        err = -EINTR;
    if (!err) {
        /* Release the immutable file snapshot before publication. */
        if (op->file.close)
            op->file.close(op->file.cookie);
        op->file.close = 0;
        if (self->process->state != PROC_LIVE || !security_process_live(op->child)) {
            err = -EINTR;
            goto out;
        }
        if (supervisor_dosvm_delegate)
            supervisor_dosvm_delegate(self->process, op->child, path);
        if(w32_service_delegate)w32_service_delegate(self->process,op->child,path);
        if (!security_process_live(op->child) || self->process->state != PROC_LIVE) {
            err = -EACCES;
            goto out;
        }
        proc_publish(op->child, a.flags);
        err = (int)op->child->pid;
        op->child = 0;
    }
out:
    proc_thread_cleanup(self);
    return err;
}
