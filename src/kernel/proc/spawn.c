/* Bounded one-time argument snapshots and atomic spawn preparation.
 * SysV stack: https://www.sco.com/developers/devspecs/abi386-4.pdf
 * Ciuki fixes the auxv tags, registers and ARG_MAX accounting in abi.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>

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

static int prepare_image(struct process *p, const struct ciuki_file *file,
                          const struct proc_strings *strings, uint64_t mask)
{
    struct elf_image image;
    int err = elf_validate(file, &image);
    if (err)
        return err;
    err = elf_load(file, &image, p->memory);
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
        err = prepare_image(p, file, strings, mask);
    if (!err && parent->state != PROC_LIVE)
        err = -EINTR;
    if (err) {
        proc_discard(p);
        return err;
    }
    proc_publish(p, flags);
    *out = p;
    return (int)p->pid;
}

struct spawn_operation {
    struct process *child;
    struct proc_strings *strings;
    struct ciuki_spawn_fd *fds;
    char *path;
    struct ciuki_file file;
};

static void spawn_cleanup(void *operation)
{
    struct spawn_operation *op = operation;
    if (op->file.close)
        op->file.close(op->file.cookie);
    if (op->child)
        proc_discard(op->child);
    proc_strings_free(op->strings);
    kfree(op->fds);
    kfree(op->path);
    kfree(op);
}

int proc_spawn(uint32_t args_va)
{
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
    if (self->process->state != PROC_LIVE) {
        err = -EINTR;
        goto out;
    }
    err = prepare_image(op->child, &op->file, strings, mask);
    if (!err && (self->process->state != PROC_LIVE || proc_signal_caught(self)))
        err = -EINTR;
    if (!err) {
        /* Release the immutable file snapshot before publication. */
        if (op->file.close)
            op->file.close(op->file.cookie);
        op->file.close = 0;
        if (self->process->state != PROC_LIVE) {
            err = -EINTR;
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
