/* Lua application controller. Upstream sources and tests stay unmodified.
 * Research: https://www.lua.org/tests/ and
 * https://www.lua.org/manual/5.4/manual.html#pdf-os.clock . See the f2-15
 * host report for timing, capture and sampled-memory qualification limits.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <ciuki/probe.h>
#include <ciuki/storage.h>
#include <ciuki/files.h>

#define GATE_NAME "app-gate"
#define GATE_CWD "/system/tests/lua-5.4.8-tests"
#define GATE_PROVENANCE "/system/tests/app-gate.meta"
#define GATE_BUDGET_MS 870000u /* leave boot/cleanup room inside the 900 s boot */

/* The controller runs once per boot on one 8 KiB task stack. Its large
 * scratch buffers are static and distinct: provenance parsing calls the
 * payload digester while the provenance read buffer is still live. Small
 * format/chunk buffers stay on the stack; evidence and bounds are unchanged. */
static const char *const gate_cases[] = { "file-roundtrip", "allocation", "time-utc", "console" };
static const char *const gate_fields[] = {
    "sdk_manifest_sha256", "newlib_source_sha256", "newlib_patch_hashes",
    "application_source_sha256", "application_tests_sha256", "declared_exclusions"
};
static int gate_compare(const char *a, const char *b) { return strncmp(a,b,strlen(b)+1); }

/* No raw application/provenance text is ever interpolated into a record.
 * The sidecar uses six tokens, with bounded validated identifiers and hex. */
static bool gate_hex(const char *s, unsigned min, unsigned max)
{
    unsigned n = 0;
    for (; s[n]; n++) if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return false;
    return n >= min && n <= max && !(n & 1);
}
static bool gate_number(const char *s, uint32_t *out)
{
    uint32_t n = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT32_MAX - (unsigned)(*s - '0')) / 10) return false;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n; return true;
}
static unsigned gate_tokens(char *line, char **tokens, unsigned capacity)
{
    unsigned n = 0;
    for (char *p = line; *p;) {
        if (n == capacity || *p == ' ') return 0;
        tokens[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p++ = 0; if (!*p) return 0; }
    }
    return n;
}
static void gate_metadata(const char *name, const char *encoding, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    unsigned length = (unsigned)strlen(value), parts = (length + 15) / 16;
    for (unsigned part = 0; part < parts; part++) {
        char encoded[33];
        unsigned n = length - part * 16; if (n > 16) n = 16;
        for (unsigned i = 0; i < n; i++) {
            uint8_t b = (uint8_t)value[part * 16 + i];
            encoded[2*i] = hex[b >> 4]; encoded[2*i+1] = hex[b & 15];
        }
        encoded[2*n] = 0;
        rec_emit(GATE_NAME,"DATA","group=metadata name=%s part=%u parts=%u encoding=%s hex=%s",
                 name,part+1,parts,encoding,encoded);
    }
}
static int gate_digest(const char *path, char hex[65])
{
    const struct ciuki_file_ops *ops = proc_get_file_ops();
    struct ciuki_file file = {0};
    if (!ops || !ops->open) return -ENOENT;
    int err = ops->open(proc_supervisor()->cwd,path,&file);
    if (err) return err;
    struct sha256_ctx ctx; sha256_init(&ctx);
    static uint8_t bytes[512];
    uint8_t digest[32];
    for (uint32_t off = 0; !err && off < file.bytes;) {
        uint32_t n = file.bytes - off; if (n > sizeof(bytes)) n = sizeof(bytes);
        err = file.read(file.cookie,off,bytes,n);
        if (!err) sha256_update(&ctx,bytes,n);
        off += n;
    }
    if (file.close) file.close(file.cookie);
    if (!err) { sha256_final(&ctx,digest); sha256_hex(digest,hex); }
    return err;
}
static bool gate_path(const char *path)
{
    if (gate_compare(path,"/bin/lua") && gate_compare(path,"/system/tests/ciuki-f2.lua") &&
        strncmp(path,GATE_CWD "/",sizeof(GATE_CWD))) return false;
    if (strlen(path) > 80) return false; /* full SHA and path fit in 240 bytes */
    for (const char *p = path; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '/' || *p == '.' || *p == '_' || *p == '-')) return false;
    return true;
}
static bool gate_provenance_line(char *line, uint32_t next[6], uint32_t total[6], uint32_t *payloads)
{
    char *v[6]; unsigned count = gate_tokens(line,v,ARRAY_SIZE(v));
    if (count == 3 && !gate_compare(v[0],"P") && gate_path(v[1]) && gate_hex(v[2],64,64)) {
        char actual[65]; int err = gate_digest(v[1],actual);
        if (err) return false;
        rec_emit(GATE_NAME,"DATA","group=payload path=%s sha256=%s",v[1],actual);
        (*payloads)++;
        return !gate_compare(actual,v[2]);
    }
    if (count != 6 || gate_compare(v[0],"M") || !gate_hex(v[5],2,32)) return false;
    unsigned field;
    for (field = 0; field < ARRAY_SIZE(gate_fields); field++) if (!gate_compare(v[1],gate_fields[field])) break;
    uint32_t part, parts;
    if (field == ARRAY_SIZE(gate_fields) || !gate_number(v[2],&part) || !gate_number(v[3],&parts) ||
        !parts || parts > 256 || part != next[field] + 1 || part > parts ||
        (total[field] && total[field] != parts) || gate_compare(v[4],field == 2 || field == 5 ? "json" : "sha256")) return false;
    next[field] = part; total[field] = parts;
    rec_emit(GATE_NAME,"DATA","group=metadata name=%s part=%u parts=%u encoding=%s hex=%s",
             gate_fields[field],part,parts,v[4],v[5]);
    return true;
}
static bool gate_provenance(void)
{
    const struct ciuki_file_ops *ops = proc_get_file_ops();
    struct ciuki_file file = {0};
    int err = !ops || !ops->open ? -ENOENT : ops->open(proc_supervisor()->cwd,GATE_PROVENANCE,&file);
    if (err) {
        rec_emit(GATE_NAME,"DATA","case=provenance expected=0 observed=%d",err);
        return false;
    }
    bool ok = file.bytes && file.bytes <= 65536;
    uint32_t next[6] = {0}, total[6] = {0}, payloads = 0;
    static uint8_t bytes[512];
    char line[192]; unsigned used = 0;
    for (uint32_t off = 0; ok && off < file.bytes;) {
        unsigned n = file.bytes - off; if (n > sizeof(bytes)) n = sizeof(bytes);
        if (file.read(file.cookie,off,bytes,n)) { ok = false; break; }
        off += n;
        for (unsigned i = 0; ok && i < n; i++) {
            if (bytes[i] == '\n') {
                line[used] = 0; ok = gate_provenance_line(line,next,total,&payloads); used = 0;
            } else if (bytes[i] < 32 || bytes[i] > 126 || used == sizeof(line)-1) ok = false;
            else line[used++] = (char)bytes[i];
        }
    }
    if (file.close) file.close(file.cookie);
    for (unsigned i = 0; i < ARRAY_SIZE(next); i++) ok = ok && next[i] && next[i] == total[i];
    ok = ok && !used && payloads >= 3;
    rec_emit(GATE_NAME,"DATA","case=provenance payloads=%u expected=1 observed=%u",payloads,ok);
    gate_metadata("elf_hashes","json","{}"); /* actual hashes are group=payload */
    return ok;
}

struct gate_memory { uint32_t heap, stack, resident, committed, samples; };
static void gate_sample(struct process *app, struct gate_memory *peak)
{
    if (!app || !app->memory) return;
    struct uaddr *u = app->memory;
    uint32_t heap = u->brk - u->heap_base, stack = 0;
    for (struct ua_extent *e = u->head; e; e = e->next) if (e->kind == UA_STACK) stack += e->end - e->base;
    if (heap > peak->heap) peak->heap = heap;
    if (stack > peak->stack) peak->stack = stack;
    if (u->as.pages > peak->resident) peak->resident = u->as.pages;
    if (u->backing > peak->committed) peak->committed = u->backing;
    peak->samples++;
}
static void gate_memory_record(const char *run, uint32_t pid, const struct gate_memory *peak)
{
    rec_emit(GATE_NAME,"DATA","group=memory run_case=%s pid=%u heap_high_water=%u stack_high_water=%u measurement=sampled",
             run,pid,peak->heap,peak->stack);
    rec_emit(GATE_NAME,"DATA","group=memory run_case=%s pid=%u resident_high_water=%u committed_high_water=%u samples=%u",
             run,pid,peak->resident,peak->committed,peak->samples);
}

/* Supplement output is <1 KiB on success. Fail rather than infer missing
 * cases from an incomplete capture. Split writes and trailing data matter. */
static bool gate_capture_text(unsigned stream, char *text, unsigned capacity)
{
    const struct supervisor_capture *c = supervisor_captured(stream);
    if (!c || c->bytes >= capacity || c->bytes > c->head_bytes) return false;
    for (unsigned i = 0; i < c->head_bytes; i++) {
        uint8_t b = c->head[i];
        if ((b < 32 && b != '\n') || b > 126) return false;
        text[i] = (char)b;
    }
    text[c->head_bytes] = 0; return true;
}
static bool gate_supplement(uint32_t pid, int status)
{
    static char out[SUPERVISOR_CAPTURE_BYTES+1], err[SUPERVISOR_CAPTURE_BYTES+1];
    bool valid = gate_capture_text(1,out,sizeof(out)) && gate_capture_text(2,err,sizeof(err));
    static const char *const details[] = {
        "bytes=65536 rewritten=4096 fnv1a32=f6671c1c rename=1 remove=1",
        "cycles=100 live_strings=4096 string_bytes=128 reference_errors=0",
        "dates=2 epoch_2000=951782400 epoch_2040=2208988800 clock_nonnegative=1",
        "lines=4 ordered=1"
    };
    static const char console_out[] = "ciuki-f2 stdout 1\nciuki-f2 stdout 3\n";
    static const char console_err[] = "ciuki-f2 stderr 2\nciuki-f2 stderr 4\n";
    unsigned cases = 0;
    char *cursor = out;
    for (unsigned i = 0; i < ARRAY_SIZE(gate_cases); i++) {
        bool ok = valid;
        if (i == 3 && ok) {
            ok = !strncmp(cursor,console_out,sizeof(console_out)-1) && !gate_compare(err,console_err);
            if (ok) cursor += sizeof(console_out)-1;
        }
        char expected[192];
        ksnprintf(expected,sizeof(expected),"case=%s ok=1 %s\n",gate_cases[i],details[i]);
        ok = ok && !strncmp(cursor,expected,strlen(expected));
        if (ok) { cursor += strlen(expected); cases++; }
        rec_emit(GATE_NAME,"DATA","case=%s pid=%u expected=1 observed=%u exit=%d failures=%u",gate_cases[i],pid,ok,status,ok ? 0 : 1);
        if (ok) rec_emit(GATE_NAME,"DATA","case=%s pid=%u %s",gate_cases[i],pid,details[i]);
        valid = valid && ok;
    }
    valid = valid && !*cursor && cases == 4;
    rec_emit(GATE_NAME,"DATA","group=supplement pid=%u cases=%u final_ok=%u console_order=per_stream",pid,cases,valid);
    return valid;
}
static void gate_remove(struct process *owner, struct process *p)
{
    if (!p) return;
    uint32_t pid = p->pid;
    if (p->state != PROC_ZOMBIE) proc_stop(p,1,0);
    uint64_t deadline = g_ticks + 1000;
    while ((p = proc_find(pid)) && p->state != PROC_ZOMBIE && g_ticks < deadline) { proc_collect(); task_sleep_ms(1); }
    if (p && p->state == PROC_ZOMBIE) proc_reap(owner,p);
}
static bool gate_live(uint32_t pid) { struct process *p = proc_find(pid); return p && p->state == PROC_LIVE; }
static uint64_t gate_progress(struct process *desktop, bool native)
{
    if (native) { struct desktop_activity a; desktop_activity_snapshot(&a); return a.presents; }
    /* The production stand-in has no channel at ordinary boot; its tick
     * counter, rather than a fabricated reply count, establishes progress. */
    uint32_t ticks = 0;
    if (!desktop || desktop->state != PROC_LIVE || !desktop->memory ||
        ua_read(desktop->memory,&ticks,CIUKI_IMAGE_BASE+CIUKI_PAGE_SIZE+16,sizeof(ticks))) return 0;
    return ticks;
}
struct gate_outcome { int status; uint32_t assertions; };
struct gate_resources {
    uint32_t free_pages, kernel_bytes, descriptions, live_threads, retained_threads, waiters;
    struct file_ledger files;
};
static void gate_resources_snapshot(struct gate_resources *r)
{
    memset(r,0,sizeof(*r));
    r->free_pages = pmm_free_count(); r->kernel_bytes = (uint32_t)kheap_in_use();
    r->descriptions = file_description_count();
    struct px_node *cwd = proc_supervisor()->cwd;
    if (cwd) files_snapshot(cwd->space,&r->files);
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (!t) continue;
        r->live_threads += !t->stopped;
        r->retained_threads += t->retained;
        r->waiters += t->word_wait.queued || (t->task && t->task->state == T_BLOCKED && !t->task->wake_tick);
    }
}
static unsigned gate_ledger_format(char *out, unsigned capacity, const struct proc_ledger *p,
                                  const struct desktop_ledger *d, const struct gate_resources *r)
{
    /* Named array layouts are documented in the handoff. All counters are
     * actual snapshots; maximum-width values still fit the 1024-byte JSON. */
    return (unsigned)ksnprintf(out,capacity,
        "{\"proc\":[%u,%u,%u,%u,%u,%u,%u],\"desktop\":[%u,%u,%u,%u,%u,%u],"
        "\"free_pages\":%u,\"kernel_bytes\":%u,\"files\":[%u,%u,%u,%u],\"native\":[%u,%u,%u]}",
        p->processes,p->threads,p->zombies,p->handles,p->extents,p->backing,p->tables,
        d->descriptions,d->surfaces,d->pages,d->channels,d->messages,d->grants,
        r->free_pages,r->kernel_bytes,r->files.nodes,r->files.descriptions,r->files.pins,r->descriptions,
        r->live_threads,r->retained_threads,r->waiters);
}
static bool gate_run(struct process *owner, struct process *desktop, bool native, bool supplement,
                     uint64_t deadline, struct gate_memory *peak, struct gate_outcome *outcome)
{
    const char *run = supplement ? "lua-supplement" : "lua-basic";
    uint32_t desktop_pid = desktop->pid;
    uint64_t before = gate_progress(desktop,native), start = g_ticks;
    struct process *app = 0;
    int result = supervisor_spawn_gate(supplement,&app);
    if (result < 0) {
        rec_emit(GATE_NAME,"DATA","case=launch run_case=%s expected=1 observed=0 error=%d",run,result);
        return false;
    }
    /* No yield between publication, ownership and observation. PID 1 cannot
     * collect a fast child while setup sleeps: supervisor publishes last. */
    app->ppid = owner->pid;
    uint32_t pid = app->pid;
    supervisor_observe(GATE_NAME,pid);
    rec_emit(GATE_NAME,"DATA","case=launch run_case=%s pid=%u pgid=%u abi_version=%u",run,pid,app->pgid,CIUKI_ABI_VERSION);
    rec_emit(GATE_NAME,"DATA","group=setup run_case=%s cwd_text=%s argv_text=%s",run,GATE_CWD,
             supplement ? "lua,ciuki-f2.lua" : "lua,-e,_U:true,all.lua");
    gate_sample(app,peak);
    while (app->state != PROC_ZOMBIE && g_ticks < deadline && gate_live(desktop_pid)) {
        task_sleep_ms(1); gate_sample(app,peak);
    }
    bool exited = app->state == PROC_ZOMBIE, timeout = g_ticks >= deadline;
    int status = exited ? app->status : -1;
    const struct supervisor_capture *stdout = supervisor_captured(1), *stderr = supervisor_captured(2);
    uint32_t final = supplement ? gate_supplement(pid,status) : stdout->final_ok;
    uint32_t assertions = stdout->assertion_failures + stderr->assertion_failures;
    outcome->status = status; outcome->assertions = assertions;
    bool pass = exited && !timeout && status == 0 && final == 1 && !assertions && !app->fault_vector;
    rec_emit(GATE_NAME,"DATA","case=%s pid=%u exit=%d final_ok=%u assertion_failures=%u",run,pid,status,final,assertions);
    rec_emit(GATE_NAME,"DATA","group=wait run_case=%s pid=%u status=%d timeout=%u fault_vector=%u elapsed_ms=%llu",
             run,pid,status,timeout,app->fault_vector,g_ticks-start);
    gate_memory_record(run,pid,peak);
    supervisor_observe_end();
    gate_remove(owner,app);
    /* A short run must still demonstrate a fresh desktop repaint/tick. */
    uint64_t progress_deadline = g_ticks + 2000; if (progress_deadline > deadline) progress_deadline = deadline;
    while (gate_live(desktop_pid) && gate_progress(desktop,native) <= before && g_ticks < progress_deadline) task_sleep_ms(1);
    uint64_t after = gate_live(desktop_pid) ? gate_progress(desktop,native) : before;
    bool alive = gate_live(desktop_pid) && desktop->pid == desktop_pid;
    rec_emit(GATE_NAME,"DATA","group=survivor run_case=%s pid=%u alive=%u pid_unchanged=%u",run,desktop_pid,alive,alive);
    rec_emit(GATE_NAME,"DATA","group=survivor run_case=%s before=%llu after=%llu progress=%llu metric=%s",
             run,before,after,after >= before ? after-before : 0,native ? "presents" : "ticks");
    return pass && alive && after > before;
}

int probe_f2_app_gate(void)
{
    rec_emit(GATE_NAME,"BEGIN",0);
    uint64_t deadline = g_ticks + GATE_BUDGET_MS;
    rec_emit(GATE_NAME,"DATA","group=app-gate application=Lua version=5.4.8 abi_version=1 extra_skips=0");
    rec_emit(GATE_NAME,"DATA","group=exclusions switch=_U nonportable=excluded_by_contract large_memory=excluded_by_contract");
    rec_emit(GATE_NAME,"DATA","group=exclusions complete=excluded_by_contract internal=excluded_by_contract");
    int err = storage_enable_write(storage_get(),2);
    rec_emit(GATE_NAME,"DATA","case=write-gate expected=0 observed=%d",err);
    if (err) { rec_emit(GATE_NAME,"END","status=FAIL reason=write_gate"); return 1; }
    struct process *owner = 0, *desktop = 0;
    if (proc_prepare(proc_supervisor(),&owner)) { rec_emit(GATE_NAME,"END","status=FAIL reason=controller_memory"); return 1; }
    proc_publish(owner,0);
    const struct fb_device *lfb = fbdev_get();
    const struct ciuki_file_ops *ops = proc_get_file_ops(); struct ciuki_file file = {0};
    bool native = !(g_boot.flags & CBI_F_SAFE_MODE) && lfb->present && ops && ops->open &&
                  !ops->open(proc_supervisor()->cwd,"/bin/desktop",&file);
    if (native && file.close) file.close(file.cookie);
    const char *argv[] = {"desktop",0};
    err = native ? supervisor_spawn("/bin/desktop",argv,0,true,&desktop) : supervisor_spawn_standin(owner,&desktop);
    bool pass = err > 0;
    if (!pass) rec_emit(GATE_NAME,"DATA","case=desktop expected=1 observed=0 error=%d",err);
    if (pass) {
        desktop->ppid = owner->pid;
        rec_emit(GATE_NAME,"DATA","case=desktop server=%s pid=%u pgid=%u expected=1 observed=1",native ? "desktop" : "standin",desktop->pid,desktop->pgid);
        uint64_t ready_deadline = g_ticks + 10000;
        while (gate_live(desktop->pid) && !gate_progress(desktop,native) && g_ticks < ready_deadline) task_sleep_ms(1);
        pass = gate_live(desktop->pid) && gate_progress(desktop,native) && gate_provenance();
    }
    gate_metadata("argv","json","[\"lua\",\"-e\",\"_U=true\",\"all.lua\"]");
    gate_metadata("env","json","{\"LC_ALL\":\"C\",\"TZ\":\"UTC0\",\"HOME\":\"/home\",\"TMPDIR\":\"/tmp\",\"PATH\":\"/bin\"}");
    gate_metadata("cwd","utf8",GATE_CWD);
    gate_metadata("fd_setup","json","{\"inherited\":[0,1,2],\"stdin\":\"/dev/null\",\"stdout\":\"bounded\",\"stderr\":\"bounded\"}");
    struct proc_ledger baseline, final; struct desktop_ledger objects_before, objects_after;
    proc_snapshot(&baseline); desktop_snapshot(&objects_before);
    struct gate_resources resources_before, resources_after;
    gate_resources_snapshot(&resources_before);
    struct gate_memory basic = {0}, extra = {0};
    struct gate_outcome basic_result = {.status=-1}, extra_result = {.status=-1};
    bool upstream = false, supplement = false;
    if (pass) upstream = gate_run(owner,desktop,native,false,deadline,&basic,&basic_result);
    /* A failed upstream still gets independent supplement evidence unless
     * the deadline/desktop prevents it. It never substitutes for upstream. */
    if (pass && gate_live(desktop->pid) && g_ticks < deadline) supplement = gate_run(owner,desktop,native,true,deadline,&extra,&extra_result);
    proc_snapshot(&final); desktop_snapshot(&objects_after);
    gate_resources_snapshot(&resources_after);
    bool restored = !memcmp(&baseline,&final,sizeof(baseline)) && !memcmp(&objects_before,&objects_after,sizeof(objects_before)) &&
                    !memcmp(&resources_before,&resources_after,sizeof(resources_before));
    rec_emit(GATE_NAME,"DATA","group=resources processes_delta=%d zombies_delta=%d threads_delta=%d fds_delta=%d",
             (int)final.processes-(int)baseline.processes,(int)final.zombies-(int)baseline.zombies,
             (int)final.threads-(int)baseline.threads,(int)final.handles-(int)baseline.handles);
    rec_emit(GATE_NAME,"DATA","group=resources mappings_delta=%d backing_delta=%d restored=%u",
             (int)final.extents-(int)baseline.extents,(int)final.backing-(int)baseline.backing,
             restored);
    rec_emit(GATE_NAME,"DATA","group=resources descriptions_delta=%d surfaces_delta=%d messages_delta=%d",
             (int)objects_after.descriptions-(int)objects_before.descriptions,(int)objects_after.surfaces-(int)objects_before.surfaces,
             (int)objects_after.messages-(int)objects_before.messages);
    rec_emit(GATE_NAME,"DATA","group=resources pages_delta=%d kernel_bytes_delta=%d file_descriptions_delta=%d",
             (int)resources_before.free_pages-(int)resources_after.free_pages,
             (int)resources_after.kernel_bytes-(int)resources_before.kernel_bytes,
             (int)resources_after.descriptions-(int)resources_before.descriptions);
    rec_emit(GATE_NAME,"DATA","group=resources live_threads_delta=%d retained_threads_delta=%d waiters_delta=%d",
             (int)resources_after.live_threads-(int)resources_before.live_threads,
             (int)resources_after.retained_threads-(int)resources_before.retained_threads,
             (int)resources_after.waiters-(int)resources_before.waiters);
    rec_emit(GATE_NAME,"DATA","group=resources cache_nodes_before=%u cache_nodes_after=%u cache_accounted=%u",
             resources_before.files.nodes,resources_after.files.nodes,resources_before.files.nodes == resources_after.files.nodes);
    static char ledgers[1024];
    unsigned used = (unsigned)ksnprintf(ledgers,sizeof(ledgers),"{\"baseline\":");
    used += gate_ledger_format(ledgers+used,sizeof(ledgers)-used,&baseline,&objects_before,&resources_before);
    used += (unsigned)ksnprintf(ledgers+used,sizeof(ledgers)-used,",\"final\":");
    used += gate_ledger_format(ledgers+used,sizeof(ledgers)-used,&final,&objects_after,&resources_after);
    ksnprintf(ledgers+used,sizeof(ledgers)-used,",\"restored\":%u}",restored);
    gate_metadata("resource_ledgers","json",ledgers);
    rec_emit(GATE_NAME,"DATA","max_resident_pages=%u max_committed_pages=%u",basic.resident > extra.resident ? basic.resident : extra.resident,
             basic.committed > extra.committed ? basic.committed : extra.committed);
    rec_emit(GATE_NAME,"DATA","group=app-gate final_ok=%u assertion_failures=%u",upstream && supplement,
             basic_result.assertions+extra_result.assertions);
    if (basic_result.status >= 0 && extra_result.status >= 0)
        rec_emit(GATE_NAME,"DATA","group=app-gate application_wait_status=%d",basic_result.status ? basic_result.status : extra_result.status);
    pass = pass && upstream && supplement && restored;
    gate_remove(owner,desktop); proc_stop(owner,0,0); proc_collect();
    rec_emit(GATE_NAME,"END",pass ? "status=PASS" : "status=FAIL reason=application_contract");
    return pass ? 0 : 1;
}
CIUKI_F2_PROBE("app-gate", probe_f2_app_gate);
