/* SPDX-License-Identifier: MIT
 * G7: compiled once by model.c, over the same replaceable VFS as Files. */
static int search_glob(const char *p, const char *n)
{
    const char *star = NULL, *retry = NULL;
    if (!compare(p, "*.*")) return 1;
    while (*n) {
        if (*p == '?' || (*p && fold((unsigned char)*p) == fold((unsigned char)*n))) { p++; n++; }
        else if (*p == '*') { star = p++; retry = n; }
        else if (star) { p = star + 1; n = ++retry; }
        else return 0;
    }
    while (*p == '*') p++;
    /* DOS-style optional extension: foo.* also finds foo. */
    return !*p || !strcmp(p, ".*");
}
int fm_search_name(const char *pattern, const char *name)
{
    if (!*pattern) return 1;
    while (*pattern) {
        while (*pattern == ' ' || *pattern == ';') pattern++;
        if (!*pattern) break;
        char token[FM_NAME]; unsigned used = 0; int quoted = 0;
        while (*pattern && (quoted || (*pattern != ' ' && *pattern != ';'))) {
            if (*pattern == '"') { quoted = !quoted; pattern++; continue; }
            if (used + 1 >= sizeof(token)) return 0;
            token[used++] = *pattern++;
        }
        token[used] = 0;
        if (!used || quoted) continue;
        if (strchr(token, '*') || strchr(token, '?')) {
            if (search_glob(token, name)) return 1;
        } else {
            for (const char *n = name; *n; n++) {
                unsigned i = 0;
                while (i < used && n[i] && fold((unsigned char)n[i]) == fold((unsigned char)token[i])) i++;
                if (i == used) return 1;
            }
        }
    }
    return 0;
}
int fm_search_size(const char *text, uint64_t *out)
{
    if (!*text) return -EINVAL;
    uint64_t v = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || v > (UINT64_MAX / 1024 - (unsigned)(*text - '0')) / 10) return -EINVAL;
        v = v * 10 + (unsigned)(*text - '0');
    }
    *out = v * 1024; return 0;
}
int fm_search_date(const char *text, int end, int64_t *out)
{
    if (strlen(text) != 10 || text[4] != '-' || text[7] != '-') return -EINVAL;
    for (unsigned i = 0; i < 10; i++) if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) return -EINVAL;
    unsigned y = (unsigned)(text[0]-'0')*1000 + (unsigned)(text[1]-'0')*100 + (unsigned)(text[2]-'0')*10 + (unsigned)(text[3]-'0');
    unsigned m = (unsigned)(text[5]-'0')*10 + (unsigned)(text[6]-'0'), d = (unsigned)(text[8]-'0')*10 + (unsigned)(text[9]-'0');
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1980 || y > 2107 || !m || m > 12 || !d || d > days[m-1] + (m == 2 && !(y%4) && (y%100 || !(y%400)))) return -EINVAL;
    struct tm t = {.tm_year=(int)y-1900, .tm_mon=(int)m-1, .tm_mday=(int)d, .tm_isdst=-1};
    time_t start = mktime(&t);
    if (start == (time_t)-1) return -EINVAL;
    if (end) { t.tm_mday++; t.tm_isdst=-1; start = mktime(&t); if (start == (time_t)-1) return -EINVAL; start--; }
    *out = (int64_t)start; return 0;
}
struct search_walk {
    struct fm_model *m;
    const struct fm_search_query *q;
    struct { struct listing list; char path[FM_PATH]; unsigned next; struct fm_meta meta; } frames[FM_DEPTH];
    unsigned depth, visited, path_capacity, prefix[256];
    unsigned char buffer[4096];
    char full[FM_PATH];
};
static void search_warning(struct search_walk *w, int error)
{ w->m->search_incomplete = 1; if (!w->m->search_error) w->m->search_error = error; }
static int search_content(struct search_walk *w)
{
    const struct fm_vfs *fs = &w->m->fs;
    unsigned length = (unsigned)strlen(w->q->containing), at = 0;
    int fd = fs->open(fs->ctx, w->full, 0);
    if (fd < 0) return fd;
    int result = 0;
    for (;;) {
        int n = fs->read(fs->ctx, fd, w->buffer, sizeof(w->buffer));
        if (n == -EINTR) continue;
        if (n < 0) { result = n; break; }
        if (!n) break;
        if ((unsigned)n > sizeof(w->buffer)) { result = -EIO; break; }
        for (int i = 0; i < n; i++) {
            unsigned c = fold(w->buffer[i]);
            while (at && c != fold((unsigned char)w->q->containing[at])) at = w->prefix[at-1];
            if (c == fold((unsigned char)w->q->containing[at])) at++;
            if (at == length) { result = 1; break; }
        }
        if (result) break;
    }
    int closed = fs->close(fs->ctx, fd);
    return closed < 0 && result >= 0 ? closed : result;
}
static int search_accept(struct search_walk *w, const struct fm_entry *e)
{
    const struct fm_search_query *q = w->q;
    if (!fm_search_name(q->named, e->name)) return 0;
    if (q->date_from_set && e->meta.modified < q->date_from) return 0;
    if (q->date_to_set && e->meta.modified > q->date_to) return 0;
    if (q->size_mode && (e->meta.type != FM_FILE || (q->size_mode == 1 ? e->meta.size < q->size_bytes : e->meta.size > q->size_bytes))) return 0;
    if (*q->type && compare(q->type, "all")) {
        if (!compare(q->type, "folders")) { if (e->meta.type != FM_DIR) return 0; }
        else if (!compare(q->type, "files")) { if (e->meta.type != FM_FILE) return 0; }
        else {
            if (e->meta.type != FM_FILE) return 0;
            if (strchr(q->type, '*') || strchr(q->type, '?')) { if (!search_glob(q->type, e->name)) return 0; }
            else { const char *ext = suffix(e->name), *want = q->type + (q->type[0] == '.'); if (!ext || compare(ext, want)) return 0; }
        }
    }
    return *q->containing ? (e->meta.type == FM_FILE ? search_content(w) : 0) : 1;
}
static int search_add(struct search_walk *w, const struct fm_entry *e)
{
    struct fm_model *m = w->m;
    if (m->count == FM_ENTRIES) return -E2BIG;
    if (m->count == m->capacity) {
        unsigned cap = m->capacity ? m->capacity * 2 : 32;
        struct fm_entry *p = realloc(m->entries, cap * sizeof(*p)); if (!p) return -ENOMEM;
        m->entries = p; m->capacity = cap;
    }
    if (m->result_count == w->path_capacity) {
        unsigned cap = w->path_capacity ? w->path_capacity * 2 : 32;
        void *p = realloc(m->result_paths, cap * FM_PATH); if (!p) return -ENOMEM;
        m->result_paths = p; w->path_capacity = cap;
    }
    strcpy(m->result_paths[m->result_count], w->full);
    m->entries[m->count] = *e; m->entries[m->count].result = ++m->result_count;
    m->count++; return 0;
}
static int search_root(struct search_walk *w, const char *root)
{
    struct fm_model *m = w->m;
    strcpy(w->frames[0].path, root);
    int r = m->fs.stat_path(m->fs.ctx, root, &w->frames[0].meta);
    if (r) return r;
    if (w->frames[0].meta.type != FM_DIR) return -ENOTDIR;
    r = listing(&m->fs, root, &w->frames[0].list);
    if (r) return r;
    w->frames[0].next = 0; w->depth = 1;
    while (w->depth) {
        unsigned top = w->depth-1;
        if (w->frames[top].next == w->frames[top].list.count) {
            free(w->frames[top].list.entries); w->frames[top].list.entries = NULL; w->depth--; continue;
        }
        const struct fm_entry *e = &w->frames[top].list.entries[w->frames[top].next++];
        if (fm_hidden(w->frames[top].path, e->name) || e->meta.type == FM_OTHER) continue;
        if (++w->visited > 65536) { r = -E2BIG; break; }
        r = fm_join(w->full, w->frames[top].path, e->name);
        if (r) { search_warning(w, r); continue; }
        r = search_accept(w, e);
        if (r == -ECANCELED) break;
        if (r < 0) search_warning(w, r);
        else if (r && (r = search_add(w, e))) break;
        r = 0;
        if (e->meta.type != FM_DIR || !w->q->recursive) continue;
        if (w->depth == FM_DEPTH) { search_warning(w, -E2BIG); continue; }
        int cycle = 0;
        for (unsigned i = 0; i < w->depth; i++) if (e->meta.ino && e->meta.ino == w->frames[i].meta.ino && e->meta.dev == w->frames[i].meta.dev) cycle = 1;
        if (cycle) { search_warning(w, -EIO); continue; }
        strcpy(w->frames[w->depth].path, w->full); w->frames[w->depth].meta = e->meta;
        r = listing(&m->fs, w->full, &w->frames[w->depth].list);
        if (r == -ECANCELED) break;
        if (r) { search_warning(w, r); r = 0; continue; }
        w->frames[w->depth].next = 0; w->depth++;
    }
    while (w->depth) { free(w->frames[--w->depth].list.entries); w->frames[w->depth].list.entries = NULL; }
    return r;
}
int fm_search(struct fm_model *m, const struct fm_search_query *query)
{
    /* Build a new snapshot; errors/cancellation never destroy the old one. */
    struct fm_model *out = calloc(1, sizeof(*out));
    struct search_walk *w = calloc(1, sizeof(*w));
    if (!out || !w) { free(out); free(w); return -ENOMEM; }
    fm_init(out, &m->fs); out->query = *query; w->m = out; w->q = &out->query;
    unsigned length = (unsigned)strlen(query->containing);
    for (unsigned i = 1, at = 0; i < length; i++) {
        while (at && fold((unsigned char)query->containing[i]) != fold((unsigned char)query->containing[at])) at = w->prefix[at-1];
        if (fold((unsigned char)query->containing[i]) == fold((unsigned char)query->containing[at])) at++;
        w->prefix[i] = at;
    }
    int r = fm_posix_path(out->path, query->look_in);
    if (!r && fm_is_recycle(out->path)) r = -EINVAL;
    if (!r && out->path[0]) r = search_root(w, out->path);
    else if (!r) {
        struct listing volumes = {0};
        r = fm_volumes(&m->fs, collect, &volumes);
        for (unsigned i = 0; !r && i < volumes.count; i++) {
            char root[FM_PATH]; r = fm_posix_path(root, volumes.entries[i].name);
            if (!r) r = search_root(w, root);
            if (r && r != -ECANCELED && r != -ENOMEM && r != -E2BIG) { search_warning(w, r); r = 0; }
        }
        free(volumes.entries);
        /* Results need a concrete non-virtual path for ordinary file actions. */
        strcpy(out->path, "/");
    }
    if (r == -E2BIG) { search_warning(w, r); r = 0; }
    if (!r) {
        free(m->entries); free(m->bin); free(m->result_paths); m->bin = NULL; m->bin_count = 0;
        m->entries = out->entries; out->entries = NULL; m->count = out->count; m->capacity = out->capacity;
        m->result_paths = out->result_paths; out->result_paths = NULL; m->result_count = out->result_count;
        m->query = out->query; m->searching = 1; m->search_incomplete = out->search_incomplete; m->search_error = out->search_error;
        strcpy(m->path, out->path); m->active = m->anchor = 0; fm_sort(m, m->sort, m->descending);
    }
    fm_destroy(out); free(out); free(w); return r;
}
