/* SPDX-License-Identifier: MIT
 * Terminal scripting (G27, AX-17): %VARIABLES%, pipes and redirection
 * (cmd | cmd, > file, >> file, < file), batch files (.BAT/.CMD with the
 * Windows 98 COMMAND.COM control commands) and the commands the prompt was
 * missing: SET, ATTRIB, LABEL, VOL, CHKDSK, FIND, SORT, MORE, IPCONFIG.
 * Windows 98 reference: the MS-DOS 7 command interpreter (batch syntax,
 * ERRORLEVEL, FOR %v IN (set) DO, standard stream redirection). */
#include "terminal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LINE_MAX_EXPANDED (DU_TEXT_MAX * 2)
#define STEP_LIMIT 100000u
#define FILE_LIMIT (1024u * 1024u)
#define NO_INPUT (-100000)   /* a filter was started with neither files nor a pipe */

static int same(const char *a, const char *b) { return !strcasecmp(a, b); }
static int word_char(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* ---- environment ---- */
const char *term_getenv(const struct terminal *t, const char *name)
{
    size_t n = strlen(name);
    for (unsigned i = 0; i < t->env_count; i++)
        if (!strncasecmp(t->env[i], name, n) && t->env[i][n] == '=') return t->env[i] + n + 1;
    return NULL;
}
int term_setenv(struct terminal *t, const char *name, const char *value)
{
    size_t n = strlen(name);
    if (!n || strchr(name, '=')) return -EINVAL;
    for (unsigned i = 0; i < t->env_count; i++)
        if (!strncasecmp(t->env[i], name, n) && t->env[i][n] == '=') {
            free(t->env[i]); memmove(&t->env[i], &t->env[i + 1], (t->env_count - i - 1) * sizeof(*t->env)); t->env_count--; break;
        }
    if (!value || !*value) return 0;
    if (t->env_count == TERM_ENV) return -ENOMEM;
    char *entry = malloc(n + strlen(value) + 2);
    if (!entry) return -ENOMEM;
    for (size_t i = 0; i < n; i++) entry[i] = (char)((name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i]);   /* names are kept in capitals, as DOS */
    entry[n] = '='; strcpy(entry + n + 1, value);
    t->env[t->env_count++] = entry;
    return 0;
}
void term_env_free(struct terminal *t)
{
    for (unsigned i = 0; i < t->env_count; i++) free(t->env[i]);
    t->env_count = 0;
}

/* ---- batch frame ---- */
struct batch {
    char *text, **lines; unsigned count;
    char *args[10]; int argc;        /* %0 is the file; %1..%9 shift */
    unsigned depth;
    int exit_code, finished;
    char zero[FM_PATH];              /* %0: the batch file as the prompt shows it */
};

/* %NAME%, %ERRORLEVEL%, and in a batch file %0-%9, %* and %% (a literal percent). Unknown names expand to nothing
 * in a batch file and stay as typed at the prompt. */
static int expand(const struct terminal *t, const char *in, char *out, size_t cap)
{
    const struct batch *b = t->batch; size_t n = 0;
#define PUT(c) do { if (n + 1 >= cap) return -E2BIG; out[n++] = (char)(c); } while (0)
    while (*in) {
        if (*in != '%') { PUT(*in++); continue; }
        if (b && in[1] == '%') { PUT('%'); in += 2; continue; }
        if (b && in[1] >= '0' && in[1] <= '9') {
            const char *v = b->args[in[1] - '0'];
            for (; v && *v; v++) PUT(*v);
            in += 2; continue;
        }
        if (b && in[1] == '*') {
            for (int i = 1; i < 10; i++) if (b->args[i]) { if (i > 1) PUT(' '); for (const char *v = b->args[i]; *v; v++) PUT(*v); }
            in += 2; continue;
        }
        const char *end = in + 1;
        while (word_char((unsigned char)*end)) end++;
        if (end == in + 1 || *end != '%') { PUT(*in++); continue; }   /* not a variable: %x of FOR, or a lone percent */
        char name[64]; size_t len = (size_t)(end - in - 1);
        if (len >= sizeof(name)) return -E2BIG;
        memcpy(name, in + 1, len); name[len] = 0;
        const char *v = same(name, "ERRORLEVEL") ? NULL : term_getenv(t, name);
        char level[16];
        if (same(name, "ERRORLEVEL")) { snprintf(level, sizeof(level), "%d", t->errorlevel); v = level; }
        if (v) for (; *v; v++) PUT(*v);
        else if (!b) { for (const char *c = in; c <= end; c++) PUT(*c); }
        in = end + 1;
    }
    out[n] = 0;
#undef PUT
    return 0;
}

/* ---- files ---- */
static int read_file(struct terminal *t, const char *path, char **data, size_t *length)
{
    struct fm_meta meta; int r = t->host.fs.stat_path(t->host.fs.ctx, path, &meta);
    if (r) return r;
    if (meta.type != FM_FILE) return -EISDIR;
    if (meta.size > FILE_LIMIT) return -EFBIG;
    int fd = t->host.fs.open(t->host.fs.ctx, path, 0);
    if (fd < 0) return fd;
    char *buffer = malloc((size_t)meta.size + 1);
    if (!buffer) { t->host.fs.close(t->host.fs.ctx, fd); return -ENOMEM; }
    size_t used = 0;
    while (used < meta.size) {
        int n = t->host.fs.read(t->host.fs.ctx, fd, buffer + used, (unsigned)(meta.size - used > 65536 ? 65536 : meta.size - used));
        if (n < 0) { r = n; break; }
        if (!n) break;
        used += (size_t)n;
    }
    int c = t->host.fs.close(t->host.fs.ctx, fd);
    if (!r) r = c;
    if (r) { free(buffer); return r; }
    buffer[used] = 0; *data = buffer; *length = used;
    return 0;
}
static int write_file(struct terminal *t, const char *path, const char *data, size_t length, int append)
{
    char *old = NULL; size_t old_length = 0; int r;
    if (append) {
        r = read_file(t, path, &old, &old_length);
        if (r && r != -ENOENT) return r;
    }
    struct fm_meta meta;
    if (!t->host.fs.stat_path(t->host.fs.ctx, path, &meta)) {
        if (meta.type != FM_FILE) { free(old); return -EISDIR; }
        r = t->host.fs.unlink(t->host.fs.ctx, path);
        if (r) { free(old); return r; }
    }
    int fd = t->host.fs.open(t->host.fs.ctx, path, 1);
    if (fd < 0) { free(old); return fd; }
    r = 0;
    const char *parts[2] = {old, data}; size_t sizes[2] = {old_length, length};
    for (int k = 0; k < 2 && !r; k++) {
        const char *p = parts[k]; size_t left = sizes[k];
        while (left && !r) {
            int n = t->host.fs.write(t->host.fs.ctx, fd, p, (unsigned)(left > 65536 ? 65536 : left));
            if (n <= 0) { r = n ? n : -EIO; break; }
            p += n; left -= (size_t)n;
        }
    }
    if (!r && t->host.fs.sync) r = t->host.fs.sync(t->host.fs.ctx, fd);
    int c = t->host.fs.close(t->host.fs.ctx, fd);
    free(old);
    return r ? r : c;
}

/* ---- pipes and redirection ---- */
struct stage { char *text; char in[FM_PATH], out[FM_PATH]; int append, has_in, has_out; };

/* Splits at unquoted '|'; the stages are views into the writable copy. */
static int split_pipes(char *line, char **stages, int max)
{
    int n = 0, quote = 0; stages[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '"') quote = !quote;
        else if (*p == '|' && !quote) { if (n == max) return -E2BIG; *p = 0; stages[n++] = p + 1; }
    }
    return quote ? -EINVAL : n;
}
static int filename_token(char **p, char *out, size_t cap)
{
    char *s = *p; size_t n = 0; int quote = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '<' || *s == '>' || *s == '|') return -EINVAL;
    while (*s && (quote || (*s != ' ' && *s != '\t' && *s != '<' && *s != '>'))) {
        if (*s == '"') { quote = !quote; s++; continue; }
        if (n + 1 >= cap) return -ENAMETOOLONG;
        out[n++] = *s++;
    }
    out[n] = 0; *p = s;
    return n ? 0 : -EINVAL;
}
/* Extracts '< file', '> file', '>> file' from a stage, blanking them in the text. */
static int redirections(char *text, struct stage *st)
{
    int quote = 0;
    for (char *p = text; *p; ) {
        if (*p == '"') { quote = !quote; p++; continue; }
        if (quote || (*p != '<' && *p != '>')) { p++; continue; }
        char *begin = p; int input = *p == '<';
        p++;
        if (!input && *p == '>') { st->append = 1; p++; } else if (!input) st->append = 0;
        int r = filename_token(&p, input ? st->in : st->out, FM_PATH);
        if (r) return r;
        if (input) st->has_in = 1; else st->has_out = 1;
        for (char *c = begin; c < p; c++) *c = ' ';
    }
    return 0;
}
static int run_command(struct terminal *t, const char *text);

static int run_pipeline(struct terminal *t, const char *line)
{
    char *copy = malloc(strlen(line) + 1), *stages[TERM_STAGES]; int r = 0;
    if (!copy) return -ENOMEM;
    strcpy(copy, line);
    int n = split_pipes(copy, stages, TERM_STAGES);
    if (n < 0) { free(copy); return n; }
    struct stage *st = calloc((size_t)n, sizeof(*st));
    if (!st) { free(copy); return -ENOMEM; }
    int simple = n == 1;
    for (int i = 0; i < n && !r; i++) { st[i].text = stages[i]; r = redirections(stages[i], &st[i]); simple = simple && !st[i].has_in && !st[i].has_out; }
    char *carry = NULL; size_t carry_length = 0;
    if (r) term_write(t, "Syntax error in the redirection.\n");
    else if (simple) r = run_command(t, st[0].text);
    for (int i = 0; !simple && i < n && !r; i++) {
        char *input = NULL; size_t input_length = 0; int last = i == n - 1;
        if (st[i].has_in) {
            char path[FM_PATH];
            r = term_path(t, st[i].in, path);
            if (!r) r = read_file(t, path, &input, &input_length);
            if (r) { term_printf(t, "%s: %s (error %d)\n", st[i].in, strerror(-r), -r); break; }
        } else if (i > 0) { input = carry; input_length = carry_length; carry = NULL; }
        const char *saved_in = t->in_data; size_t saved_length = t->in_length;
        t->in_data = input; t->in_length = input_length;
        int capturing = !last || st[i].has_out;
        char *saved = t->capture; size_t saved_size = t->captured; int saved_error = t->capture_error, error;
        char *out = NULL;
        if (capturing) {
            out = malloc(TERM_CAPTURE + 1);
            if (!out) r = -ENOMEM;
            else { t->capture = out; t->captured = 0; t->capture_error = 0; out[0] = 0; }
        }
        if (!r) {
            r = run_command(t, st[i].text);
            error = t->capture_error;
        } else error = 0;
        size_t produced = t->captured;
        if (capturing) { t->capture = saved; t->captured = saved_size; t->capture_error = saved_error; }   /* a last stage writes straight into the outer output */
        t->in_data = saved_in; t->in_length = saved_length;
        free(input);
        if (capturing && out) {
            if (error && !r) r = error;
            if (st[i].has_out) {
                char path[FM_PATH]; int w = term_path(t, st[i].out, path);
                if (!w && !path[0]) w = -EINVAL;
                if (!w) w = write_file(t, path, out, produced, st[i].append);
                if (w) { term_printf(t, "%s: %s (error %d)\n", st[i].out, strerror(-w), -w); if (!r) r = w; }
                free(out);
            } else { carry = out; carry_length = produced; }
        }
        if (r && !last) r = 0;   /* DOS keeps the pipeline going: what a failing stage printed is the next one's data */
    }
    free(carry); free(st); free(copy);
    return r;
}

/* ---- control commands and the line dispatcher ---- */
static char *skip(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int word_is(const char *s, const char *word)
{
    size_t n = strlen(word);
    return !strncasecmp(s, word, n) && (!s[n] || s[n] == ' ' || s[n] == '\t' || s[n] == '.' || s[n] == '/');
}
static int token(char **p, char *out, size_t cap)
{
    char *s = skip(*p); size_t n = 0; int quote = 0;
    while (*s && (quote || (*s != ' ' && *s != '\t'))) { if (*s == '"') quote = !quote; if (n + 1 < cap) out[n++] = *s; s++; }
    out[n] = 0; *p = s;
    return (int)n;
}
static int exec_line(struct terminal *t, unsigned *pc, char *line);

static int label_index(const struct batch *b, const char *label)
{
    for (unsigned i = 0; i < b->count; i++) {
        char *s = skip(b->lines[i]);
        if (*s != ':' || s[1] == ':') continue;
        s++; size_t n = 0;
        while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
        if (n == strlen(label) && !strncasecmp(s, label, n)) return (int)i;
    }
    return -1;
}
static void substitute(char *out, size_t cap, const char *body, char var, const char *value)
{
    size_t n = 0;
    for (; *body && n + 1 < cap; body++) {
        if (body[0] == '%' && body[1] == var) { for (const char *v = value; *v && n + 1 < cap; v++) out[n++] = *v; body++; }
        else out[n++] = *body;
    }
    out[n] = 0;
}
static int exec_if(struct terminal *t, unsigned *pc, char *rest)
{
    int negate = 0; char a[FM_PATH], b[FM_PATH], keyword[32];
    char *p = skip(rest); char *q = p;
    token(&q, keyword, sizeof(keyword));
    if (same(keyword, "NOT")) { negate = 1; p = q; }
    p = skip(p); q = p;
    token(&q, keyword, sizeof(keyword));
    int truth = 0;
    if (same(keyword, "EXIST")) {
        p = q; if (!token(&p, a, sizeof(a))) return -EINVAL;
        char **names; unsigned count;
        char unquoted[FM_PATH]; size_t k = 0;
        for (const char *c = a; *c && k + 1 < sizeof(unquoted); c++) if (*c != '"') unquoted[k++] = *c;
        unquoted[k] = 0;
        if (strpbrk(unquoted, "*?")) { truth = !term_glob(t, unquoted, &names, &count) && count; if (truth) term_glob_free(names, count); }
        else { char path[FM_PATH]; struct fm_meta meta; truth = !term_path(t, unquoted, path) && path[0] && !t->host.fs.stat_path(t->host.fs.ctx, path, &meta); }
    } else if (same(keyword, "ERRORLEVEL")) {
        p = q; if (!token(&p, a, sizeof(a))) return -EINVAL;
        char *end; long n = strtol(a, &end, 10);
        if (*end) return -EINVAL;
        truth = t->errorlevel >= n;
    } else {
        p = skip(p);
        if (!token(&p, a, sizeof(a))) return -EINVAL;
        char *eq = strstr(a, "==");
        if (eq) { *eq = 0; snprintf(b, sizeof(b), "%s", eq + 2); if (!b[0]) { char *r2 = p; if (!token(&r2, b, sizeof(b))) return -EINVAL; p = r2; } }
        else {
            char op[8]; if (!token(&p, op, sizeof(op)) || strcmp(op, "==")) return -EINVAL;
            if (!token(&p, b, sizeof(b))) return -EINVAL;
        }
        truth = !strcmp(a, b);
    }
    if (negate) truth = !truth;
    p = skip(p);
    if (!*p) return -EINVAL;
    if (!truth) return 0;
    char *command = malloc(strlen(p) + 1);
    if (!command) return -ENOMEM;
    strcpy(command, p);
    int r = exec_line(t, pc, command);
    free(command);
    return r;
}
static int exec_for(struct terminal *t, unsigned *pc, char *rest)
{
    char *p = skip(rest);
    if (*p != '%' || !((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z'))) return -EINVAL;
    char var = p[1]; p = skip(p + 2);
    if (!word_is(p, "IN")) return -EINVAL;
    p = skip(p + 2);
    if (*p != '(') return -EINVAL;
    char *close = strchr(p, ')');
    if (!close) return -EINVAL;
    *close = 0;
    char *set = p + 1, *after = skip(close + 1);
    if (!word_is(after, "DO")) return -EINVAL;
    char *body = skip(after + 2);
    if (!*body) return -EINVAL;
    char *items[TERM_ARGS * 4]; unsigned count = 0;
    char *copy = malloc(strlen(set) + 1);
    if (!copy) return -ENOMEM;
    strcpy(copy, set);
    char *argv[TERM_ARGS]; int n = term_parse(copy, argv), r = 0;
    char **globs[TERM_ARGS]; unsigned glob_counts[TERM_ARGS]; unsigned globs_used = 0;
    if (n < 0) { free(copy); return n; }
    for (int i = 0; i < n && count < sizeof(items) / sizeof(items[0]); i++) {
        if (strpbrk(argv[i], "*?")) {
            char **names; unsigned found;
            if (!term_glob(t, argv[i], &names, &found)) {
                globs[globs_used] = names; glob_counts[globs_used++] = found;
                for (unsigned k = 0; k < found && count < sizeof(items) / sizeof(items[0]); k++) items[count++] = names[k];
            }
        } else items[count++] = argv[i];
    }
    for (unsigned i = 0; i < count && !r; i++) {
        char *line = malloc(LINE_MAX_EXPANDED);
        if (!line) { r = -ENOMEM; break; }
        substitute(line, LINE_MAX_EXPANDED, body, var, items[i]);
        r = exec_line(t, pc, line);
        free(line);
        if (t->interrupted || t->quit) r = -EINTR;
    }
    for (unsigned i = 0; i < globs_used; i++) term_glob_free(globs[i], glob_counts[i]);
    free(copy);
    return r;
}

static int call_batch(struct terminal *t, char *rest);

/* One logical line: control commands first, then pipes and the command itself. */
static int exec_line(struct terminal *t, unsigned *pc, char *line)
{
    struct batch *b = t->batch;
    char *s = skip(line);
    if (!*s) return 0;
    if (word_is(s, "REM") || !strncmp(s, "::", 2)) return 0;
    if (word_is(s, "ECHO") && strncasecmp(s, "ECHO.", 5)) {
        char *p = skip(s + 4);
        if (same(p, "OFF")) { t->echo_off = 1; return 0; }
        if (same(p, "ON")) { t->echo_off = 0; return 0; }
    }
    if (word_is(s, "GOTO")) {
        if (!b) { term_write(t, "GOTO: only in a batch file.\n"); return -EINVAL; }
        char *p = skip(s + 4); if (*p == ':') p++;
        char label[64]; size_t n = 0;
        while (*p && *p != ' ' && *p != '\t' && n + 1 < sizeof(label)) label[n++] = *p++;
        label[n] = 0;
        if (same(label, "EOF")) { b->finished = 1; return 0; }
        int at = label_index(b, label);
        if (at < 0) { term_printf(t, "Label not found - %s\n", label); b->finished = 1; return -ENOENT; }
        if (pc) *pc = (unsigned)at + 1;
        return 0;
    }
    if (word_is(s, "SHIFT")) {
        if (!b) return -EINVAL;
        for (int i = 1; i < 9; i++) b->args[i] = b->args[i + 1];
        b->args[9] = NULL;
        return 0;
    }
    if (word_is(s, "PAUSE")) { term_write(t, "Press any key to continue . . .\n"); return 0; }   /* the batch runs to completion: no key is awaited */
    if (word_is(s, "IF")) return exec_if(t, pc, s + 2);
    if (word_is(s, "FOR")) return exec_for(t, pc, s + 3);
    if (word_is(s, "CALL")) return call_batch(t, s + 4);
    if (word_is(s, "EXIT") && b && !strncasecmp(skip(s + 4), "/B", 2)) {
        char *p = skip(skip(s + 4) + 2); b->exit_code = atoi(p); b->finished = 1; return b->exit_code ? -b->exit_code : 0;
    }
    return run_pipeline(t, s);
}
static int run_command(struct terminal *t, const char *text) { return term_command(t, text); }

/* ---- batch files ---- */
static int batch_name(const char *name) { size_t n = strlen(name); return n > 4 && (same(name + n - 4, ".bat") || same(name + n - 4, ".cmd")); }

int term_batch(struct terminal *t, const char *path, int argc, char **argv)
{
    if (t->batch_depth >= TERM_BATCH_DEPTH) { term_write(t, "Batch files are nested too deeply.\n"); return -EIO; }
    char *data; size_t length; int r = read_file(t, path, &data, &length);
    if (r) return r;
    if (length > TERM_BATCH_MAX) { free(data); return -EFBIG; }
    struct batch b = {0};
    b.text = data;
    unsigned lines = 1;
    for (size_t i = 0; i < length; i++) lines += data[i] == '\n';
    b.lines = calloc(lines, sizeof(*b.lines));
    if (!b.lines) { free(data); return -ENOMEM; }
    for (char *p = data; p && *p; ) {
        char *end = strchr(p, '\n');
        b.lines[b.count++] = p;
        if (end) { *end = 0; if (end > p && end[-1] == '\r') end[-1] = 0; p = end + 1; } else { size_t n = strlen(p); if (n && p[n - 1] == '\r') p[n - 1] = 0; p = NULL; }
    }
    if (fm_display_path(b.zero, path)) snprintf(b.zero, sizeof(b.zero), "%s", path);
    b.args[0] = b.zero;
    for (int i = 0; i < argc && i < 9; i++) b.args[i + 1] = argv[i];
    b.argc = argc;
    void *saved = t->batch; int saved_echo = t->echo_off;
    t->batch = &b; t->batch_depth++;
    unsigned pc = 0, steps = 0; r = 0;
    char *line = malloc(LINE_MAX_EXPANDED);
    if (!line) r = -ENOMEM;
    for (;;) {
        if (!line || pc >= b.count || b.finished) break;
        if (t->interrupted || t->quit) { r = -EINTR; break; }
        if (++steps > STEP_LIMIT) { term_write(t, "The batch file ran too long and was stopped.\n"); r = -EIO; break; }
        if (t->host.pump && !(steps % 64)) t->host.pump(t->host.ctx);
        char *raw = skip(b.lines[pc++]);
        int quiet = 0;
        if (*raw == '@') { quiet = 1; raw = skip(raw + 1); }
        if (!*raw || *raw == ':' ) continue;
        if (expand(t, raw, line, LINE_MAX_EXPANDED)) { term_write(t, "The line is too long.\n"); r = -E2BIG; break; }
        if (!quiet && !t->echo_off) {
            char cwd[FM_PATH]; if (fm_display_path(cwd, t->cwd)) snprintf(cwd, sizeof(cwd), "%s", t->cwd);
            term_printf(t, "%s>%s\n", cwd, line);
        }
        int e = exec_line(t, &pc, line);
        t->errorlevel = e ? -e : 0;
        r = e;   /* the batch file ends with the status of its last command */
        if (b.finished) break;
    }
    if (b.exit_code) r = -b.exit_code;
    free(line);
    t->batch = saved; t->batch_depth--; t->echo_off = saved_echo;   /* ECHO OFF lasts for the batch file */
    free(b.lines); free(data);
    return r;
}
static int call_batch(struct terminal *t, char *rest)
{
    char *copy = malloc(strlen(rest) + 1);
    if (!copy) return -ENOMEM;
    strcpy(copy, rest);
    char *argv[TERM_ARGS]; int n = term_parse(copy, argv), r;
    if (n < 0) { free(copy); return n; }
    if (!n) { free(copy); return -EINVAL; }
    char path[FM_PATH], with_ext[FM_PATH]; struct fm_meta meta;
    r = term_path(t, argv[0], path);
    if (!r && !batch_name(argv[0])) {
        snprintf(with_ext, sizeof(with_ext), "%s.bat", path);
        if (!t->host.fs.stat_path(t->host.fs.ctx, with_ext, &meta)) strcpy(path, with_ext);
        else { snprintf(with_ext, sizeof(with_ext), "%s.cmd", path); if (!t->host.fs.stat_path(t->host.fs.ctx, with_ext, &meta)) strcpy(path, with_ext); }
    }
    if (!r) r = t->host.fs.stat_path(t->host.fs.ctx, path, &meta);
    if (r) term_printf(t, "%s: %s (error %d)\n", argv[0], strerror(-r), -r);
    else r = term_batch(t, path, n - 1, argv + 1);   /* a nonzero status is the batch file's ERRORLEVEL, not a failure to report */
    free(copy);
    return r;
}

int term_execute(struct terminal *t, const char *line)
{
    if (strlen(line) >= DU_TEXT_MAX) return term_command(t, line);   /* reports "Command too long." */
    char *expanded = malloc(LINE_MAX_EXPANDED);
    if (!expanded) return -ENOMEM;
    int r = expand(t, line, expanded, LINE_MAX_EXPANDED);
    if (r) { term_write(t, "The command is too long after expanding variables.\n"); free(expanded); return r; }
    unsigned pc = 0;
    r = exec_line(t, &pc, expanded);
    free(expanded);
    t->errorlevel = r ? -r : 0;
    return r;
}

/* ---- commands: SET, ATTRIB, LABEL, VOL, CHKDSK, FIND, SORT, MORE, IPCONFIG, PING, FORMAT ---- */
static int drive_of(struct terminal *t, const char *arg)
{
    if (arg && arg[0] && arg[1] == ':' && !arg[2]) { int c = arg[0] | 0x20; return c >= 'a' && c <= 'z' ? c - 'a' : -1; }
    if (arg && arg[0]) return -1;
    int v = fm_volume(t->cwd);
    return v >= 'A' && v <= 'Z' ? v - 'A' : 2;
}
static void put_lines(struct terminal *t, char **lines, unsigned count)
{
    for (unsigned i = 0; i < count; i++) { term_write(t, lines[i]); term_write(t, "\n"); }
}
/* Splits text into lines (views into a writable copy). */
static char **lines_of(const char *text, size_t length, unsigned *count, char **copy)
{
    char *c = malloc(length + 1);
    if (!c) return NULL;
    memcpy(c, text, length); c[length] = 0;
    unsigned n = 0; for (size_t i = 0; i < length; i++) n += c[i] == '\n';
    n += length && c[length - 1] != '\n';
    char **lines = calloc(n ? n : 1, sizeof(*lines));
    if (!lines) { free(c); return NULL; }
    unsigned k = 0;
    for (char *p = c; *p && k < n; ) {
        char *end = strchr(p, '\n'); lines[k++] = p;
        if (end) { *end = 0; if (end > p && end[-1] == '\r') end[-1] = 0; p = end + 1; } else break;
    }
    *count = k; *copy = c;
    return lines;
}
static int reverse_compare(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
static int forward_compare(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
static int casefind(const char *hay, const char *needle, int ignore)
{
    if (!ignore) return strstr(hay, needle) != NULL;
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return 1;
    return !n;
}
/* Text of the file arguments, or the standard input when there are none. */
static int gather(struct terminal *t, int n, char **a, int first, char **text, size_t *length, int *owned)
{
    *owned = 0;
    if (first >= n) {
        if (!t->in_data) { *text = NULL; *length = 0; return NO_INPUT; }
        *text = (char *)t->in_data; *length = t->in_length; return 0;
    }
    char *all = NULL; size_t used = 0;
    for (int i = first; i < n; i++) {
        char **names; unsigned count;
        int r = term_glob(t, a[i], &names, &count);
        if (r) { free(all); return r; }
        for (unsigned k = 0; k < count; k++) {
            char path[FM_PATH], *data; size_t size;
            const char *name = names[k];
            r = term_path(t, name, path);
            if (!r) r = read_file(t, path, &data, &size);
            if (r) { term_glob_free(names, count); free(all); return r; }
            char *grown = realloc(all, used + size + 1);
            if (!grown) { free(data); term_glob_free(names, count); free(all); return -ENOMEM; }
            all = grown; memcpy(all + used, data, size); used += size; all[used] = 0; free(data);
        }
        term_glob_free(names, count);
    }
    if (!all) { all = malloc(1); if (!all) return -ENOMEM; all[0] = 0; }
    *text = all; *length = used; *owned = 1;
    return 0;
}
static int cmd_find(struct terminal *t, int n, char **a)
{
    int ignore = 0, invert = 0, count_only = 0, number = 0, i = 1;
    for (; i < n && a[i][0] == '/'; i++) {
        if (same(a[i], "/I")) ignore = 1; else if (same(a[i], "/V")) invert = 1; else if (same(a[i], "/C")) count_only = 1;
        else if (same(a[i], "/N")) number = 1; else return -EINVAL;
    }
    if (i >= n) { term_write(t, "FIND: the search string is missing.\n"); return -EINVAL; }
    const char *needle = a[i++];
    char *text; size_t length; int owned, r = gather(t, n, a, i, &text, &length, &owned);
    if (r == NO_INPUT) { term_write(t, "FIND: no input.\n"); return -EINVAL; }
    if (r) return r;
    unsigned count, hits = 0; char *copy; char **lines = lines_of(text, length, &count, &copy);
    if (!lines) { if (owned) free(text); return -ENOMEM; }
    for (unsigned k = 0; k < count; k++) {
        if (casefind(lines[k], needle, ignore) == invert) continue;
        hits++;
        if (!count_only) { if (number) term_printf(t, "[%u]", k + 1); term_write(t, lines[k]); term_write(t, "\n"); }
    }
    if (count_only) term_printf(t, "%u\n", hits);
    free(lines); free(copy); if (owned) free(text);
    return 0;
}
static int cmd_sort(struct terminal *t, int n, char **a)
{
    int reverse = 0, i = 1;
    for (; i < n && a[i][0] == '/'; i++) { if (same(a[i], "/R")) reverse = 1; else return -EINVAL; }
    char *text; size_t length; int owned, r = gather(t, n, a, i, &text, &length, &owned);
    if (r == NO_INPUT) { term_write(t, "SORT: no input.\n"); return -EINVAL; }
    if (r) return r;
    unsigned count; char *copy; char **lines = lines_of(text, length, &count, &copy);
    if (!lines) { if (owned) free(text); return -ENOMEM; }
    qsort(lines, count, sizeof(*lines), reverse ? reverse_compare : forward_compare);
    put_lines(t, lines, count);
    free(lines); free(copy); if (owned) free(text);
    return 0;
}
static int cmd_more(struct terminal *t, int n, char **a)
{
    char *text; size_t length; int owned, r = gather(t, n, a, 1, &text, &length, &owned);
    if (r == NO_INPUT) { term_write(t, "MORE: no input.\n"); return -EINVAL; }
    if (r) return r;
    term_write(t, text);   /* the scroll-back replaces paging */
    if (length && text[length - 1] != '\n') term_write(t, "\n");
    if (owned) free(text);
    return 0;
}
static void attribute_text(uint32_t attributes, char out[8])
{
    out[0] = attributes & 0x20 ? 'A' : ' '; out[1] = ' '; out[2] = ' ';
    out[3] = attributes & 4 ? 'S' : ' '; out[4] = attributes & 2 ? 'H' : ' '; out[5] = attributes & 1 ? 'R' : ' '; out[6] = 0;
}
static int cmd_attrib(struct terminal *t, int n, char **a)
{
    uint32_t set = 0, clear = 0; int i = 1, change = 0;
    for (; i < n && (a[i][0] == '+' || a[i][0] == '-') && a[i][1] && !a[i][2]; i++) {
        uint32_t bit; switch (a[i][1] | 0x20) { case 'r': bit = 1; break; case 'h': bit = 2; break; case 's': bit = 4; break; case 'a': bit = 0x20; break; default: return -EINVAL; }
        if (a[i][0] == '+') { set |= bit; clear &= ~bit; } else { clear |= bit; set &= ~bit; }
        change = 1;
    }
    const char *pattern = i < n ? a[i] : "*";
    if (i + 1 < n) return -EINVAL;
    if (change && !t->host.attributes) return -ENOSYS;
    char **names; unsigned count; int r = term_glob(t, pattern, &names, &count);
    if (r) return r;
    for (unsigned k = 0; k < count && !r; k++) {
        char path[FM_PATH]; struct fm_meta meta;
        r = term_path(t, names[k], path);
        if (!r) r = t->host.fs.stat_path(t->host.fs.ctx, path, &meta);
        if (r) break;
        uint32_t now = meta.attributes;
        if (change) {
            if (meta.type == FM_DIR) { term_printf(t, "%s: folders keep their attributes.\n", names[k]); continue; }
            uint32_t next = (now & ~clear) | set;
            if (next != now) r = t->host.attributes(t->host.ctx, path, next & 0x27u);
            now = next;
        }
        if (!r) {
            char flags[8], display[FM_PATH]; attribute_text(now, flags);
            if (fm_display_path(display, path)) snprintf(display, sizeof(display), "%s", path);
            term_printf(t, "%s     %s\n", flags, display);
        }
    }
    term_glob_free(names, count);
    return r;
}
static int cmd_label(struct terminal *t, int n, char **a, int show_only)
{
    int drive = drive_of(t, n > 1 ? a[1] : NULL), text_at = 1;
    if (n > 1 && drive >= 0) text_at = 2;
    else if (n > 1) { if (show_only) return -EINVAL; drive = drive_of(t, NULL); }   /* LABEL text without a drive letter names the current drive */
    if (drive < 0) return -EINVAL;
    char label[33] = "";
    if (!t->host.label_get) return -ENOSYS;
    if (!show_only && text_at < n) {
        if (!t->host.label_set) return -ENOSYS;
        char text[40]; size_t k = 0;
        for (int i = text_at; i < n && k + 2 < sizeof(text); i++) { if (i > text_at) text[k++] = ' '; for (const char *c = a[i]; *c && k + 1 < sizeof(text); c++) text[k++] = *c; }
        text[k] = 0;
        return t->host.label_set(t->host.ctx, (unsigned)drive, text);
    }
    int r = t->host.label_get(t->host.ctx, (unsigned)drive, label);
    if (r) return r;
    if (label[0]) term_printf(t, " Volume in drive %c is %s\n", 'A' + drive, label);
    else term_printf(t, " Volume in drive %c has no label\n", 'A' + drive);
    return 0;
}
struct census { char **dirs; unsigned dir_count, dir_cap; uint64_t files, folders, bytes, hidden_files, hidden_bytes; int overflow; char parent[FM_PATH]; };
static int census_entry(const char *name, const struct fm_meta *m, void *ctx)
{
    struct census *c = ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (m->type == FM_DIR) {
        c->folders++;
        if (c->dir_count == c->dir_cap) {
            unsigned cap = c->dir_cap ? c->dir_cap * 2 : 16; char **grown = realloc(c->dirs, cap * sizeof(*grown));
            if (!grown) return -ENOMEM;
            c->dirs = grown; c->dir_cap = cap;
        }
        char path[FM_PATH]; if (fm_join(path, c->parent, name)) { c->overflow = 1; return 0; }
        char *copy = malloc(strlen(path) + 1); if (!copy) return -ENOMEM;
        strcpy(copy, path); c->dirs[c->dir_count++] = copy;
    } else {
        c->files++; c->bytes += m->size;
        if (m->attributes & 2) { c->hidden_files++; c->hidden_bytes += m->size; }
    }
    return 0;
}
static int cmd_chkdsk(struct terminal *t, int n, char **a)
{
    if (n > 2 || (n == 2 && a[1][0] == '/')) { if (n == 2 && a[1][0] == '/') { term_write(t, "CHKDSK checks only: /F is not available, nothing is repaired.\n"); return -EINVAL; } return -EINVAL; }
    int drive = drive_of(t, n == 2 ? a[1] : NULL);
    if (drive < 0) return -EINVAL;
    char root[16]; snprintf(root, sizeof(root), drive == 2 ? "/" : "/mnt/%c", 'a' + drive);
    struct fm_meta meta;
    int r = t->host.fs.stat_path(t->host.fs.ctx, root, &meta);
    if (r) return r;
    struct census c = {0};
    unsigned head = 0;
    c.dirs = malloc(16 * sizeof(*c.dirs)); c.dir_cap = 16;
    if (!c.dirs) return -ENOMEM;
    c.dirs[0] = malloc(strlen(root) + 1);
    if (!c.dirs[0]) { free(c.dirs); return -ENOMEM; }
    strcpy(c.dirs[0], root); c.dir_count = 1;
    unsigned budget = 100000;
    while (head < c.dir_count && !r && budget--) {
        snprintf(c.parent, sizeof(c.parent), "%s", c.dirs[head]);
        r = t->host.fs.list(t->host.fs.ctx, c.parent, FM_LIST_SNIFF, census_entry, &c);
        if (t->host.pump && !(head % 16)) t->host.pump(t->host.ctx);
        if (t->interrupted) r = -EINTR;
        head++;
    }
    for (unsigned i = 0; i < c.dir_count; i++) free(c.dirs[i]);
    free(c.dirs);
    if (r) return r;
    char label[33] = "";
    if (t->host.label_get && !t->host.label_get(t->host.ctx, (unsigned)drive, label) && label[0]) term_printf(t, " Volume %s\n", label);
    uint64_t total = 0, free_bytes = 0;
    int have_space = t->host.space && !t->host.space(t->host.ctx, (unsigned)drive, &total, &free_bytes);
    if (have_space) term_printf(t, "%14llu bytes total disk space\n", (unsigned long long)total);
    term_printf(t, "%14llu bytes in %llu hidden files\n", (unsigned long long)c.hidden_bytes, (unsigned long long)c.hidden_files);
    term_printf(t, "%14llu bytes in %llu folders\n", 0ull, (unsigned long long)c.folders);
    term_printf(t, "%14llu bytes in %llu user files\n", (unsigned long long)(c.bytes - c.hidden_bytes), (unsigned long long)(c.files - c.hidden_files));
    if (have_space) term_printf(t, "%14llu bytes available on disk\n", (unsigned long long)free_bytes);
    term_write(t, "No errors are looked for in the file system: this is a read-only survey (use ScanDisk for repairs).\n");
    return 0;
}
static int cmd_ipconfig(struct terminal *t, int n, char **a)
{
    int all = n == 2 && same(a[1], "/ALL");
    if (n > 2 || (n == 2 && !all)) return -EINVAL;
    struct ciuki_net_state s = {.version = 1, .size = sizeof(s)};
    int r = t->host.net ? t->host.net(t->host.ctx, &s) : -ENODEV;
    if (r) return r;
    term_write(t, "CiukiOS IP Configuration\n\nEthernet adapter:\n");
    if (all) term_printf(t, "   Description . . . . . . . . : %.16s\n   Physical Address. . . . . . : %02X-%02X-%02X-%02X-%02X-%02X\n",
                         s.driver, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
    term_printf(t, "   Media State . . . . . . . . : %s\n", s.link == CIUKI_NET_LINK_UP ? "Connected" : s.link == CIUKI_NET_LINK_DOWN ? "Cable disconnected" : "Unknown");
    if (s.address_configured) term_printf(t, "   IP Address. . . . . . . . . : %u.%u.%u.%u\n", s.ipv4[0], s.ipv4[1], s.ipv4[2], s.ipv4[3]);
    else term_write(t, "   IP Address. . . . . . . . . : 0.0.0.0\n");
    if (all) term_printf(t, "   Adapter state . . . . . . . : %s\n", s.state == CIUKI_NET_RUNNING ? "running" : s.state == CIUKI_NET_QUARANTINED ? "quarantined" : "stopped");
    return 0;
}
static int cmd_set(struct terminal *t, int n, char **a)
{
    if (n == 1) {
        for (unsigned i = 0; i < t->env_count; i++) { term_write(t, t->env[i]); term_write(t, "\n"); }
        return 0;
    }
    /* Re-join: "SET NAME=two words" arrives as arguments. */
    char text[DU_TEXT_MAX]; size_t k = 0;
    for (int i = 1; i < n; i++) { if (i > 1 && k + 1 < sizeof(text)) text[k++] = ' '; for (const char *c = a[i]; *c && k + 1 < sizeof(text); c++) text[k++] = *c; }
    text[k] = 0;
    char *eq = strchr(text, '=');
    if (!eq) {
        size_t len = strlen(text); int found = 0;
        for (unsigned i = 0; i < t->env_count; i++) if (!strncasecmp(t->env[i], text, len) && t->env[i][len] == '=') { term_write(t, t->env[i]); term_write(t, "\n"); found = 1; }
        if (!found) term_printf(t, "Environment variable %s not defined\n", text);
        return found ? 0 : -ENOENT;
    }
    *eq = 0;
    return term_setenv(t, text, eq + 1);
}

int term_extra(struct terminal *t, int n, char **a, int *result)
{
    int r;
    if (same(a[0], "SET")) r = cmd_set(t, n, a);
    else if (same(a[0], "ATTRIB")) r = cmd_attrib(t, n, a);
    else if (same(a[0], "LABEL")) r = cmd_label(t, n, a, 0);
    else if (same(a[0], "VOL")) r = cmd_label(t, n, a, 1);
    else if (same(a[0], "CHKDSK")) r = cmd_chkdsk(t, n, a);
    else if (same(a[0], "FIND")) r = cmd_find(t, n, a);
    else if (same(a[0], "SORT")) r = cmd_sort(t, n, a);
    else if (same(a[0], "MORE")) r = cmd_more(t, n, a);
    else if (same(a[0], "IPCONFIG")) r = cmd_ipconfig(t, n, a);
    else if (same(a[0], "PING")) { term_write(t, "PING needs raw ICMP sockets, which the network stack does not offer yet.\n"); r = -ENOSYS; }
    else if (same(a[0], "FORMAT")) { term_write(t, "FORMAT needs the kernel's disk-format service, which is not available yet.\n"); r = -ENOSYS; }
    else {
        /* NAME, NAME.BAT or NAME.CMD in the current folder (or a path) runs the batch file. */
        char path[FM_PATH], with_ext[FM_PATH]; struct fm_meta meta;
        if (term_path(t, a[0], path) || !path[0]) return 0;
        int found = batch_name(a[0]) && !t->host.fs.stat_path(t->host.fs.ctx, path, &meta);
        if (!found && !batch_name(a[0])) {
            snprintf(with_ext, sizeof(with_ext), "%s.bat", path);
            if (!t->host.fs.stat_path(t->host.fs.ctx, with_ext, &meta)) { strcpy(path, with_ext); found = 1; }
            else { snprintf(with_ext, sizeof(with_ext), "%s.cmd", path); if (!t->host.fs.stat_path(t->host.fs.ctx, with_ext, &meta)) { strcpy(path, with_ext); found = 1; } }
        }
        if (!found) { if (!batch_name(a[0])) return 0; *result = -ENOENT; return 1; }
        r = term_batch(t, path, n - 1, a + 1);
    }
    *result = r;
    return 1;
}
