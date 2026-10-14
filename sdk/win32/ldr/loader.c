/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <w32/ldr.h>
enum { M_MAPPING = 1, M_BOUND, M_ATTACHING, M_READY };
static int error(struct w32_loader *l, ULONG code, const char *reason, const char *module,
                 const char *symbol) {
    if (!l->error.reason) {
        l->error.code = code;
        l->error.reason = reason;
        w32_copy_string(l->error.module, sizeof(l->error.module), module ? module : "");
        w32_copy_string(l->error.symbol, sizeof(l->error.symbol), symbol ? symbol : "");
        char detail[512];
        ULONG n = w32_strlen(reason);
        w32_copy_string(detail, sizeof(detail), reason);
        if (symbol && *symbol && n + 2 + w32_strlen(symbol) < sizeof(detail)) {
            detail[n++] = ':';
            w32_copy_string(detail + n, sizeof(detail) - n, symbol);
        }
        w32_diag("load", module ? module : "", detail);
    }
    return 0;
}
static void clear_error(struct w32_loader *l) {
    w32_fill(&l->error, 0, sizeof(l->error));
}
static const char *basename(const char *p) {
    const char *n = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/')
            n = p + 1;
    return n;
}
static int fullpath(const char *p) {
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            return 1;
    return 0;
}
static int dllname(char *out, const char *name) {
    if (!w32_copy_string(out, 256, basename(name)))
        return 0;
    for (const char *p = out; *p; p++)
        if (*p == '.')
            return 1;
    ULONG n = w32_strlen(out);
    if (n > 251)
        return 0;
    w32_copy(out + n, ".dll", 5);
    return 1;
}
static void list_init(LIST_ENTRY *e) {
    e->Flink = e->Blink = e;
}
static void list_add(LIST_ENTRY *head, LIST_ENTRY *e) {
    e->Flink = head;
    e->Blink = head->Blink;
    head->Blink->Flink = e;
    head->Blink = e;
}
static void list_remove(LIST_ENTRY *e) {
    if (e->Flink) {
        e->Flink->Blink = e->Blink;
        e->Blink->Flink = e->Flink;
        e->Flink = e->Blink = 0;
    }
}
static void wide(UNICODE_STRING *s, WCHAR *buf, const char *name) {
    ULONG n = w32_strlen(name);
    for (ULONG i = 0; i <= n; i++)
        buf[i] = (uint8_t)name[i];
    s->Buffer = buf;
    s->Length = n * 2;
    s->MaximumLength = (n + 1) * 2;
}
int w32_builtin_validate(const struct w32_builtin *b, ULONG count, struct w32_error *e) {
    if (count && !b) {
        e->code = 193;
        e->reason = "builtin-manifest";
        return 0;
    }
    for (ULONG i = 0; i < count; i++) {
        int ok = b[i].name && b[i].path && b[i].sha256 && w32_strlen(b[i].sha256) == 64 &&
                 b[i].base >= 0x70000000 && !(b[i].base & 65535) && b[i].size &&
                 b[i].base < 0x7ffe0000 && b[i].size <= 0x7ffe0000 - b[i].base;
        if (ok)
            for (ULONG k = 0; k < 64; k++) {
                unsigned c = (uint8_t)b[i].sha256[k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    ok = 0;
            }
        for (ULONG j = 0; ok && j < i; j++)
            if (!w32_stricmp(b[i].name, b[j].name) ||
                (b[i].base < b[j].base + b[j].size && b[j].base < b[i].base + b[i].size))
                ok = 0;
        if (!ok) {
            e->code = 193;
            e->reason = "builtin-manifest";
            return 0;
        }
    }
    return 1;
}
void w32_ldr_init(struct w32_loader *l, const struct w32_services *s, struct w32_peb *peb) {
    w32_fill(l, 0, sizeof(*l));
    l->services = s;
    l->peb = peb;
    w32_set_services(s);
    RtlInitializeCriticalSection(&l->lock);
    w32_copy_string(l->exe_directory, sizeof(l->exe_directory), "C:\\");
    w32_copy_string(l->current_directory, sizeof(l->current_directory), "C:\\");
    w32_copy_string(l->system_directory, sizeof(l->system_directory), "C:\\WINDOWS\\SYSTEM");
    w32_copy_string(l->windows_directory, sizeof(l->windows_directory), "C:\\WINDOWS");
    l->lists.Length = sizeof(l->lists);
    l->lists.Initialized = 1;
    list_init(&l->lists.InLoadOrderModuleList);
    list_init(&l->lists.InMemoryOrderModuleList);
    list_init(&l->lists.InInitializationOrderModuleList);
    if (peb) {
        peb->Ldr = &l->lists;
        peb->FastPebLock = &l->lock;
    }
}
struct w32_module *w32_ldr_module(struct w32_loader *l, const char *name) {
    if (!name)
        return l->main;
    char base[256];
    if (!dllname(base, name))
        return 0;
    for (struct w32_module *m = l->modules; m; m = m->next)
        if (!(m->load_flags & W32_LOAD_DATAFILE) && !w32_stricmp(m->name, base))
            return m;
    return 0;
}
static int open_join(struct w32_loader *l, const char *dir, const char *name, char *path,
                     struct w32_file *file) {
    ULONG a = w32_strlen(dir), b = w32_strlen(name);
    if (a + b + 2 > W32_PATH_MAX)
        return 0;
    w32_copy(path, dir, a);
    if (a && path[a - 1] != '\\' && path[a - 1] != '/')
        path[a++] = '\\';
    w32_copy(path + a, name, b + 1);
    return l->services->open(l->services->opaque, path, file);
}
static int locate(struct w32_loader *l, const char *name, char *path, struct w32_file *file,
                  const struct w32_builtin **builtin) {
    char base[256];
    if (!dllname(base, name))
        return error(l, 193, "module-name-limit", name, 0);
    *builtin = 0;
    for (ULONG i = 0; i < l->builtin_count; i++)
        if (!w32_stricmp(l->builtins[i].name, base)) {
            *builtin = &l->builtins[i];
            w32_copy_string(path, W32_PATH_MAX, (*builtin)->path);
            if (l->services->open(l->services->opaque, path, file))
                return 1;
            return error(l, 126, "missing-dll", name, 0);
        }
    if (fullpath(name)) {
        if (!w32_copy_string(path, W32_PATH_MAX, name))
            return error(l, 193, "module-name-limit", name, 0);
        if (l->services->open(l->services->opaque, path, file))
            return 1;
    } else {
        const char *dirs[] = {l->exe_directory, l->current_directory, l->system_directory,
                              l->windows_directory};
        for (ULONG i = 0; i < 4; i++)
            if (open_join(l, dirs[i], base, path, file))
                return 1;
        const char *p = l->path;
        while (p && *p) {
            char dir[W32_PATH_MAX];
            ULONG n = 0;
            while (*p && *p != ';') {
                if (n + 1 < sizeof(dir))
                    dir[n++] = *p;
                p++;
            }
            dir[n] = 0;
            if (*p)
                p++;
            if (n && open_join(l, dir, base, path, file))
                return 1;
        }
    }
    return error(l, 126, "missing-dll", name, 0);
}
static int dependency(struct w32_loader *l, struct w32_module *m, struct w32_module *d) {
    if (m == d)
        return 1;
    for (ULONG i = 0; i < m->dependency_count; i++)
        if (m->dependencies[i] == d)
            return 1;
    if (m->dependency_count == W32_MAX_MODULES)
        return error(l, 8, "dependency-limit", m->name, 0);
    m->dependencies[m->dependency_count++] = d;
    return 1;
}
static struct w32_module *load_one(struct w32_loader *, const char *, ULONG, ULONG);
static void *resolve(struct w32_loader *l, struct w32_module *m, const char *name, ULONG ordinal,
                     ULONG hint, ULONG depth) {
    struct w32_pe *p = &m->pe;
    struct w32_directory d = p->dirs[0];
    if (!d.size || (m->load_flags & W32_LOAD_DATAFILE)) {
        error(l, 127, "missing-export", m->name, name);
        return 0;
    }
    uint8_t *head = w32_pe_span(p, d.rva, 40);
    if (!head)
        return 0;
    ULONG base = w32_u32(head + 16), nf = w32_u32(head + 20), nn = w32_u32(head + 24),
          index = 0xffffffffu;
    uint8_t *funcs = w32_pe_span(p, w32_u32(head + 28), nf * 4),
            *names = w32_pe_span(p, w32_u32(head + 32), nn * 4),
            *ords = w32_pe_span(p, w32_u32(head + 36), nn * 2);
    if (!funcs || !names || !ords)
        return 0;
    if (!name) {
        if (ordinal >= base && ordinal - base < nf)
            index = ordinal - base;
    } else {
        if (hint < nn) {
            const char *s = w32_pe_string(p, w32_u32(names + hint * 4));
            if (s && !w32_strcmp(s, name))
                index = w32_u16(ords + hint * 2);
        }
        if (index == 0xffffffffu) {
            ULONG lo = 0, hi = nn;
            while (lo < hi) {
                ULONG mid = lo + (hi - lo) / 2;
                const char *s = w32_pe_string(p, w32_u32(names + mid * 4));
                if (!s)
                    return 0;
                int cmp = w32_strcmp(name, s);
                if (!cmp) {
                    index = w32_u16(ords + mid * 2);
                    break;
                }
                if (cmp < 0)
                    hi = mid;
                else
                    lo = mid + 1;
            }
        }
    }
    if (index >= nf || !w32_u32(funcs + index * 4)) {
        char number[8] = "#";
        ULONG n = ordinal & 65535, digits = 1;
        for (ULONG v = n; v >= 10; v /= 10)
            digits++;
        for (ULONG i = digits; i; i--, n /= 10)
            number[i] = '0' + n % 10;
        number[digits + 1] = 0;
        error(l, 127, "missing-export", m->name, name ? name : number);
        return 0;
    }
    ULONG rva = w32_u32(funcs + index * 4);
    if (rva >= d.rva && rva - d.rva < d.size) {
        if (depth >= 16) {
            error(l, 127, "forwarder-depth", m->name, name);
            return 0;
        }
        const char *f = w32_pe_string(p, rva);
        if (!f)
            return 0;
        char dll[256];
        ULONG at = 0;
        while (f[at] && f[at] != '.' && at < 251) {
            dll[at] = f[at];
            at++;
        }
        if (!at || f[at] != '.' || !f[at + 1]) {
            error(l, 193, "export-forwarder-format", m->name, name);
            return 0;
        }
        w32_copy(dll + at, ".dll", 5);
        struct w32_module *target = load_one(l, dll, 0, depth + 1);
        if (!target || !dependency(l, m, target))
            return 0;
        const char *symbol = f + at + 1;
        ULONG ord = 0;
        if (*symbol == '#') {
            symbol++;
            if (!*symbol) {
                error(l, 193, "export-forwarder-format", m->name, name);
                return 0;
            }
            while (*symbol) {
                if (*symbol < '0' || *symbol > '9' ||
                    ord > (65535u - (ULONG)(*symbol - '0')) / 10) {
                    error(l, 193, "export-forwarder-format", m->name, name);
                    return 0;
                }
                ord = ord * 10 + (*symbol++ - '0');
            }
            return resolve(l, target, 0, ord, 0xffffffffu, depth + 1);
        }
        return resolve(l, target, symbol, 0, 0xffffffffu, depth + 1);
    }
    return w32_pe_span(p, rva, 1);
}
static int bind_imports(struct w32_loader *l, struct w32_module *m, ULONG depth) {
    struct w32_pe *p = &m->pe;
    struct w32_directory d = p->dirs[1];
    for (ULONG off = 0; off < d.size; off += 20) {
        uint8_t *t = w32_pe_span(p, d.rva + off, 20);
        if (!t)
            return 0;
        ULONG any = 0;
        for (ULONG i = 0; i < 20; i++)
            any |= t[i];
        if (!any)
            break;
        ULONG ilt = w32_u32(t), iat = w32_u32(t + 16);
        if (ilt && !w32_u32(p->image + ilt))
            ilt = 0;
        if (!ilt && w32_u32(t + 4) && w32_u32(p->image + iat) >= p->size)
            return error(l, 193, "bound-iat-without-names", m->name, 0);
        const char *dll = w32_pe_string(p, w32_u32(t + 12));
        if (!dll)
            return 0;
        struct w32_module *dep = load_one(l, dll, 0, depth + 1);
        if (!dep || !dependency(l, m, dep))
            return 0;
        for (ULONG i = 0; i < 65536; i++) {
            ULONG v = w32_u32(p->image + (ilt ? ilt : iat) + i * 4);
            if (!v)
                break;
            const char *name = v & 0x80000000 ? 0 : w32_pe_string(p, v + 2);
            ULONG hint = name ? w32_u16(p->image + v) : 0xffffffffu;
            void *address = resolve(l, dep, name, v & 65535, hint, 0);
            if (!address)
                return 0;
            w32_put32(p->image + iat + i * 4, (ULONG)(uintptr_t)address);
        }
    }
    return 1;
}
static int prepare_tls(struct w32_loader *l, struct w32_module *m) {
    m->tls_index = 0xffffffffu;
    if (!m->pe.dirs[9].size)
        return 1;
    ULONG index = 0;
    while (index < W32_MAX_TLS && l->tls_used[index])
        index++;
    if (index == W32_MAX_TLS)
        return error(l, 8, "tls-index-limit", m->name, 0);
    uint8_t *d = m->pe.image + m->pe.dirs[9].rva;
    m->tls_template = w32_u32(d) ? w32_u32(d) - m->pe.base : 0;
    m->tls_size = w32_u32(d + 4) - w32_u32(d);
    m->tls_zero = w32_u32(d + 16);
    ULONG alignment = (w32_u32(d + 20) >> 20) & 15;
    if (alignment == 15 || m->tls_size > 0x20000000 || m->tls_zero > 0x20000000 - m->tls_size)
        return error(l, 193, "tls-size-alignment", m->name, 0);
    m->tls_alignment = alignment ? 1u << (alignment - 1) : 4;
    m->tls_index = index;
    l->tls_used[index] = 1;
    m->entry.TlsIndex = index;
    w32_put32(m->pe.image + w32_u32(d + 8) - m->pe.base, index);
    return 1;
}
static int thread_tls(struct w32_loader *l, struct w32_module *m, struct w32_ldr_thread *t) {
    if (m->tls_index == 0xffffffffu || t->tls[m->tls_index])
        return 1;
    ULONG size = m->tls_size + m->tls_zero;
    if (!size)
        size = 1;
    void *data = l->services->alloc(l->services->opaque, size);
    if (!data)
        return error(l, 8, "tls-allocation", m->name, 0);
    if ((uintptr_t)data & (m->tls_alignment - 1)) {
        l->services->free(l->services->opaque, data, size);
        return error(l, 8, "tls-allocation-alignment", m->name, 0);
    }
    w32_fill(data, 0, size);
    w32_copy(data, m->pe.image + m->tls_template, m->tls_size);
    t->tls[m->tls_index] = data;
    t->sizes[m->tls_index] = size;
    return 1;
}
static int call(struct w32_loader *l, struct w32_module *m, void *proc, ULONG reason, int dll) {
    struct w32_error saved = l->error;
    int result = 1;
    /* lpReserved is non-NULL for static loads at process start and for the
     * detach at process exit, NULL for LoadLibrary/FreeLibrary (DllMain). */
    void *reserved =
        (reason == 1 && l->process_start) || (reason == 0 && l->terminating) ? (void *)1 : 0;
    if (l->services->invoke)
        result = l->services->invoke(l->services->opaque, proc, m->pe.image, reason, dll);
    else if (dll)
        result = ((int(W32API *)(void *, ULONG, void *))proc)(m->pe.image, reason, reserved);
    else
        ((void(W32API *)(void *, ULONG, void *))proc)(m->pe.image, reason, reserved);
    l->error = saved;
    return result;
}
static int notify(struct w32_loader *l, struct w32_module *m, ULONG reason) {
    if (m->load_flags & W32_LOAD_DATAFILE)
        return 1;
    if (reason >= 2 && m->disable_threads)
        return 1;
    if (m->pe.dirs[9].size) {
        ULONG va = w32_u32(m->pe.image + m->pe.dirs[9].rva + 12);
        if (va)
            for (ULONG i = 0; i < 4096; i++) {
                ULONG cb = w32_u32(m->pe.image + va - m->pe.base + i * 4);
                if (!cb)
                    break;
                call(l, m, (void *)(uintptr_t)cb, reason, 0);
            }
    }
    if ((m->pe.flags & 0x2000) && m->pe.entry)
        return call(l, m, m->pe.image + m->pe.entry, reason, 1);
    return 1;
}
static int initialize(struct w32_loader *l, struct w32_module *m) {
    if (m->state == M_READY || m->state == M_ATTACHING)
        return 1;
    if (m->state != M_BOUND)
        return error(l, 193, "module-not-bound", m->name, 0);
    m->state = M_ATTACHING;
    m->busy++;
    for (ULONG i = 0; i < m->dependency_count; i++)
        if (!initialize(l, m->dependencies[i])) {
            m->busy--;
            return 0;
        }
    if (!notify(l, m, 1)) {
        notify(l, m, 0);
        m->state = M_BOUND;
        m->busy--;
        return error(l, 1114, "dllmain-attach-failed", m->name, 0);
    }
    m->state = M_READY;
    m->initialization_order = ++l->sequence;
    if (!(m->load_flags & W32_LOAD_DATAFILE))
        list_add(&l->lists.InInitializationOrderModuleList, &m->entry.InInitializationOrderLinks);
    m->busy--;
    return 1;
}
static struct w32_module *load_one(struct w32_loader *l, const char *name, ULONG flags,
                                   ULONG depth) {
    if (depth > W32_MAX_MODULES) {
        error(l, 193, "import-depth-limit", name, 0);
        return 0;
    }
    if (!(flags & W32_LOAD_DATAFILE)) {
        struct w32_module *m = w32_ldr_module(l, name);
        if (m)
            return m;
    }
    if (l->module_count >= W32_MAX_MODULES) {
        error(l, 8, "module-limit", name, 0);
        return 0;
    }
    struct w32_file f;
    char path[W32_PATH_MAX];
    const struct w32_builtin *builtin;
    if (!locate(l, name, path, &f, &builtin))
        return 0;
    struct w32_module *m = l->services->alloc(l->services->opaque, sizeof(*m));
    if (!m) {
        l->services->close(l->services->opaque, &f);
        error(l, 8, "module-allocation", name, 0);
        return 0;
    }
    w32_fill(m, 0, sizeof(*m));
    m->tls_index = 0xffffffffu;
    m->entry.TlsIndex = 65535;
    m->load_flags = flags;
    w32_copy_string(m->path, sizeof(m->path), path);
    dllname(m->name, name);
    int ok = w32_pe_parse(&m->pe, f.bytes, f.size, &l->error);
    if (ok && (flags & W32_LOAD_EXECUTABLE) && (m->pe.flags & 0x2000))
        ok = error(l, 193, "expected-executable", name, 0);
    if (ok && builtin &&
        (m->pe.preferred != builtin->base || m->pe.size > builtin->size || !(m->pe.flags & 0x2000)))
        ok = error(l, 193, "builtin-image-manifest", name, 0);
    if (ok) {
        ULONG preferred = (flags & W32_LOAD_DATAFILE) ? 0 : m->pe.preferred;
        int exact =
            !(flags & W32_LOAD_DATAFILE) && (builtin || !m->pe.dirs[5].size || (m->pe.flags & 1));
        /* Preferred bases outside the contract's user region are not mapped. */
        if (!(flags & W32_LOAD_DATAFILE) &&
            (preferred < 0x10000 || preferred >= 0x7ffe0000 ||
             m->pe.size > 0x7ffe0000 - preferred ||
             (!builtin && (preferred >= 0x70000000 || m->pe.size > 0x70000000 - preferred))))
            ok = error(l, 193, "image-address-range", name, 0);
        else {
            void *image = l->services->map(l->services->opaque, preferred, m->pe.size, exact);
            if (!image)
                ok = error(l, 193, "relocation-base-collision", name, 0);
            else
                ok = w32_pe_copy_image(&m->pe, image) && w32_pe_validate(&m->pe);
        }
    }
    l->services->close(l->services->opaque, &f);
    m->pe.file = 0;
    m->pe.file_size = 0;
    if (ok && !(flags & W32_LOAD_DATAFILE))
        ok = w32_pe_relocate(&m->pe, (ULONG)(uintptr_t)m->pe.image) && w32_pe_validate(&m->pe);
    if (!ok) {
        if (!l->error.module[0])
            w32_copy_string(l->error.module, sizeof(l->error.module), name);
        if (m->pe.image)
            l->services->unmap(l->services->opaque, m->pe.image, m->pe.size);
        l->services->free(l->services->opaque, m, sizeof(*m));
        return 0;
    }
    m->builtin = builtin != 0;
    m->state = M_MAPPING;
    m->load_order = ++l->sequence;
    m->next = l->modules;
    l->modules = m;
    l->module_count++;
    m->entry.DllBase = m->pe.image;
    m->entry.EntryPoint = m->pe.image + m->pe.entry;
    m->entry.SizeOfImage = m->pe.size;
    wide(&m->entry.FullDllName, m->wide_path, m->path);
    wide(&m->entry.BaseDllName, m->wide_name, m->name);
    if (!(flags & W32_LOAD_DATAFILE)) {
        list_add(&l->lists.InLoadOrderModuleList, &m->entry.InLoadOrderLinks);
        list_add(&l->lists.InMemoryOrderModuleList, &m->entry.InMemoryOrderLinks);
        if (!bind_imports(l, m, depth) || !prepare_tls(l, m))
            return 0;
        for (struct w32_ldr_thread *t = l->threads; t; t = t->next)
            if (!thread_tls(l, m, t))
                return 0;
    }
    if (!w32_pe_protect(&m->pe, l->services))
        return 0;
    m->state = M_BOUND;
    return m;
}
static void destroy_module(struct w32_loader *l, struct w32_module *m) {
    if (m->tls_index != 0xffffffffu) {
        for (struct w32_ldr_thread *t = l->threads; t; t = t->next)
            if (t->tls[m->tls_index]) {
                l->services->free(l->services->opaque, t->tls[m->tls_index],
                                  t->sizes[m->tls_index]);
                t->tls[m->tls_index] = 0;
                t->sizes[m->tls_index] = 0;
            }
        l->tls_used[m->tls_index] = 0;
    }
    list_remove(&m->entry.InLoadOrderLinks);
    list_remove(&m->entry.InMemoryOrderLinks);
    list_remove(&m->entry.InInitializationOrderLinks);
    struct w32_module **link = &l->modules;
    while (*link && *link != m)
        link = &(*link)->next;
    if (*link)
        *link = m->next;
    if (l->main == m) {
        l->main = 0;
        if (l->peb)
            l->peb->ImageBaseAddress = 0;
    }
    l->module_count--;
    l->services->unmap(l->services->opaque, m->pe.image, m->pe.size);
    l->services->free(l->services->opaque, m, sizeof(*m));
}
static void mark(struct w32_module *m) {
    if (m->mark)
        return;
    m->mark = 1;
    for (ULONG i = 0; i < m->dependency_count; i++)
        mark(m->dependencies[i]);
}
static void mark_roots(struct w32_loader *l, int shutdown) {
    for (struct w32_module *m = l->modules; m; m = m->next)
        m->mark = 0;
    if (!shutdown)
        for (struct w32_module *m = l->modules; m; m = m->next)
            if (m->references || m->builtin || m->busy)
                mark(m);
}
static void collect(struct w32_loader *l, int shutdown) {
    if (l->collecting)
        return;
    l->collecting = 1;
    l->transaction_depth++;
    mark_roots(l, shutdown);
    /* Detach all selected modules before unmapping any dependency. */
    for (;;) {
        struct w32_module *latest = 0;
        for (struct w32_module *m = l->modules; m; m = m->next)
            if (!m->mark && m->state == M_READY &&
                (!latest || m->initialization_order > latest->initialization_order))
                latest = m;
        if (!latest)
            break;
        latest->state = M_BOUND;
        notify(l, latest, 0);
        mark_roots(l, shutdown);
    }
    struct w32_module *m = l->modules;
    while (m) {
        struct w32_module *next = m->next;
        if (!m->mark)
            destroy_module(l, m);
        m = next;
    }
    l->transaction_depth--;
    l->collecting = 0;
}
int w32_ldr_load(struct w32_loader *l, const char *name, ULONG flags, struct w32_module **out) {
    if (out)
        *out = 0;
    if (!name || !out || flags & ~(W32_LOAD_DATAFILE | W32_LOAD_EXECUTABLE))
        return error(l, 87, "load-flags", name, 0);
    if (RtlEnterCriticalSection(&l->lock))
        return 0;
    clear_error(l);
    if (!w32_builtin_validate(l->builtins, l->builtin_count, &l->error)) {
        RtlLeaveCriticalSection(&l->lock);
        return 0;
    }
    /* Snapshot dependency lists too: GetProcAddress forwarders can add edges
     * to an existing module during a recursive initialization transaction. */
    struct {
        struct w32_module *m;
        ULONG deps;
    } old[W32_MAX_MODULES];
    ULONG count = 0, cutoff = l->sequence;
    for (struct w32_module *m = l->modules; m; m = m->next) {
        old[count].m = m;
        old[count++].deps = m->dependency_count;
    }
    l->transaction_depth++;
    struct w32_module *m = load_one(l, name, flags, 0);
    int ok = m && initialize(l, m);
    if (ok) {
        m->references++;
        m->entry.LoadCount = m->references > 65535 ? 65535 : m->references;
        *out = m;
        if (flags & W32_LOAD_EXECUTABLE) {
            l->main = m;
            if (l->peb)
                l->peb->ImageBaseAddress = m->pe.image;
        }
        collect(l, 0);
    } else {
        struct w32_error saved = l->error;
        for (ULONG i = 0; i < count; i++)
            old[i].m->dependency_count = old[i].deps;
        /* Roll back just this transaction, including builtins first mapped
         * during it; retain successful recursive loads with external refs. */
        for (struct w32_module *n = l->modules; n; n = n->next) {
            n->mark = n->load_order <= cutoff || n->references;
            n->builtin = n->builtin && n->mark;
        }
        collect(l, 0);
        l->error = saved;
    }
    l->transaction_depth--;
    RtlLeaveCriticalSection(&l->lock);
    return ok;
}
void *w32_ldr_proc(struct w32_loader *l, struct w32_module *m, const char *name, ULONG ordinal,
                   ULONG hint) {
    if (!m || RtlEnterCriticalSection(&l->lock))
        return 0;
    clear_error(l);
    /* Pin the source while a forwarded DLL is resolved and initialized. */
    m->references++;
    ULONG olddeps = m->dependency_count, cutoff = l->sequence;
    void *p = resolve(l, m, name, ordinal, hint, 0);
    if (p)
        for (ULONG i = 0; i < m->dependency_count; i++)
            if (!initialize(l, m->dependencies[i])) {
                p = 0;
                break;
            }
    if (!p) {
        m->dependency_count = olddeps;
        for (struct w32_module *n = l->modules; n; n = n->next)
            if (n->load_order > cutoff && !n->references && !n->busy)
                n->builtin = 0;
    }
    m->references--;
    if (!p && !l->transaction_depth) {
        struct w32_error saved = l->error;
        collect(l, 0);
        l->error = saved;
    }
    RtlLeaveCriticalSection(&l->lock);
    return p;
}
int w32_ldr_free(struct w32_loader *l, struct w32_module *m) {
    if (!m || RtlEnterCriticalSection(&l->lock))
        return 0;
    if (l->transaction_depth) {
        RtlLeaveCriticalSection(&l->lock);
        return error(l, 87, "unload-during-initialization", m->name, 0);
    }
    int found = 0;
    for (struct w32_module *n = l->modules; n; n = n->next)
        if (n == m)
            found = 1;
    if (!found || !m->references) {
        RtlLeaveCriticalSection(&l->lock);
        return 0;
    }
    if (!m->builtin)
        m->references--;
    m->entry.LoadCount = m->references > 65535 ? 65535 : m->references;
    collect(l, 0);
    RtlLeaveCriticalSection(&l->lock);
    return 1;
}
int w32_ldr_thread_attach(struct w32_loader *l, struct w32_teb *teb) {
    if (!teb || RtlEnterCriticalSection(&l->lock))
        return 0;
    for (struct w32_ldr_thread *n = l->threads; n; n = n->next)
        if (n->teb == teb) {
            RtlLeaveCriticalSection(&l->lock);
            return 1;
        }
    struct w32_ldr_thread *t = l->services->alloc(l->services->opaque, sizeof(*t));
    if (!t) {
        RtlLeaveCriticalSection(&l->lock);
        return 0;
    }
    w32_fill(t, 0, sizeof(*t));
    t->teb = teb;
    t->next = l->threads;
    l->threads = t;
    teb->ThreadLocalStoragePointer = t->tls;
    int ok = 1;
    for (struct w32_module *m = l->modules; m; m = m->next)
        if (!thread_tls(l, m, t)) {
            ok = 0;
            break;
        }
    if (ok) {
        l->transaction_depth++;
        for (ULONG seq = 1; seq <= l->sequence; seq++)
            for (struct w32_module *m = l->modules; m; m = m->next)
                if (m->state == M_READY && m->initialization_order == seq)
                    notify(l, m, 2);
        l->transaction_depth--;
    }
    if (!ok) {
        l->threads = t->next;
        for (ULONG i = 0; i < W32_MAX_TLS; i++)
            if (t->tls[i])
                l->services->free(l->services->opaque, t->tls[i], t->sizes[i]);
        l->services->free(l->services->opaque, t, sizeof(*t));
        teb->ThreadLocalStoragePointer = 0;
    }
    RtlLeaveCriticalSection(&l->lock);
    return ok;
}
void w32_ldr_thread_detach(struct w32_loader *l, struct w32_teb *teb) {
    if (RtlEnterCriticalSection(&l->lock))
        return;
    struct w32_ldr_thread **link = &l->threads;
    while (*link && (*link)->teb != teb)
        link = &(*link)->next;
    if (*link) {
        struct w32_ldr_thread *t = *link;
        l->transaction_depth++;
        for (ULONG seq = l->sequence; seq; seq--)
            for (struct w32_module *m = l->modules; m; m = m->next)
                if (m->state == M_READY && m->initialization_order == seq)
                    notify(l, m, 3);
        l->transaction_depth--;
        *link = t->next;
        for (ULONG i = 0; i < W32_MAX_TLS; i++)
            if (t->tls[i])
                l->services->free(l->services->opaque, t->tls[i], t->sizes[i]);
        l->services->free(l->services->opaque, t, sizeof(*t));
        teb->ThreadLocalStoragePointer = 0;
    }
    RtlLeaveCriticalSection(&l->lock);
}
void w32_ldr_shutdown(struct w32_loader *l) {
    if (RtlEnterCriticalSection(&l->lock))
        return;
    l->terminating = 1;
    collect(l, 1);
    l->terminating = 0;
    while (l->threads) {
        struct w32_ldr_thread *t = l->threads;
        l->threads = t->next;
        t->teb->ThreadLocalStoragePointer = 0;
        l->services->free(l->services->opaque, t, sizeof(*t));
    }
    RtlLeaveCriticalSection(&l->lock);
}
/* Process start (contract "Imports, exports and binding"): the Windows 98
 * texts for a missing DLL or export, also recorded as a W21 diagnostic. */
static int append(char *out, ULONG n, ULONG *at, const char *text) {
    ULONG k = w32_strlen(text);
    if (*at >= n || k >= n - *at)
        return 0;
    w32_copy(out + *at, text, k + 1);
    *at += k;
    return 1;
}
int w32_ldr_start_message(const struct w32_loader *l, const char *exe, char *out, ULONG n) {
    ULONG at = 0;
    if (!out || !n)
        return 0;
    out[0] = 0;
    if (l->error.code == 126) {
        char directory[W32_PATH_MAX];
        w32_copy_string(directory, sizeof(directory), l->exe_directory);
        ULONG k = w32_strlen(directory);
        if (k > 3 && (directory[k - 1] == '\\' || directory[k - 1] == '/'))
            directory[k - 1] = 0;
        int ok = append(out, n, &at, "The dynamic link library ") &&
                 append(out, n, &at, l->error.module) &&
                 append(out, n, &at, " could not be found in the specified path ") &&
                 append(out, n, &at, directory) && append(out, n, &at, ";.;") &&
                 append(out, n, &at, l->system_directory) && append(out, n, &at, ";") &&
                 append(out, n, &at, l->windows_directory);
        if (ok && l->path && *l->path)
            ok = append(out, n, &at, ";") && append(out, n, &at, l->path);
        return ok && append(out, n, &at, ".");
    }
    if (l->error.code == 127)
        return append(out, n, &at, "The ") && append(out, n, &at, basename(exe)) &&
               append(out, n, &at, " file is linked to missing export ") &&
               append(out, n, &at, l->error.module) && append(out, n, &at, ":") &&
               append(out, n, &at, l->error.symbol) && append(out, n, &at, ".");
    return 0;
}
/* Map the EXE first (nothing else in the application region before it),
 * create the process heap, then bind and initialise. Returns the NTSTATUS
 * the process exits with on failure. */
NTSTATUS w32_ldr_start_process(struct w32_loader *l, const char *exe, struct w32_module **out) {
    if (out)
        *out = 0;
    if (!exe || !out)
        return STATUS_INVALID_PARAMETER;
    const char *name = basename(exe);
    if (name != exe && (ULONG)(name - exe) < sizeof(l->exe_directory)) {
        w32_copy(l->exe_directory, exe, name - exe);
        l->exe_directory[name - exe] = 0;
    }
    if (l->peb && !l->peb->ProcessHeap) {
        l->peb->ProcessHeap = RtlCreateHeap(HEAP_GROWABLE, 0, 0x100000, 0x10000, 0, 0);
        if (!l->peb->ProcessHeap)
            return STATUS_NO_MEMORY;
    }
    l->process_start = 1;
    int loaded = w32_ldr_load(l, exe, W32_LOAD_EXECUTABLE, out);
    l->process_start = 0;
    if (loaded)
        return STATUS_SUCCESS;
    char message[W32_PATH_MAX * 2];
    if (w32_ldr_start_message(l, exe, message, sizeof(message)))
        w32_diag("dialog", name, message);
    switch (l->error.code) {
    case 126:
        return STATUS_DLL_NOT_FOUND;
    case 127:
        return STATUS_ENTRYPOINT_NOT_FOUND;
    case 1114:
        return STATUS_DLL_INIT_FAILED;
    case 8:
        return STATUS_NO_MEMORY;
    default:
        return (NTSTATUS)0xc000007b; /* STATUS_INVALID_IMAGE_FORMAT */
    }
}
