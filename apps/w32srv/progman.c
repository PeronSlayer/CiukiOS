/* f5-07B PROGMAN DDE server effects: the Program Manager DDE command set
 * (CreateGroup, ShowGroup, AddItem, DeleteItem, ReplaceItem, DeleteGroup,
 * Reload, ExitProgman) over the Start-menu Programs folder, the Groups and
 * group item requests, and the CiukiOS .lnk v3 writer.
 * Grammar and answers: Microsoft Platform SDK "Command-String Interface",
 * Microsoft KB Q138800 (commands), Q102575 (Groups/group requests), and
 * "Using Dynamic Data Exchange" (message, data and ACK ownership). See
 * docs/validation/f5-07b-shell.md for the inspected sources and limits.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../desktop/shell_link.h"
#include "shell_internal.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int utf8_valid(const uint8_t *s, unsigned n) { return desk_utf8_valid(s, n); }
unsigned ws_link_encode(const struct ws_link *r, uint8_t *out, unsigned cap)
{
    if (!r) return 0;
    struct desk_launch_record record = {0};
    const char *in[] = {r->label,r->target,r->arguments,r->directory,r->icon};
    char *fields[] = {record.label,record.target,record.arguments,record.directory,record.icon};
    const size_t sizes[] = {64,260,1024,260,260};
    for (unsigned i=0;i<5;i++) {
        if (!in[i] || strlen(in[i])>=sizes[i]) return 0;
        memcpy(fields[i],in[i],strlen(in[i])+1);
    }
    record.show=r->show;record.icon_index=r->icon_index;
    return desk_link_encode(&record,out,cap);
}
int ws_link_decode(const uint8_t *bytes, unsigned count, struct ws_link_record *out)
{
    struct desk_launch_record record;
    if (!out || !desk_link_decode(bytes,count,&record)) return 0;
    memcpy(out->label,record.label,64);memcpy(out->target,record.target,260);
    memcpy(out->arguments,record.arguments,1024);memcpy(out->directory,record.directory,260);
    memcpy(out->icon,record.icon,260);out->show=record.show;out->icon_index=record.icon_index;
    return 1;
}
/* ---- command grammar ------------------------------------------------- */
static const char *spaces(const char *p) { while (*p == ' ') p++; return p; }
int ws_parse_next(const char **cursor, struct ws_command *out)
{
    const char *p = spaces(*cursor);
    memset(out, 0, sizeof(*out));
    if (*p != '[') { *cursor = p; return 0; }
    p = spaces(p + 1);
    unsigned n = 0;
    while (*p && !strchr(" ,()[]\"", *p)) { if (n + 1 >= sizeof(out->opcode)) return -1; out->opcode[n++] = *p++; }
    if (!*p) return -1;
    p = spaces(p);
    if (*p == '(') {
        p++;
        while (*p != ')') {
            if (out->argc == WS_ARGS) return -1;
            p = spaces(p);
            char *arg = out->args[out->argc]; unsigned k = 0;
            if (*p == '"') {
                p++;
                while (*p != '"') { if (!*p || k + 1 >= WS_ARG) return -1; arg[k++] = *p++; }
                p++;
            } else {
                while (*p && !strchr(",()[]", *p)) { if (k + 1 >= WS_ARG) return -1; arg[k++] = *p++; }
                if (!*p) return -1;
                while (k && arg[k - 1] == ' ') k--;
            }
            arg[k] = 0; out->argc++;
            p = spaces(p);
            if (*p == ',') p++;
            else if (*p != ')') return -1;
        }
        p = spaces(p + 1);
    }
    if (*p != ']') return -1;
    *cursor = p + 1;
    return 1;
}
/* ---- names and paths -------------------------------------------------- */
static int ascii_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
/* A group or item file name: FAT-invalid characters become '_', trailing
 * dots and spaces are dropped (Windows 9x shell behaviour), 63 bytes. */
static int sanitize(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned c = *p;
        if (n + 1 >= cap || n >= 63) return 0;
        out[n++] = c < 32 || c == 127 || strchr("\\/:*?\"<>|", (int)c) ? '_' : (char)c;
    }
    while (n && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return n && strcmp(out, ".") && strcmp(out, "..") && utf8_valid((const uint8_t *)out, (unsigned)n);
}
void ws_log(struct ws_shell *s, const char *fmt, ...)
{
    char line[240]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    if (s->fs.log) s->fs.log(s->fs.opaque, line);
}
/* "C:\X" -> "/X"; "X" (relative) -> "<windows>/X"; other drives refused. */
int ws_native(const struct ws_shell *s, const char *path, char *out, size_t cap)
{
    const char *rest = path; int n;
    if (path[0] && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        if (path[0] != 'C' && path[0] != 'c') return 15; /* ERROR_INVALID_DRIVE */
        n = snprintf(out, cap, "%s", path + 2); rest = NULL;
    } else if (path[0] == '\\' || path[0] == '/') { n = snprintf(out, cap, "%s", path); rest = NULL; }
    else n = snprintf(out, cap, "%s/%s", s->windows, path);
    (void)rest;
    if (n < 0 || (size_t)n >= cap) return 206; /* ERROR_FILENAME_EXCED_RANGE */
    char *write=out;
    for(char *p=out;*p;p++){char c=*p=='\\'?'/':*p;if(c=='/' && write>out && write[-1]=='/')continue;*write++=c;}
    *write=0;
    return 0;
}
static int path_join(char *out, size_t cap, const char *a, const char *b, const char *suffix)
{
    int n = snprintf(out, cap, "%s/%s%s", a, b, suffix ? suffix : "");
    return n > 0 && (size_t)n < cap;
}
static int exists(struct ws_shell *s, const char *path, int *dir)
{
    uint64_t size; int is_dir = 0;
    int e = s->fs.stat(s->fs.opaque, path, &is_dir, &size);
    if (dir) *dir = is_dir;
    return !e;
}
/* ---- sessions ----------------------------------------------------------- */
static char *session_group(struct ws_shell *s, uint32_t pid, uint32_t id, int create)
{
    for (unsigned i = 0; i < WS_SESSIONS; i++) if (s->sessions[i].pid == pid && s->sessions[i].id == id) { s->sessions[i].used=s->registry->now; return s->active_group; }
    if (!create) return NULL;
    for (unsigned i = 0; i < WS_SESSIONS; i++) if (!s->sessions[i].pid) { s->sessions[i].pid = pid; s->sessions[i].id=id; s->sessions[i].used=s->registry->now; return s->active_group; }
    return NULL;
}
void ws_exit(void *svc, uint32_t pid)
{
    struct ws_shell *s = svc;
    for (unsigned i = 0; i < WS_SESSIONS; i++) if (s->sessions[i].pid == pid) memset(&s->sessions[i], 0, sizeof(s->sessions[i]));
}
/* ---- group folders ------------------------------------------------------ */
#define WS_FILE_NAME 68u
struct listing { char (*names)[WS_FILE_NAME]; unsigned count, cap, dirs_only, truncated; };
static int collect(void *ctx, const char *name, int is_dir)
{
    struct listing *l = ctx;
    if (!strcmp(name,".") || !strcmp(name,"..") || (l->dirs_only && !is_dir) || strlen(name) >= (l->dirs_only?64:WS_FILE_NAME)) return 0;
    if (l->count == l->cap) { l->truncated = 1; return 1; }
    memcpy(l->names[l->count++], name, strlen(name) + 1);
    return 0;
}
static void sort_names(struct listing *l)
{
    for (unsigned i = 1; i < l->count; i++) for (unsigned j = i; j && strcmp(l->names[j - 1], l->names[j]) > 0; j--) {
        char t[WS_FILE_NAME]; memcpy(t,l->names[j-1],WS_FILE_NAME);memcpy(l->names[j-1],l->names[j],WS_FILE_NAME);memcpy(l->names[j],t,WS_FILE_NAME);
    }
}
static int remove_tree(struct ws_shell *s, const char *path, unsigned depth)
{
    char (*names)[WS_FILE_NAME] = calloc(WS_ITEMS + WS_GROUPS, WS_FILE_NAME); if (!names) return ENOMEM;
    struct listing l = {names, 0, WS_ITEMS + WS_GROUPS, 0, 0};
    int e = s->fs.list(s->fs.opaque, path, collect, &l);
    if (!e && l.truncated) e = EFBIG;
    for (unsigned i = 0; !e && i < l.count; i++) {
        char child[WS_PATH]; if (!path_join(child, sizeof(child), path, l.names[i], NULL)) { e = ENAMETOOLONG; break; }
        int dir = 0; exists(s, child, &dir);
        if (dir) e = depth ? remove_tree(s, child, depth - 1) : ENOTEMPTY;
        else e = s->fs.unlink(s->fs.opaque, child);
    }
    free(names);
    return e ? e : s->fs.rmdir(s->fs.opaque, path);
}
static int group_path(struct ws_shell *s, const char *name, char *out, size_t cap)
{
    char clean[64];
    return sanitize(name, clean, sizeof(clean)) && path_join(out, cap, s->programs, clean, NULL);
}
/* ---- AddItem target resolution ------------------------------------------ */
static int has_extension(const char *name)
{
    const char *slash = strrchr(name, '\\'), *slash2 = strrchr(name, '/');
    const char *base = slash; if (slash2 && (!base || slash2 > base)) base = slash2; base = base ? base + 1 : name;
    return strchr(base, '.') != NULL;
}
/* An existing file (not a directory) named exe: a drive or root path as
 * given, else searched in the item's working directory, C:\WINDOWS,
 * C:\WINDOWS\SYSTEM and C:\WINDOWS\COMMAND; ".exe" is appended when the name
 * has no extension, as SearchPath(NULL, name, ".exe", ...) does. target
 * receives the Windows path. */
static int resolve_target(struct ws_shell *s, const char *exe, const char *defdir, char *target, size_t cap)
{
    const char *suffixes[] = {"", ".exe"};
    unsigned variants = has_extension(exe) ? 1 : 2;
    char native[WS_PATH]; int dir;
    if (!*exe || strpbrk(exe, "\t\"*?<>|")) return 0;
    if ((exe[0] && exe[1] == ':') || exe[0] == '\\' || exe[0] == '/') {
        const char *drive = exe[1] == ':' ? "" : "C:";
        for (unsigned v = 0; v < variants; v++) {
            if (snprintf(target, cap, "%s%s%s", drive, exe, suffixes[v]) >= (int)cap) return 0;
            if (ws_native(s, target, native, sizeof(native))) return 0;
            dir = 0;
            if (exists(s, native, &dir) && !dir) return 1;
        }
        return 0;
    }
    const char *dirs[] = {defdir && *defdir ? defdir : NULL, "C:\\WINDOWS", "C:\\WINDOWS\\SYSTEM", "C:\\WINDOWS\\COMMAND"};
    for (unsigned d = 0; d < 4; d++) {
        if (!dirs[d]) continue;
        for (unsigned v = 0; v < variants; v++) {
            if (snprintf(target, cap, "%s\\%s%s", dirs[d], exe, suffixes[v]) >= (int)cap) continue;
            if (ws_native(s, target, native, sizeof(native))) continue;
            dir = 0;
            if (exists(s, native, &dir) && !dir) return 1;
        }
    }
    return 0;
}
/* AddItem's first parameter is a program and its arguments in one string
 * with no delimiter. As on Windows (Wine's tests of Program Manager DDE), a
 * quoted program ends at its closing quote; otherwise the whole string is
 * tried first and trailing words are dropped until a program exists, the
 * dropped words becoming the arguments. */
static int find_program(struct ws_shell *s, const char *cmd, const char *defdir, char *target, size_t cap, const char **arguments)
{
    char buf[WS_ARG];
    const char *p = spaces(cmd);
    if (*p == '"') {
        const char *q = strchr(p + 1, '"');
        size_t n = q ? (size_t)(q - (p + 1)) : 0;
        if (!n || n >= sizeof(buf)) return 0;
        memcpy(buf, p + 1, n); buf[n] = 0;
        if (!resolve_target(s, buf, defdir, target, cap)) return 0;
        *arguments = spaces(q + 1);
        return 1;
    }
    size_t len = strlen(p);
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, p, len + 1);
    for (;;) {
        size_t n = strlen(buf);
        while (n && buf[n - 1] == ' ') buf[--n] = 0;
        if (!n) return 0;
        if (resolve_target(s, buf, defdir, target, cap)) { *arguments = spaces(p + n); return 1; }
        char *space = strrchr(buf, ' ');
        if (!space) return 0;
        *space = 0;
    }
}
static int number(const char *s, long *out)
{
    char *end; if (!*s) return 0; long v = strtol(s, &end, 10); if (*end) return 0; *out = v; return 1;
}
/* ---- the commands ------------------------------------------------------- */
static int count_names(struct ws_shell *s, const char *dir, unsigned dirs_only, unsigned cap)
{
    char (*names)[WS_FILE_NAME] = calloc(cap + 1, WS_FILE_NAME); if (!names) return -1;
    struct listing l = {names, 0, cap, dirs_only, 0}; s->fs.list(s->fs.opaque, dir, collect, &l);
    int n = (int)(l.count + l.truncated); free(names); return n;
}
/* The writer seam of f5-15A: stores one shortcut record at a native path. */
int ws_link_store(struct ws_shell *s, const char *path, const struct ws_link *record)
{
    uint8_t *bytes = malloc(WS_LINK_BYTES); if (!bytes) return ENOMEM;
    unsigned n = ws_link_encode(record, bytes, WS_LINK_BYTES);
    int e = n ? s->fs.write(s->fs.opaque, path, bytes, n) : EINVAL;
    free(bytes); return e;
}
static uint32_t add_item(struct ws_shell *s, const struct ws_command *c, const char *group)
{
    /* AddItem(CmdLine[,Name[,IconPath[,IconIndex[,xPos,yPos[,DefDir[,HotKey[,fMinimize[,fSeparateMemSpace]]]]]]]]) */
    if (c->argc < 1 || !*group) return WS_DDE_FNOTPROCESSED;
    char target[260]; const char *arguments;
    if (!find_program(s, c->args[0], c->argc > 6 ? c->args[6] : NULL, target, sizeof(target), &arguments)) {
        ws_log(s, "[win32] progman op=AddItem group=%s result=target-missing", group); return WS_DDE_FNOTPROCESSED;
    }
    char item[64];
    int named = c->argc > 1 && c->args[1][0];
    if (named) { if (!sanitize(c->args[1], item, sizeof(item))) return WS_DDE_FNOTPROCESSED; }
    else {
        const char *base = strrchr(target, '\\'); base = base ? base + 1 : target;
        char stem[64]; snprintf(stem, sizeof(stem), "%s", base);
        char *dot = strrchr(stem, '.'); if (dot && dot != stem) *dot = 0;
        if (!sanitize(stem, item, sizeof(item))) return WS_DDE_FNOTPROCESSED;
    }
    long index = 0, minimize = 0;
    if (c->argc > 3 && c->args[3][0] && !number(c->args[3], &index)) return WS_DDE_FNOTPROCESSED;
    if (c->argc > 8 && c->args[8][0] && !number(c->args[8], &minimize)) return WS_DDE_FNOTPROCESSED;
    struct ws_link link = {.label = named ? c->args[1] : item, .target = target, .arguments = arguments,
        .directory = c->argc > 6 ? c->args[6] : "", .icon = c->argc > 2 ? c->args[2] : "", .show = minimize ? 7 : 1, .icon_index = (int32_t)index};
    if (strlen(link.label) >= 64 || strlen(link.directory) >= 260 || strlen(link.icon) >= 260 || strlen(arguments) >= 1024) return WS_DDE_FNOTPROCESSED;
    char dir[WS_PATH], path[WS_PATH];
    if (!path_join(dir, sizeof(dir), s->programs, group, NULL) || !path_join(path, sizeof(path), dir, item, ".lnk")) return WS_DDE_FNOTPROCESSED;
    int e = 0;
    if (!exists(s, path, NULL)) { int n = count_names(s, dir, 0, WS_ITEMS); if (n < 0) e = ENOMEM; else if ((unsigned)n >= WS_ITEMS) e = EFBIG; }
    if (!e) e = ws_link_store(s, path, &link);
    if (e) { ws_log(s, "[win32] progman op=AddItem group=%s item=%s result=error errno=%d", group, item, e); return WS_DDE_FNOTPROCESSED; }
    ws_log(s, "[win32] progman op=AddItem group=%s item=%s target=%s", group, item, target);
    return WS_DDE_FACK;
}
static uint32_t execute(struct ws_shell *s, char *group, const struct ws_command *c)
{
    if (!group) { ws_log(s, "[win32] progman op=%s result=sessions-exhausted", c->opcode); return WS_DDE_FNOTPROCESSED; }
    char path[WS_PATH];
    if (ascii_equal(c->opcode, "CreateGroup")) {
        if (c->argc < 1 || !group_path(s, c->args[0], path, sizeof(path))) return WS_DDE_FNOTPROCESSED;
        int dir = 0;
        if (exists(s, path, &dir)) { if (!dir) return WS_DDE_FNOTPROCESSED; }
        else {
            int count = count_names(s, s->programs, 1, WS_GROUPS);
            if (count < 0 || (unsigned)count >= WS_GROUPS) { ws_log(s, "[win32] progman op=CreateGroup result=bounds groups=%d", count); return WS_DDE_FNOTPROCESSED; }
            int e = s->fs.mkdir(s->fs.opaque, path);
            if (e && e != EEXIST) { ws_log(s, "[win32] progman op=CreateGroup result=error errno=%d", e); return WS_DDE_FNOTPROCESSED; }
        }
        sanitize(c->args[0], group, 64);
        ws_log(s, "[win32] progman op=CreateGroup group=%s", group);
        return WS_DDE_FACK;
    }
    if (ascii_equal(c->opcode, "DeleteGroup")) {
        if (c->argc < 1 || !group_path(s, c->args[0], path, sizeof(path))) return WS_DDE_FNOTPROCESSED;
        int dir = 0;
        if (!exists(s, path, &dir) || !dir) return WS_DDE_FNOTPROCESSED;
        int e = remove_tree(s, path, 2);
        if (e) { ws_log(s, "[win32] progman op=DeleteGroup result=error errno=%d", e); return WS_DDE_FNOTPROCESSED; }
        char clean[64]; sanitize(c->args[0], clean, sizeof(clean));
        if(ascii_equal(s->active_group,clean))s->active_group[0]=0;
        ws_log(s, "[win32] progman op=DeleteGroup group=%s", clean);
        return WS_DDE_FACK;
    }
    if (ascii_equal(c->opcode, "ShowGroup")) {
        /* Two parameters are required; Windows ignores the second's value. */
        if (c->argc < 2 || !group_path(s, c->args[0], path, sizeof(path))) return WS_DDE_FNOTPROCESSED;
        int dir = 0;
        if (!exists(s, path, &dir) || !dir) return WS_DDE_FNOTPROCESSED;
        sanitize(c->args[0], group, 64);
        ws_log(s, "[win32] progman op=ShowGroup group=%s", group);
        return WS_DDE_FACK;
    }
    if (ascii_equal(c->opcode, "Reload")) {
        if (c->argc > 1) return WS_DDE_FNOTPROCESSED;
        if (c->argc == 1) { int dir = 0; if (!group_path(s, c->args[0], path, sizeof(path)) || !exists(s, path, &dir) || !dir) return WS_DDE_FNOTPROCESSED; }
        return WS_DDE_FACK;
    }
    if (ascii_equal(c->opcode, "ExitProgman")) return WS_DDE_FACK; /* no Program Manager to exit */
    if (ascii_equal(c->opcode, "DeleteItem") || ascii_equal(c->opcode, "ReplaceItem")) {
        char item[64];
        if (c->argc < 1 || !*group || !sanitize(c->args[0], item, sizeof(item))) return WS_DDE_FNOTPROCESSED;
        char dir[WS_PATH];
        if (!path_join(dir, sizeof(dir), s->programs, group, NULL) || !path_join(path, sizeof(path), dir, item, ".lnk")) return WS_DDE_FNOTPROCESSED;
        int e = s->fs.unlink(s->fs.opaque, path);
        if (e) { ws_log(s, "[win32] progman op=%s group=%s item=%s result=error errno=%d", c->opcode, group, item, e); return WS_DDE_FNOTPROCESSED; }
        ws_log(s, "[win32] progman op=%s group=%s item=%s", c->opcode, group, item);
        return WS_DDE_FACK;
    }
    if (ascii_equal(c->opcode, "AddItem")) return add_item(s, c, group);
    ws_log(s, "[win32] progman op=%s result=unknown", c->opcode);
    return WS_DDE_FNOTPROCESSED;
}
static uint32_t execute_commands(struct ws_shell *s, uint32_t pid, uint32_t id, const char *commands, unsigned *executed)
{
    struct ws_command *c = malloc(sizeof(*c)); if (!c) return WS_DDE_FNOTPROCESSED;
    const char *cursor = commands; unsigned done = 0; uint32_t status = WS_DDE_FNOTPROCESSED;
    for (;;) {
        int r = ws_parse_next(&cursor, c);
        if (r < 0) { ws_log(s, "[win32] progman result=syntax at=%u", (unsigned)(cursor - commands)); status = WS_DDE_FNOTPROCESSED; break; }
        if (!r) { status = done ? WS_DDE_FACK : WS_DDE_FNOTPROCESSED; break; }
        if (execute(s, session_group(s,pid,id,1), c) != WS_DDE_FACK) { status = WS_DDE_FNOTPROCESSED; break; }
        done++;
    }
    free(c);
    if (executed) *executed = done;
    return status;
}
uint32_t ws_progman_execute(struct ws_shell *s, uint32_t pid, const char *commands, unsigned *executed)
{ return execute_commands(s,pid,0,commands,executed); }
/* ---- requests ------------------------------------------------------------ */
static int append(char *out, size_t cap, size_t *n, const char *text)
{
    size_t len = strlen(text);
    if (*n + len + 1 > cap) return 0;
    memcpy(out + *n, text, len + 1); *n += len; return 1;
}
uint32_t ws_progman_request(struct ws_shell *s, const char *item, char *out, size_t cap, size_t *n)
{
    *n = 0; if (cap) out[0] = 0;
    char (*names)[WS_FILE_NAME] = calloc(WS_ITEMS + WS_GROUPS, WS_FILE_NAME); if (!names) return 8;
    uint32_t error = 0;
    if (ascii_equal(item, "Groups") || ascii_equal(item, "PROGMAN")) {
        struct listing l = {names, 0, WS_GROUPS, 1, 0};
        if (s->fs.list(s->fs.opaque, s->programs, collect, &l)) l.count = 0;
        sort_names(&l);
        for (unsigned i = 0; i < l.count && !error; i++) if (!append(out, cap, n, l.names[i]) || !append(out, cap, n, "\r\n")) error = 234;
    } else {
        char dir[WS_PATH], clean[64]; int is_dir = 0;
        if (!sanitize(item, clean, sizeof(clean)) || !path_join(dir, sizeof(dir), s->programs, clean, NULL) || !exists(s, dir, &is_dir) || !is_dir) { free(names); return 2; }
        struct listing l = {names, 0, WS_ITEMS, 0, 0};
        if (s->fs.list(s->fs.opaque, dir, collect, &l)) l.count = 0;
        sort_names(&l);
        struct ws_link_record *record = malloc(sizeof(*record));
        char *head = malloc(WS_PATH + 1536); size_t items = 0;
        if (!record || !head) error = 8;
        char *body = calloc(1, cap ? cap : 1); size_t body_n = 0;
        if (!body) error = 8;
        for (unsigned i = 0; !error && i < l.count; i++) {
            size_t len = strlen(l.names[i]);
            if (len < 5 || !ascii_equal(l.names[i] + len - 4, ".lnk")) continue;
            char file[WS_PATH]; if (!path_join(file, sizeof(file), dir, l.names[i], NULL)) continue;
            uint8_t *bytes = NULL; size_t count = 0;
            if (s->fs.read(s->fs.opaque, file, &bytes, &count, WS_LINK_BYTES) || !ws_link_decode(bytes, (unsigned)count, record)) { free(bytes); continue; }
            free(bytes);
            int w = snprintf(head, WS_PATH + 1536, "\"%s\",\"%s%s%s\",%s,%s,0,0,%d,0,%u\r\n", record->label, record->target,
                             record->arguments[0] ? " " : "", record->arguments, record->directory, record->icon, record->icon_index,
                             record->show == 7 ? 1u : 0u);
            if (w < 0 || (size_t)w >= WS_PATH + 1536 || !append(body, cap, &body_n, head)) { error = 234; break; }
            items++;
        }
        if (!error) {
            snprintf(head, WS_PATH + 1536, "\"%s\",C:\\WINDOWS\\Start Menu\\Programs\\%s,%u,3\r\n", clean, clean, (unsigned)items);
            if (!append(out, cap, n, head) || !append(out, cap, n, body)) error = 234;
        }
        free(record); free(head); free(body);
    }
    free(names);
    if (error) { *n = 0; if (cap) out[0] = 0; }
    return error;
}
/* ---- service object and the wr_server hook ------------------------------ */
struct ws_shell *ws_create(const struct ws_fs *fs, const struct ws_config *config, struct wr_server *registry)
{
    if (!registry || !fs || !fs->stat || !fs->mkdir || !fs->rmdir || !fs->unlink || !fs->rename || !fs->read || !fs->write || !fs->list || !config || !config->programs || !config->windows || strlen(config->programs) >= WS_PATH || strlen(config->windows) >= WS_PATH) return NULL;
    struct ws_shell *s = calloc(1, sizeof(*s)); if (!s) return NULL;
    s->fs = *fs; s->registry = registry;
    memcpy(s->programs, config->programs, strlen(config->programs) + 1);
    memcpy(s->windows, config->windows, strlen(config->windows) + 1);
    ws_restart_recover(s);
    return s;
}
void ws_destroy(struct ws_shell *s) { if (s) free(s->wininit); free(s); }
uint32_t ws_call(void *svc, uint32_t pid, const struct w32r_request *a, const char *path, const char *name,
                 struct w32r_reply *r, uint8_t *bytes, size_t cap)
{
    struct ws_shell *s = svc;
    if (a->op == W32R_PROGMAN_OPEN) {
        if (a->key || a->index || a->type || a->flags || a->data_bytes) return 87;
        if ((!ascii_equal(path,"PROGMAN") && !ascii_equal(path,"Shell")) ||
            (!ascii_equal(name,"PROGMAN") && !ascii_equal(name,"AppProperties"))) return 2;
        for(unsigned i=0;i<WS_SESSIONS;i++)if(s->sessions[i].pid && s->registry->now-s->sessions[i].used>300000){
            int live=0;for(unsigned j=0;j<s->registry->live_count;j++)if(s->registry->live[j]==s->sessions[i].pid)live=1;
            if(!live)memset(&s->sessions[i],0,sizeof(s->sessions[i]));
        }
        uint32_t id=wr_new_id(s->registry);if(!id)return 8;
        if (!session_group(s,pid,id,1)) return 8;
        r->key=id;return 0;
    }
    if (a->op >= W32R_PROGMAN_EXECUTE && a->op <= W32R_PROGMAN_CLOSE &&
        (!a->index || !session_group(s,pid,a->index,0))) return 6;
    if (a->op == W32R_PROGMAN_EXECUTE) {
        if (*name || a->key || a->type || a->flags || a->data_bytes || !*path) return 87;
        unsigned executed = 0;
        r->key = execute_commands(s, pid, a->index, path, &executed); r->type = executed;
        return 0;
    }
    if (a->op == W32R_PROGMAN_REQUEST) {
        if (*path || a->key || a->type || a->flags || a->data_bytes || !*name) return 87;
        if (!cap) return 87;
        size_t n = 0; uint32_t e = ws_progman_request(s, name, (char *)bytes, cap, &n);
        if (e) return e;
        r->type = 1; r->data_bytes = (uint32_t)n + 1; /* CF_TEXT with its NUL */
        return 0;
    }
    if (a->op == W32R_PROGMAN_CLOSE) {
        if (*path || *name || a->key || a->type || a->flags || a->data_bytes) return 87;
        for(unsigned i=0;i<WS_SESSIONS;i++) if(s->sessions[i].pid==pid && s->sessions[i].id==a->index)
            memset(&s->sessions[i],0,sizeof(s->sessions[i]));
        return 0;
    }
    if (a->op == W32R_SHUTDOWN) {
        if (a->index == W32R_SHUTDOWN_STATUS) {
            if (*path || a->flags || a->type) return 87;
            r->key = s->phase; r->disposition = ws_restart_pending(s) ? 1 : 0; return 0;
        }
        if (a->index == W32R_SHUTDOWN_BEGIN) { if (*path || a->type || (a->flags & ~0x1fu)) return 87; return ws_restart_begin(s, a->flags); }
        if (a->index == W32R_SHUTDOWN_NEXT) {
            if (*path || a->flags || a->type) return 87;
            uint32_t type = 0, handed = 0;
            uint32_t e = ws_restart_next(s, (char *)bytes, cap, &type, &handed);
            if (e) return e;
            r->type = type; r->key = handed; r->data_bytes = (uint32_t)strlen((char *)bytes) + 1; return 0;
        }
        if (a->index == W32R_SHUTDOWN_END) { if (*path || a->flags || a->type) return 87; return ws_restart_end(s); }
        return 87;
    }
    return 120;
}
