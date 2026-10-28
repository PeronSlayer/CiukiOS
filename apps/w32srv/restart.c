/* f5-07B virtual restart, w32srv side. After the desktop has ended the Win32
 * programs it asks this service to (1) commit the hives, (2) process
 * C:\WINDOWS\WININIT.INI, (3) hand out the RunServicesOnce / RunOnce entries
 * one at a time, (4) close the restart. Every step is journalled in
 * C:\WINDOWS\CIUKIRST.LOG (written to CIUKIRST.TMP and renamed over it), so an
 * interrupted restart resumes where it stopped.
 * Semantics and sources:
 *  - WININIT.INI [rename]: "destination=source" renames/moves the source over
 *    the destination; "NUL=file" deletes the file; lines apply in file order,
 *    a later line may use the result of an earlier one. A missing source, a
 *    refused rename, a protected or non-C: path is recorded and the line
 *    skipped. When the pass ends the file becomes WININIT.BAK. The Windows 98
 *    Resource Kit (Appendix D, batch INF "OLS.Folder": wininit.ini,DIRNUL,,
 *    "%25%\Desktop\Online~1=1") also shows a [DIRNUL] section, applied here
 *    as removal of an empty directory.
 *  - RunServicesOnce, RunOnce (HKLM) and RunOnce (HKCU) values run once, in
 *    registry (creation) order; each value is deleted before its command is
 *    handed out, as Windows deletes them before running (Resource Kit
 *    chapter 27: the RunOnce WUCheckShortcut entry runs on the next startup
 *    and is gone; chapter 10: RunServicesOnce). Run and RunServices are never
 *    run. Command bytes are preserved; value-name prefixes never alter the command.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "shell_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char journal_name[] = "CIUKIRST.LOG", journal_tmp[] = "CIUKIRST.TMP";
static const char wininit_name[] = "WININIT.INI", wininit_backup[] = "WININIT.BAK";
static const char run_path[] = "Software\\Microsoft\\Windows\\CurrentVersion\\";
static const struct { unsigned hive; const char *prefix, *name; } queues[3] = {
    {0, "", "RunServicesOnce"}, {0, "", "RunOnce"}, {1, ".DEFAULT\\", "RunOnce"}};

static int ci_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int ci_prefix(const char *text, const char *prefix)
{
    for (; *prefix; text++, prefix++) {
        unsigned x = (unsigned char)*text, y = (unsigned char)*prefix;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}
static int in_windows(const struct ws_shell *s, const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", s->windows, name);
    return n > 0 && (size_t)n < cap;
}
unsigned ws_restart_phase(const struct ws_shell *s) { return s->phase; }

/* ---- journal ----------------------------------------------------------- */
static int journal_save(struct ws_shell *s)
{
    char text[160], path[WS_PATH], tmp[WS_PATH];
    int n = snprintf(text, sizeof(text), "CIUKIRST 1\nphase=%u\nflags=%x\nline=%u\nhanded=%u\n", s->phase, s->flags, s->line, s->handed);
    if (n <= 0 || (size_t)n >= sizeof(text) || !in_windows(s, journal_name, path, sizeof(path)) || !in_windows(s, journal_tmp, tmp, sizeof(tmp))) return EINVAL;
    int e = s->fs.write(s->fs.opaque, tmp, text, (size_t)n);
    if (!e) e = s->fs.rename(s->fs.opaque, tmp, path);
    if (e) ws_log(s, "[win32] restart journal=write result=error errno=%d", e);
    return e;
}
static int journal_remove(struct ws_shell *s)
{
    char path[WS_PATH];
    if(!in_windows(s,journal_name,path,sizeof(path)))return EINVAL;
    int e=s->fs.unlink(s->fs.opaque,path);if(e && e!=ENOENT)return e;
    if(in_windows(s,journal_tmp,path,sizeof(path)))s->fs.unlink(s->fs.opaque,path);
    return 0;
}
int ws_restart_recover(struct ws_shell *s)
{
    /* sec-k06: unowned v1 inputs never execute in a real user session.
     * Schema-2 work belongs to the retained REGISTRY broker protocol. */
    if(s->registry&&s->registry->session_only)return EOPNOTSUPP;
    char path[WS_PATH]; uint8_t *bytes = NULL; size_t n = 0;
    if (!in_windows(s, journal_tmp, path, sizeof(path))) return EINVAL;
    s->fs.unlink(s->fs.opaque, path); /* a torn write of the next journal */
    if (!in_windows(s, journal_name, path, sizeof(path))) return EINVAL;
    int e = s->fs.read(s->fs.opaque, path, &bytes, &n, 512);
    if (e == ENOENT) return 0;
    unsigned phase = 0, line = 0, handed = 0; unsigned flags = 0; int valid = 0;
    if (!e && n && n < 512) {
        char text[512]; memcpy(text, bytes, n); text[n] = 0;
        valid = sscanf(text, "CIUKIRST 1\nphase=%u\nflags=%x\nline=%u\nhanded=%u\n", &phase, &flags, &line, &handed) == 4 &&
                (phase == W32R_RESTART_WININIT || phase == W32R_RESTART_RUNONCE) && line <= WS_WININIT_LINES && handed <= WS_RUNS_MAX;
    }
    free(bytes);
    if (!valid) { ws_log(s, "[win32] restart journal=read result=damaged errno=%d action=restart-from-wininit", e); phase = W32R_RESTART_WININIT; line = handed = 0; flags = 0; }
    s->phase = phase; s->line = line; s->handed = handed; s->flags = flags; s->resumed = 1;
    ws_log(s, "[win32] restart resume phase=%u line=%u handed=%u", s->phase, s->line, s->handed);
    return 0;
}

/* ---- WININIT.INI ------------------------------------------------------- */
static void wininit_free(struct ws_shell *s)
{
    free(s->wininit); s->wininit = NULL; s->wininit_size = s->cursor = 0; s->cursor_line = s->section = 0; s->wininit_loaded = 0;
}
static int wininit_load(struct ws_shell *s)
{
    char path[WS_PATH];
    wininit_free(s); s->wininit_loaded = 1;
    if (!in_windows(s, wininit_name, path, sizeof(path))) return EINVAL;
    int e = s->fs.read(s->fs.opaque, path, &s->wininit, &s->wininit_size, WS_WININIT_LIMIT);
    if (e) { free(s->wininit); s->wininit = NULL; s->wininit_size = 0; }
    return e;
}
static int wininit_close(struct ws_shell *s, int had_file)
{
    char from[WS_PATH], to[WS_PATH];
    if (had_file && in_windows(s, wininit_name, from, sizeof(from)) && in_windows(s, wininit_backup, to, sizeof(to))) {
        int e = s->fs.rename(s->fs.opaque, from, to);
        if (e) { ws_log(s, "[win32] wininit result=rename-bak errno=%d", e); return e; }
    }
    wininit_free(s);
    s->phase = W32R_RESTART_RUNONCE;
    int e=journal_save(s);if(e)s->phase=W32R_RESTART_WININIT;
    return e;
}
static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return text;
}
/* The native path of one WININIT.INI operand, or an error: the C: drive
 * only, no "." or ".." component, nothing under C:\System (read-only to
 * Win32 programs), and not the service's own files. */
static uint32_t operand(struct ws_shell *s, const char *text, char *out, size_t cap)
{
    if (strlen(text) >= 1000 || strchr(text, '"') || strpbrk(text, "*?<>|")) return 123; /* ERROR_INVALID_NAME */
    uint32_t e = (uint32_t)ws_native(s, text, out, cap);
    if (e) return e;
    if (out[0] != '/') return 123;
    for (const char *p = out; *p;) {
        while (*p == '/') p++;
        const char *q = p; while (*q && *q != '/') q++;
        size_t n = (size_t)(q - p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 123;
        p = q;
    }
    if (ci_prefix(out, "/System") && (out[7] == 0 || out[7] == '/')) return 5;
    size_t w = strlen(s->windows);
    if (ci_prefix(out, s->windows) && out[w] == '/' && !strchr(out + w + 1, '/')) {
        static const char *const protected_names[] = {"SYSTEM.CR", "USER.CR", "SERVICE.CR", "CIUKIRST", "WININIT."};
        for (unsigned i = 0; i < sizeof(protected_names) / sizeof(*protected_names); i++)
            if (ci_prefix(out + w + 1, protected_names[i])) return 5;
    }
    return 0;
}
static void apply_line(struct ws_shell *s, unsigned number, unsigned section, char *left, char *right)
{
    char to[WS_PATH], from[WS_PATH]; int dir = 0; uint64_t size = 0;
    if (section == 2) { /* [DIRNUL]: "path=flag" removes an empty directory */
        uint32_t e = operand(s, left, from, sizeof(from));
        if (e) { ws_log(s, "[win32] wininit line=%u op=rmdir result=refused error=%u", number, e); return; }
        int r = s->fs.rmdir(s->fs.opaque, from);
        if (r) ws_log(s, "[win32] wininit line=%u op=rmdir result=error errno=%d", number, r);
        else ws_log(s, "[win32] wininit line=%u op=rmdir", number);
        return;
    }
    if (ci_equal(left, "NUL")) {
        uint32_t e = operand(s, right, from, sizeof(from));
        if (e) { ws_log(s, "[win32] wininit line=%u op=delete result=refused error=%u", number, e); return; }
        if (s->fs.stat(s->fs.opaque, from, &dir, &size)) { ws_log(s, "[win32] wininit line=%u op=delete result=source-missing", number); return; }
        int r = dir ? EISDIR : s->fs.unlink(s->fs.opaque, from);
        if (r) ws_log(s, "[win32] wininit line=%u op=delete result=error errno=%d", number, r);
        else ws_log(s, "[win32] wininit line=%u op=delete", number);
        return;
    }
    uint32_t e = operand(s, left, to, sizeof(to));
    if (!e) e = operand(s, right, from, sizeof(from));
    if (e) { ws_log(s, "[win32] wininit line=%u op=rename result=refused error=%u", number, e); return; }
    if (s->fs.stat(s->fs.opaque, from, &dir, &size)) { ws_log(s, "[win32] wininit line=%u op=rename result=source-missing", number); return; }
    int r = dir ? EISDIR : s->fs.rename(s->fs.opaque, from, to);
    if (r) ws_log(s, "[win32] wininit line=%u op=rename result=error errno=%d", number, r);
    else ws_log(s, "[win32] wininit line=%u op=rename", number);
}
/* Advances to and applies the next applicable line; 0 when the file ends. */
static int wininit_step(struct ws_shell *s)
{
    while (s->cursor < s->wininit_size) {
        size_t start = s->cursor, end = start;
        while (end < s->wininit_size && s->wininit[end] != '\n') end++;
        s->cursor = end < s->wininit_size ? end + 1 : end;
        size_t n = end - start;
        char line[1100];
        if (n >= sizeof(line)) { if (s->section) { s->cursor_line++; if (s->cursor_line > s->line) { s->line = s->cursor_line; ws_log(s, "[win32] wininit line=%u result=too-long", s->line); return 1; } } continue; }
        memcpy(line, s->wininit + start, n); line[n] = 0;
        if (memchr(line, 0, n)) continue;
        char *text = trim(line);
        if (!*text || *text == ';') continue;
        if (*text == '[') {
            char *close = strchr(text, ']');
            if (close) { *close = 0; char *name = trim(text + 1); s->section = ci_equal(name, "rename") ? 1 : ci_equal(name, "DIRNUL") ? 2 : 0; }
            continue;
        }
        if (!s->section) continue;
        char *equals = strchr(text, '=');
        if (!equals) continue;
        if (++s->cursor_line > WS_WININIT_LINES) { s->cursor = s->wininit_size; ws_log(s, "[win32] wininit result=line-limit"); return 0; }
        if (s->cursor_line <= s->line) continue; /* applied before the interruption */
        *equals = 0;
        char *left = trim(text), *right = trim(equals + 1);
        s->line = s->cursor_line;
        if (!*left || (s->section == 1 && !*right)) { ws_log(s, "[win32] wininit line=%u result=malformed", s->line); return 1; }
        apply_line(s, s->line, s->section, left, right);
        return 1;
    }
    return 0;
}
int ws_restart_tick(struct ws_shell *s)
{
    /* sec-k06: unowned v1 inputs never execute in a real user session.
     * Schema-2 work belongs to the retained REGISTRY broker protocol. */
    if(s->registry&&s->registry->session_only)return 0;
    if (s->phase != W32R_RESTART_WININIT || s->restart_error) return 0;
    if (!s->wininit_loaded) {
        int e = wininit_load(s);
        if (e && e != ENOENT) {
            ws_log(s, "[win32] wininit result=read-error errno=%d", e);
            s->restart_error=wininit_close(s, e == EFBIG); /* a file over the limit is retired, an I/O error retried next time */
            return 0;
        }
        if (e == ENOENT) { s->restart_error=wininit_close(s, 0); return 0; }
        ws_log(s, "[win32] wininit bytes=%u", (unsigned)s->wininit_size);
    }
    int more = wininit_step(s);
    if (more) {
        if (journal_save(s)) { /* cannot journal: stop the pass, the next start retries */
            s->restart_error=EIO;return 0;
        }
        return 1;
    }
    s->restart_error=wininit_close(s, 1);
    return 0;
}

/* ---- RunOnce queue ------------------------------------------------------ */
static struct wr_key *queue_key(struct ws_shell *s, unsigned q)
{
    char path[160];
    snprintf(path, sizeof(path), "%s%s%s", queues[q].prefix, run_path, queues[q].name);
    return s->registry ? wr_walk(s->registry, queues[q].hive, path) : NULL;
}
/* Canonical UTF-16LE to UTF-8; 0 on malformed input or overflow. */
static int utf8_from_wide(const uint8_t *bytes, uint32_t count, char *out, size_t cap)
{
    size_t at = 0;
    if (count % 2) return 0;
    for (uint32_t i = 0; i + 1 < count; i += 2) {
        uint32_t c = bytes[i] | (uint32_t)bytes[i + 1] << 8;
        if (!c) break;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 3 >= count) return 0;
            uint32_t low = bytes[i + 2] | (uint32_t)bytes[i + 3] << 8;
            if (low < 0xdc00 || low > 0xdfff) return 0;
            c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00); i += 2;
        } else if (c >= 0xdc00 && c <= 0xdfff) return 0;
        unsigned n = c < 128 ? 1 : c < 2048 ? 2 : c < 65536 ? 3 : 4;
        if (at + n + 1 > cap) return 0;
        if (n == 1) out[at++] = (char)c;
        else {
            out[at++] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | (c >> (6 * (n - 1))));
            for (unsigned j = n - 1; j; j--) out[at++] = (char)(0x80 | ((c >> (6 * (j - 1))) & 63));
        }
    }
    out[at] = 0;
    return 1;
}
/* Single-pass %NAME% expansion for REG_EXPAND_SZ (unknown names stay as written). */
static int expand(const char *in, char *out, size_t cap)
{
    static const char *const names[] = {"SystemRoot", "windir", "SystemDrive", "ProgramFiles", "CommonProgramFiles", "TEMP", "TMP"};
    static const char *const values[] = {"C:\\WINDOWS", "C:\\WINDOWS", "C:", "C:\\Program Files", "C:\\Program Files\\Common Files", "C:\\WINDOWS\\TEMP", "C:\\WINDOWS\\TEMP"};
    size_t at = 0;
    while (*in) {
        const char *end = *in == '%' ? strchr(in + 1, '%') : NULL, *value = NULL; size_t n = 1;
        if (end && end > in + 1) {
            for (unsigned k = 0; k < sizeof(names) / sizeof(*names); k++)
                if (strlen(names[k]) == (size_t)(end - in - 1) && ci_prefix(in + 1, names[k])) value = values[k];
        }
        const char *chunk = in;
        if (value) { chunk = value; n = strlen(value); in = end + 1; }
        else in++;
        if (at + n + 1 > cap) return 0;
        memcpy(out + at, chunk, n); at += n;
    }
    out[at] = 0;
    return 1;
}
uint32_t ws_restart_next(struct ws_shell *s, char *command, size_t cap, uint32_t *type, uint32_t *handed)
{
    /* sec-k06: unowned v1 inputs never execute in a real user session.
     * Schema-2 work belongs to the retained REGISTRY broker protocol. */
    if(s->registry&&s->registry->session_only)return 120;
    if (s->restart_error) return 29;
    if (s->phase == W32R_RESTART_IDLE) return 87;
    if (s->phase == W32R_RESTART_WININIT) return 170; /* ERROR_BUSY: the WININIT pass is still running */
    if (cap < 2048) return 87;
    if(s->pending_run) {
        uint32_t e=wr_flush(s->registry);if(e)return e;
        if(journal_save(s))return 29;
        if(s->pending_command[0]) {
            memcpy(command,s->pending_command,strlen(s->pending_command)+1);
            *type=s->pending_type;*handed=s->handed;s->pending_run=0;return 0;
        }
        s->pending_run=0;
    }
    for (unsigned q = 0; q < 3; q++) for (;;) {
        struct wr_key *key = queue_key(s, q);
        struct wr_value *v = key ? key->values : NULL;
        if (!v) break;
        if (s->handed >= WS_RUNS_MAX) { ws_log(s, "[win32] restart runonce result=limit handed=%u", s->handed); return 8; }
        char name[W32R_NAME], expanded[2048];char *text=s->pending_command;text[0]=0;int ok=0;
        uint32_t vtype = v->type;
        snprintf(name, sizeof(name), "%s", v->name);
        if ((vtype == 1 || vtype == 2) && utf8_from_wide(v->data,v->bytes,text,sizeof(s->pending_command))) {
            ok = 1;
            if (vtype == 2) ok = expand(text, expanded, sizeof(expanded)) ? (memcpy(text, expanded, strlen(expanded) + 1), 1) : 0;
        }
        /* Deleted before it runs, and durably: a crash must not run it twice. */
        uint32_t d = wr_remove_value(s->registry, key, name);
        if (d) { ws_log(s, "[win32] restart runonce queue=%u result=delete-error error=%u", q, d); return d; }
        s->handed++;s->pending_run=1;s->pending_type=q;
        if(!ok)s->pending_command[0]=0;
        uint32_t commit=wr_flush(s->registry);
        if(commit)return commit;
        if(journal_save(s))return 29;
        s->pending_run=0;
        char *p=text;
        if (!ok || !*p) { ws_log(s, "[win32] restart runonce queue=%u result=skipped", q); continue; }
        size_t n = strlen(p);
        memmove(command, p, n + 1);
        *type = q; *handed = s->handed;
        ws_log(s, "[win32] restart runonce queue=%u handed=%u", q, s->handed);
        return 0;
    }
    s->exhausted=1;return 259; /* ERROR_NO_MORE_ITEMS */
}
int ws_restart_pending(struct ws_shell *s)
{
    /* sec-k06: unowned v1 inputs never execute in a real user session.
     * Schema-2 work belongs to the retained REGISTRY broker protocol. */
    if(s->registry&&s->registry->session_only)return 0;
    char path[WS_PATH];
    if (s->phase != W32R_RESTART_IDLE || s->resumed) return 1;
    int dir = 0; uint64_t size = 0;
    if (in_windows(s, wininit_name, path, sizeof(path)) && !s->fs.stat(s->fs.opaque, path, &dir, &size) && !dir) return 1;
    for (unsigned q = 0; q < 3; q++) { struct wr_key *k = queue_key(s, q); if (k && k->values) return 1; }
    return 0;
}

/* ---- begin and end ------------------------------------------------------ */
uint32_t ws_restart_begin(struct ws_shell *s, uint32_t flags)
{
    /* sec-k06: unowned v1 inputs never execute in a real user session.
     * Schema-2 work belongs to the retained REGISTRY broker protocol. */
    if(s->registry&&s->registry->session_only)return 120;
    if(s->restart_error){wininit_free(s);s->restart_error=0;ws_restart_recover(s);}
    if (s->phase != W32R_RESTART_IDLE) return 0; /* running or resumed: repeating is accepted */
    uint32_t e = s->registry ? wr_flush(s->registry) : 0;
    if (e) { ws_log(s, "[win32] restart begin result=commit-failed error=%u", e); return e; }
    s->phase = W32R_RESTART_WININIT; s->flags = flags; s->line = s->handed = 0; s->resumed = 0;s->exhausted=0;
    wininit_free(s);
    if (journal_save(s)) { s->phase = W32R_RESTART_IDLE; return 29; } /* ERROR_WRITE_FAULT */
    ws_log(s, "[win32] restart begin flags=%x", flags);
    return 0;
}
uint32_t ws_restart_end(struct ws_shell *s)
{
    /* sec-k06: unowned v1 inputs never execute in a real user session.
     * Schema-2 work belongs to the retained REGISTRY broker protocol. */
    if(s->registry&&s->registry->session_only)return 120;
    if (s->phase == W32R_RESTART_IDLE) return 0;
    if (s->phase == W32R_RESTART_WININIT || !s->exhausted || s->pending_run) return 170;
    uint32_t e = s->registry ? wr_flush(s->registry) : 0;
    if (e) { ws_log(s, "[win32] restart end result=commit-failed error=%u", e); return e; }
    if(journal_remove(s))return 29;
    ws_log(s, "[win32] restart complete handed=%u", s->handed);
    s->phase = W32R_RESTART_IDLE; s->line = s->handed = 0; s->resumed = 0; s->flags = 0;
    return 0;
}
