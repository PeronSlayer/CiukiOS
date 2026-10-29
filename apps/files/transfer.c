#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: MIT
 * Shared file clipboard over the same VFS as Files. Text remains independent. */
#include "model.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(__i386__) || defined(CIUKI_SYSTEM_CLIPBOARD)
#define SYSTEM_CLIPBOARD 1
int fm_system_clipboard_save(struct fm_model *);
int fm_system_clipboard_load(struct fm_model *);
int fm_system_clipboard_consume(struct fm_model *);
#endif
#define CLIP "/system/settings/files.clip"

static int write_all(const struct fm_vfs *f, int fd, const void *buffer, unsigned n)
{
    const char *p = buffer;
    while (n) { int r = f->write(f->ctx, fd, p, n); if (r <= 0) return r < 0 ? r : -EIO; p += r; n -= (unsigned)r; }
    return 0;
}
int fm_clipboard_save(struct fm_model *m)
{
#ifdef SYSTEM_CLIPBOARD
    return fm_system_clipboard_save(m);
#else
    int r = m->fs.mkdir(m->fs.ctx, "/system/settings");
    if (r && r != -EEXIST) return r;
    char temp[64]; int fd=-EEXIST;
    /* Exclusive creation allocates each writer its own temporary pathname. */
    for (unsigned n=0;n<1000 && fd==-EEXIST;n++) {
        snprintf(temp,sizeof(temp),"/system/settings/files.%u.new",n);
        fd=m->fs.open(m->fs.ctx,temp,1);
    }
    if (fd<0) return fd;
    r = write_all(&m->fs, fd, m->cut ? "M\n" : "C\n", 2);
    for (unsigned i = 0; i < m->clipboard_count && !r; i++) {
        if (strchr(m->clipboard[i], '\n') || strchr(m->clipboard[i], '\r')) { r = -EINVAL; break; }
        r = write_all(&m->fs, fd, m->clipboard[i], (unsigned)strlen(m->clipboard[i]));
        if (!r) r = write_all(&m->fs, fd, "\n", 1);
    }
    if (!r) r = m->fs.sync(m->fs.ctx, fd);
    int closed = m->fs.close(m->fs.ctx, fd); if (!r) r = closed;
    if (!r) r = m->fs.rename(m->fs.ctx, temp, CLIP);
    if (r) m->fs.unlink(m->fs.ctx, temp);
    return r;
#endif
}
int fm_clipboard_load(struct fm_model *m)
{
#ifdef SYSTEM_CLIPBOARD
    return fm_system_clipboard_load(m);
#else
    int fd = m->fs.open(m->fs.ctx, CLIP, 0);
    if (fd == -ENOENT) { fm_clipboard_clear(m); return 0; }
    if (fd < 0) return fd;
    struct fm_model *next = calloc(1, sizeof(*next));
    if (!next) { m->fs.close(m->fs.ctx, fd); return -ENOMEM; }
    char line[FM_PATH], block[256]; unsigned used = 0, lines = 0, at=0, available=0; int r = 0;
    for (;;) {
        if (at==available) {
            int got=m->fs.read(m->fs.ctx,fd,block,sizeof(block));
            if (got<=0) { if (got<0) r=got; else if (used || !lines) r=-EINVAL; break; }
            at=0; available=(unsigned)got;
        }
        char c=block[at++];
        if (c != '\n') { if (!c || c == '\r' || used + 1 >= sizeof(line)) { r = -EINVAL; break; } line[used++] = c; continue; }
        line[used] = 0;
        if (!lines++) { if (strcmp(line, "M") && strcmp(line, "C")) { r = -EINVAL; break; } next->cut = line[0] == 'M'; }
        else {
            if (line[0] != '/' || next->clipboard_count >= FM_ENTRIES) { r = -EINVAL; break; }
            char **p = realloc(next->clipboard, (next->clipboard_count + 1) * sizeof(*p));
            if (!p) { r = -ENOMEM; break; } next->clipboard = p;
            p[next->clipboard_count] = malloc(used + 1);
            if (!p[next->clipboard_count]) { r = -ENOMEM; break; }
            memcpy(p[next->clipboard_count++], line, used + 1);
        }
        used = 0;
    }
    int closed = m->fs.close(m->fs.ctx, fd); if (!r) r = closed;
    if (!r) { fm_clipboard_clear(m); m->clipboard = next->clipboard; m->clipboard_count = next->clipboard_count; m->cut = next->cut; next->clipboard = NULL; next->clipboard_count = 0; }
    fm_clipboard_clear(next); free(next); return r;
#endif
}

/* fx-19 drag data object: "X\n" then one "<D|F>/path\n" per item, written
 * like the clipboard (exclusive temporary, sync, replacing rename). */
#define DRAG "/system/settings/files.drag"
int fm_drag_save(const struct fm_vfs *fs, char *const *paths, const char *types, unsigned count)
{
    if (!count || count > FM_ENTRIES) return -EINVAL;
    int r = fs->mkdir(fs->ctx, "/system/settings");
    if (r && r != -EEXIST) return r;
    char temp[64]; int fd = -EEXIST;
    for (unsigned n = 0; n < 1000 && fd == -EEXIST; n++) {
        snprintf(temp, sizeof(temp), "/system/settings/drag.%u.new", n);
        fd = fs->open(fs->ctx, temp, 1);
    }
    if (fd < 0) return fd;
    r = write_all(fs, fd, "X\n", 2);
    for (unsigned i = 0; i < count && !r; i++) {
        size_t n = strlen(paths[i]);
        if (paths[i][0] != '/' || n >= FM_PATH || strchr(paths[i], '\n') || strchr(paths[i], '\r')) { r = -EINVAL; break; }
        char type = types && types[i] == 'D' ? 'D' : 'F';
        r = write_all(fs, fd, &type, 1);
        if (!r) r = write_all(fs, fd, paths[i], (unsigned)n);
        if (!r) r = write_all(fs, fd, "\n", 1);
    }
    if (!r) r = fs->sync(fs->ctx, fd);
    int closed = fs->close(fs->ctx, fd); if (!r) r = closed;
    if (!r) r = fs->rename(fs->ctx, temp, DRAG);
    if (r) fs->unlink(fs->ctx, temp);
    return r;
}
void fm_drag_free(char **paths, char *types, unsigned count)
{
    for (unsigned i = 0; paths && i < count; i++) free(paths[i]);
    free(paths); free(types);
}
int fm_drag_load(const struct fm_vfs *fs, char ***out, char **out_types, unsigned *out_count)
{
    *out = NULL; if (out_types) *out_types = NULL; *out_count = 0;
    int fd = fs->open(fs->ctx, DRAG, 0);
    if (fd < 0) return fd;
    char **paths = NULL, *types = NULL, line[FM_PATH + 1], block[256];
    unsigned count = 0, used = 0, lines = 0, at = 0, available = 0; int r = 0;
    for (;;) {
        if (at == available) {
            int got = fs->read(fs->ctx, fd, block, sizeof(block));
            if (got <= 0) { if (got < 0) r = got; else if (used || lines < 2) r = -EINVAL; break; }
            at = 0; available = (unsigned)got;
        }
        char c = block[at++];
        if (c != '\n') { if (!c || c == '\r' || used + 1 >= sizeof(line)) { r = -EINVAL; break; } line[used++] = c; continue; }
        line[used] = 0;
        if (!lines++) { if (strcmp(line, "X")) { r = -EINVAL; break; } }
        else {
            if ((line[0] != 'D' && line[0] != 'F') || line[1] != '/' || count >= FM_ENTRIES) { r = -EINVAL; break; }
            char **p = realloc(paths, (count + 1) * sizeof(*p));
            if (!p) { r = -ENOMEM; break; } paths = p;
            char *t = realloc(types, count + 1);
            if (!t) { r = -ENOMEM; break; } types = t;
            paths[count] = malloc(used);
            if (!paths[count]) { r = -ENOMEM; break; }
            memcpy(paths[count], line + 1, used); types[count++] = line[0];
        }
        used = 0;
    }
    int closed = fs->close(fs->ctx, fd); if (!r) r = closed;
    if (r) { fm_drag_free(paths, types, count); return r; }
    *out = paths; *out_count = count;
    if (out_types) *out_types = types; else free(types);
    return 0;
}

int fm_clipboard_consume(struct fm_model *m)
{
#ifdef SYSTEM_CLIPBOARD
    return fm_system_clipboard_consume(m);
#else
    struct fm_model *now=calloc(1,sizeof(*now)); if (!now) return -ENOMEM;
    now->fs=m->fs; int r=fm_clipboard_load(now);
    int same=!r && now->cut==m->cut && now->clipboard_count==m->clipboard_count;
    for (unsigned i=0;same && i<m->clipboard_count;i++) same=fm_same_path(now->clipboard[i],m->clipboard[i]);
    if (same) { fm_clipboard_clear(now); r=fm_clipboard_save(now); }
    fm_destroy(now); free(now); fm_clipboard_clear(m); return r;
#endif
}

#include "transfer.h"
#include <time.h>
#include <stdarg.h>
#ifdef __i386__
#include <ciuki/raw.h>
#endif
const char *fm_job_verb(enum fm_job_kind kind)
{ return kind==FM_COPY ? "copy" : kind==FM_MOVE ? "move" : kind==FM_DELETE ? "delete" : "read"; }
uint64_t job_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void job_record(const char *fmt, ...)
{
    int saved = errno; char line[241]; va_list ap;
    va_start(ap, fmt); int n = vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    if (n > 0) {
        if ((unsigned)n >= sizeof(line)) n = sizeof(line)-1;
#ifdef __i386__
        (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line, (uint32_t)n, 0, 0, 0, 0);
#else
        fprintf(stderr, "%s\n", line);
#endif
    }
    errno = saved;
}
static void transfer_publish(struct files_transfer *w, uint64_t started, int finished)
{
    struct fm_job *j = w->job;
    uint64_t now=job_now();
    struct transfer_view v = {.state=j->state, .kind=j->kind, .bytes=j->bytes, .total=j->total_bytes,
        .elapsed=job_now()-started, .items=j->items_done,
        .count=j->total_items, .counting=j->counting, .files=j->files, .folders=j->folders};
    uint64_t sampled=w->samples_started ? job_now()-w->samples_started : 0;
    if (sampled >= 2000000000ull && !v.counting) v.rate = v.bytes * 1000ull / (sampled / 1000000ull);
    const char *source = j->count ? j->sources[0] : "";
    if (j->depth) {
        struct fm_frame *f=&j->frames[j->depth-1]; source=f->source;
        v.file_total=f->meta.size;
        if (!j->counting && f->meta.type==FM_FILE && (f->phase==1 || f->phase==3)) v.file_bytes=j->file_bytes;
        v.meta=f->meta; v.existing=f->existing; v.prompt_dir=j->prompt_dir;
        snprintf(v.target,sizeof(v.target),"%s",f->target);
    }
    else if (j->next) source=j->sources[j->next-1];
    snprintf(v.source, sizeof(v.source), "%s", source);
    pthread_mutex_lock(&w->lock);
    int changed=finished || v.state!=w->view.state || !w->notified_at || now-w->notified_at>=100000000ull;
    w->view = v;
    w->finished = finished;
    if (changed) w->publication++;
    pthread_mutex_unlock(&w->lock);
    if (changed && w->notify) { w->notified_at=now; w->notify(w->notify_ctx); }
}
static void *transfer_run(void *arg)
{
    struct files_transfer *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->pending && !w->stopping) pthread_cond_wait(&w->changed, &w->lock);
        if (w->stopping && !w->pending) { pthread_mutex_unlock(&w->lock); break; }
        w->pending = 0; pthread_mutex_unlock(&w->lock);
        struct fm_job *j = w->job;
        uint64_t started=job_now();
        w->samples_started=0;
        int announced=0;
        if (w->prepare) {
            int r=w->prepare(w->ctx);
            if (r) { j->error=-r; j->state=FM_FAILED; }
        }
        for (;;) {
            pthread_mutex_lock(&w->lock);
            int cancel=w->cancel || w->stopping, answer=w->answer;
            w->answer=-1;
            pthread_mutex_unlock(&w->lock);
            if (cancel) fm_job_cancel(j);
            else if (answer >= 0) fm_job_answer(j, (enum fm_replace)answer);
            if (j->state == FM_RUNNING) fm_job_step(j);
            uint64_t now=job_now();
            if (!j->counting && !w->samples_started) w->samples_started=now;
            if (!announced && (!j->counting || j->state != FM_RUNNING)) {
                if (j->kind != FM_PROPERTIES) job_record("[files] job=%s start items=%u bytes=%llu", fm_job_verb(j->kind), j->total_items, (unsigned long long)j->total_bytes);
                announced=1;
            }
            transfer_publish(w, started, 0);
            if (j->state == FM_REPLACE) {
                pthread_mutex_lock(&w->lock);
                while (!w->stopping && !w->cancel && w->answer < 0) {
                    uint64_t paused=job_now();
                    pthread_cond_wait(&w->changed, &w->lock);
                    if (w->samples_started) w->samples_started += job_now()-paused;
                }
                pthread_mutex_unlock(&w->lock);
                continue;
            }
            if (j->state != FM_RUNNING) break;
        }
        if (j->kind != FM_PROPERTIES) {
            uint64_t ms=(job_now()-started)/1000000ull;
            if (j->state == FM_CANCELLED) job_record("[files] job=%s cancel", fm_job_verb(j->kind));
            job_record("[files] job=%s done ms=%u bytes=%llu rate_kbs=%u errors=%u", fm_job_verb(j->kind),
                (unsigned)ms, (unsigned long long)j->bytes, (unsigned)(ms ? j->bytes*1000/(ms*1024) : 0), j->error ? 1u : 0u);
        }
        if (w->complete) w->complete(w->ctx);
        /* Publish terminal ownership only after the last job access. */
        transfer_publish(w, started, 1);
    }
    return NULL;
}
int fm_transfer_create(struct files_transfer **out, struct fm_job *job)
{
    if (*out) return 0;
    struct files_transfer *w=calloc(1,sizeof(*w));
    if (!w) return -ENOMEM;
    w->job=job; w->answer=-1;
    int e=pthread_mutex_init(&w->lock,NULL);
    if (e) { free(w); return -e; }
    e=pthread_cond_init(&w->changed,NULL);
    if (e) { pthread_mutex_destroy(&w->lock); free(w); return -e; }
    e=pthread_create(&w->thread,NULL,transfer_run,w);
    if (e) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); free(w); return -e; }
    *out=w; return 0;
}
void fm_transfer_command(struct files_transfer *w, int cancel, int answer)
{
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    if (cancel) w->cancel=1;
    if (answer >= 0) w->answer=answer;
    if (cancel || answer >= 0) w->view.state=FM_RUNNING;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
}
void fm_transfer_destroy(struct files_transfer *w)
{
    if (!w) return;
    pthread_mutex_lock(&w->lock); w->stopping=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock);
    free(w);
}

void fm_transfer_reset(struct files_transfer *w)
{
    pthread_mutex_lock(&w->lock);
    w->view=(struct transfer_view){.state=FM_RUNNING,.kind=w->job->kind,.counting=w->job->counting,.preparing=w->prepare!=NULL};
    if (w->job->count) snprintf(w->view.source,sizeof(w->view.source),"%s",w->job->sources[0]);
    w->finished=w->cancel=0; w->answer=-1; w->started_at=w->progress_logged=job_now();
    pthread_mutex_unlock(&w->lock);
}
void fm_transfer_submit(struct files_transfer *w)
{
    pthread_mutex_lock(&w->lock); w->pending=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
}

/* G6 Folder Options: small, versioned shared settings, published with the
 * same exclusive/sync/rename discipline as the file clipboard. */
static const char view_header[]="CiukiFilesView1\n";
int fm_view_load(struct fm_model *m,int *attributes)
{
    const struct fm_vfs *f=&m->fs; char text[64]; unsigned used=0; int r=0;
    if (!f->open || !f->read || !f->close || !f->stat_path) return 0;
    struct fm_meta meta;
    int state=f->stat_path(f->ctx,"/system/settings/files.view",&meta);
    if (state==-ENOENT || (!state && meta.type!=FM_FILE)) return 0;
    if (state) return state;
    int fd=f->open(f->ctx,"/system/settings/files.view",0);
    if (fd==-ENOENT) return 0;
    if (fd<0) return fd;
    while (used<sizeof(text)) {
        int n=f->read(f->ctx,fd,text+used,(unsigned)sizeof(text)-used);
        if (n<0) { r=n; break; } if (!n) break;
        if ((unsigned)n>sizeof(text)-used) { r=-EIO; break; }
        used+=(unsigned)n;
    }
    int close_error=f->close(f->ctx,fd); if (!r) r=close_error;
    unsigned at=(unsigned)sizeof(view_header)-1;
    if (!r && (used!=at+5 || memcmp(text,view_header,at) || text[at+4]!='\n')) r=-EINVAL;
    for (unsigned k=0;!r && k<4;k++) if (text[at+k]!='0' && text[at+k]!='1') r=-EINVAL;
    if (!r) { m->show_hidden=text[at]-'0'; m->hide_system=text[at+1]-'0'; m->hide_extensions=text[at+2]-'0'; *attributes=text[at+3]-'0'; }
    return r;
}
int fm_view_save(struct fm_model *m,int attributes)
{
    const struct fm_vfs *f=&m->fs;
    if (!f->mkdir || !f->open || !f->write || !f->sync || !f->close || !f->rename || !f->unlink) return -ENOSYS;
    int r=f->mkdir(f->ctx,"/system/settings"); if (r && r!=-EEXIST) return r;
    char temporary[64],text[64]; int fd=-EEXIST;
    for (unsigned k=0;k<1000 && fd==-EEXIST;k++) {
        snprintf(temporary,sizeof(temporary),"/system/settings/view.%u.new",k);
        fd=f->open(f->ctx,temporary,1);
    }
    if (fd<0) return fd;
    int n=snprintf(text,sizeof(text),"%s%d%d%d%d\n",view_header,!!m->show_hidden,!!m->hide_system,!!m->hide_extensions,!!attributes);
    r=write_all(f,fd,text,(unsigned)n);
    if (!r) r=f->sync(f->ctx,fd);
    int close_error=f->close(f->ctx,fd); if (!r) r=close_error;
    if (!r) r=f->rename(f->ctx,temporary,"/system/settings/files.view");
    if (r) f->unlink(f->ctx,temporary);
    return r;
}

#include "navigation.inc"
