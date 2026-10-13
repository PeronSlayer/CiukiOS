/* SPDX-License-Identifier: MIT
 * Shared file clipboard over the same VFS as Files. Text remains independent. */
#include "model.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CLIP "/system/settings/files.clip"

static int write_all(const struct fm_vfs *f, int fd, const void *buffer, unsigned n)
{
    const char *p = buffer;
    while (n) { int r = f->write(f->ctx, fd, p, n); if (r <= 0) return r < 0 ? r : -EIO; p += r; n -= (unsigned)r; }
    return 0;
}
int fm_clipboard_save(struct fm_model *m)
{
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
}
int fm_clipboard_load(struct fm_model *m)
{
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
}

int fm_clipboard_consume(struct fm_model *m)
{
    struct fm_model *now=calloc(1,sizeof(*now)); if (!now) return -ENOMEM;
    now->fs=m->fs; int r=fm_clipboard_load(now);
    int same=!r && now->cut==m->cut && now->clipboard_count==m->clipboard_count;
    for (unsigned i=0;same && i<m->clipboard_count;i++) same=fm_same_path(now->clipboard[i],m->clipboard[i]);
    if (same) { fm_clipboard_clear(now); r=fm_clipboard_save(now); }
    fm_destroy(now); free(now); fm_clipboard_clear(m); return r;
}
