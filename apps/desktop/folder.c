/* SPDX-License-Identifier: MIT
 * Files operations served outside shell input; jobs advance incrementally. */
#include "desktop.h"
#include "folder.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void error(struct desktop *d, int r)
{
    char text[180];
    snprintf(text, sizeof(text), "Cannot complete the file operation (error %d).\n%s", -r,
        r == -EINVAL ? "A name cannot contain \\ / : * ? \" < > | or end in a space or dot." :
        r == -EEXIST ? "A file or folder with that name already exists." : "The original item is kept when the operation fails.");
    desk_shell_message(d, "Desktop", text, 1);
}
void desk_folder_changed(struct desktop *d)
{
    d->sh.icon_count = 0; d->sh.icon_drag=d->sh.click_icon=-1;
    desk_shell_apply(d);
}
int desk_folder_init(struct desktop *d, const struct fm_vfs *fs)
{
    struct desk_folder *f = calloc(1, sizeof(*f)); if (!f) return -ENOMEM;
    fm_init(&f->model, fs); f->editing = -1; f->index = -1;
    int r = fm_navigate(&f->model, DESK_FOLDER);
    if (r) { fm_destroy(&f->model); free(f); return r; }
    f->ready = 1; d->sh.folder = f; desk_folder_changed(d); return 0;
}
void desk_folder_destroy(struct desktop *d)
{
    if (!d->sh.folder) return;
    fm_job_destroy(&d->sh.folder->job); fm_destroy(&d->sh.folder->model);
    free(d->sh.folder); d->sh.folder = NULL;
}
void desk_folder_rename(struct desktop *d, int index)
{
    struct desk_folder *f = d->sh.folder;
    if (!f || f->busy || index < 3 || index>=d->sh.icon_count || (unsigned)(index-3) >= f->model.count) return;
    f->editing = index-3; f->select_all = 1;
    snprintf(f->edit, sizeof(f->edit), "%s", f->model.entries[f->editing].name);
    f->cursor = (unsigned)strlen(f->edit);
    desk_focus(d,-1); desk_damage_add(&d->damage,d->sh.icons[index].box,d->width,d->height);
}
static int bitmap(struct fm_model *m, const char *name)
{
    /* One white pixel, 24-bit uncompressed BMP, padded scanline. */
    static const unsigned char bmp[58] = {
        'B','M',58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,1,0,0,0,1,0,0,0,
        1,0,24,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255,0 };
    char path[FM_PATH]; int r = fm_join(path,m->path,name); if (r) return r;
    int fd = m->fs.open(m->fs.ctx,path,1); if (fd < 0) return fd;
    unsigned at=0;
    while (at < sizeof(bmp)) { int n=m->fs.write(m->fs.ctx,fd,bmp+at,sizeof(bmp)-at); if (n<=0) { r=n<0?n:-EIO; break; } at+=(unsigned)n; }
    if (!r) r=m->fs.sync(m->fs.ctx,fd);
    int closed=m->fs.close(m->fs.ctx,fd); if (!r) r=closed;
    if (r) m->fs.unlink(m->fs.ctx,path); else { r=fm_refresh(m); if (!r) fm_select_name(m,name,0); }
    return r;
}
static int job(struct desk_folder *f, enum fm_job_kind kind, char **paths, unsigned n, const char *to)
{
    fm_job_destroy(&f->job);
    int r=fm_job_start(&f->job,&f->model.fs,kind,paths,n,to);
    f->busy = !r; return r;
}
void desk_folder_drop(struct desktop *d, int source, int copy)
{
    /* source == -1: desktop selection; otherwise Files has published it. */
    struct desk_folder *f=d->sh.folder; if (!f || f->busy || d->power.state) return;
    int r=0;
    if (source < 0) {
        int index=d->sh.icon_selected-3;
        if (index < 0 || (unsigned)index >= f->model.count) return;
        fm_select(&f->model,(unsigned)index,0);
        r=fm_clipboard(&f->model,!copy); if (!r) r=fm_clipboard_save(&f->model);
    }
    char destination[FM_PATH]; snprintf(destination,sizeof(destination),"%s",DESK_FOLDER);
    int folder_target=0;
    for (int i=3;i<d->sh.icon_count;i++) if ((unsigned)(i-3)<f->model.count &&
        f->model.entries[i-3].meta.type==FM_DIR && desk_contains(d->sh.icons[i].box,d->mouse_x,d->mouse_y) &&
        (source>=0 || i!=d->sh.icon_selected)) {
        if (!fm_entry_path(&f->model,(unsigned)(i-3),destination)) folder_target=1;
        break;
    }
    int target=desk_hit(d,d->mouse_x,d->mouse_y);
    if (!r && target >= 0 && d->clients[target].program == DESK_APP_FILES &&
        desk_contains(desk_content(&d->clients[target]),d->mouse_x,d->mouse_y)) {
        struct ciuki_message msg; struct desk_box b=desk_content(&d->clients[target]);
        desk_message(&msg,DESK_FILE_DROP,16); msg.data[0]=(uint8_t)d->clients[target].version; msg.data[2]=(uint8_t)d->clients[target].minor;
        desk_put32(msg.data+4,copy ? 1u : 0u); desk_put32(msg.data+8,(uint32_t)(d->mouse_x-b.x)); desk_put32(msg.data+12,(uint32_t)(d->mouse_y-b.y));
        desk_queue(d,target,&msg);
    } else if (!r && target < 0 && (source >= 0 || folder_target) && d->mouse_y >= DESK_BAR && d->mouse_y < (int)d->height-DESK_BAR) {
        r=fm_clipboard_load(&f->model);
        if (!r && f->model.clipboard_count) r=job(f,copy ? FM_COPY : FM_MOVE,f->model.clipboard,f->model.clipboard_count,destination);
    }
    if (r) error(d,r);
}
void desk_folder_service(struct desktop *d)
{
    struct desk_folder *f=d->sh.folder; if (!f || d->power.state) return;
    struct fm_model *m=&f->model;
    if (f->busy) {
        if (d->sh.want_power) fm_job_cancel(&f->job);
        for (unsigned n=0;n<24 && f->job.state==FM_RUNNING;n++) fm_job_step(&f->job);
        if (f->job.state==FM_REPLACE) { fm_job_answer(&f->job,FM_CANCEL); error(d,-EEXIST); }
        if (f->job.state!=FM_RUNNING) {
            if (f->job.state==FM_FAILED) error(d,-f->job.error);
            if (f->job.state==FM_DONE && f->job.kind==FM_MOVE && m->cut) fm_clipboard_consume(m);
            f->busy=0; fm_refresh(m); desk_folder_changed(d);
        }
    }
    int command=f->command, index=f->index; f->command=DF_NONE;
    if (f->busy || !command) {
        /* Files and applications may change this directory independently. */
        if (!f->busy && f->editing<0 && d->now_ns-f->poll_ns >= 1000000000ull) {
            f->poll_ns=d->now_ns;
            struct fm_model *check=calloc(1,sizeof(*check));
            if (check) {
                fm_init(check,&m->fs);
                if (!fm_navigate(check,DESK_FOLDER)) {
                    int changed=check->count!=m->count;
                    for (unsigned i=0;!changed && i<m->count;i++) changed=strcmp(check->entries[i].name,m->entries[i].name) || check->entries[i].meta.size!=m->entries[i].meta.size || check->entries[i].meta.modified!=m->entries[i].meta.modified;
                    if (changed) { fm_refresh(m); desk_folder_changed(d); }
                }
                fm_destroy(check); free(check);
            }
        }
        return;
    }
    int r=0; char name[FM_NAME], path[FM_PATH];
    if (command==DF_FOLDER || command==DF_TEXT || command==DF_BITMAP) {
        r=fm_unique(m,command==DF_FOLDER ? "New Folder" : command==DF_TEXT ? "New Text Document" : "New Bitmap Image",command==DF_FOLDER ? "" : command==DF_TEXT ? ".txt" : ".bmp",name);
        if (!r) r=command==DF_BITMAP ? bitmap(m,name) : fm_create(m,name,command==DF_FOLDER);
        if (!r) { desk_folder_changed(d); d->sh.icon_selected=(int)m->active+3; desk_folder_rename(d,d->sh.icon_selected); }
    } else if (command==DF_REFRESH) { r=fm_refresh(m); if (!r) desk_folder_changed(d); }
    else if (command==DF_PASTE) { r=fm_clipboard_load(m); if (!r && m->clipboard_count) r=job(f,m->cut ? FM_MOVE : FM_COPY,m->clipboard,m->clipboard_count,m->path); }
    else if (index>=0 && (unsigned)index<m->count) {
        if (command==DF_RENAME) {
            int x,y,placed=cfg_icon_position(&d->sh.cfg,m->entries[index].name,&x,&y);
            snprintf(name,sizeof(name),"%s",m->entries[index].name);
            r=fm_rename(m,(unsigned)index,f->edit);
            if (!r) { cfg_icon_forget(&d->sh.cfg,name); d->sh.want_save=1; if (placed) { cfg_icon_move(&d->sh.cfg,f->edit,x,y); d->sh.want_save=1; } f->editing=-1; desk_folder_changed(d); d->sh.icon_selected=(int)m->active+3; }
        } else if (command==DF_DELETE) {
            struct fm_model *trash=calloc(1,sizeof(*trash)); if (!trash) r=-ENOMEM;
            else {
                fm_init(trash,&m->fs); r=m->fs.mkdir(m->fs.ctx,DESK_TRASH); if (r==-EEXIST) r=0;
                if (!r) r=fm_navigate(trash,DESK_TRASH);
                if (!r) r=fm_unique(trash,m->entries[index].name,"",name);
                if (!r) r=fm_entry_path(m,(unsigned)index,path);
                char target[FM_PATH]; if (!r) r=fm_join(target,DESK_TRASH,name);
                if (!r) r=m->fs.rename(m->fs.ctx,path,target);
                fm_destroy(trash); free(trash);
                if (!r) { cfg_icon_forget(&d->sh.cfg,m->entries[index].name); d->sh.want_save=1; r=fm_refresh(m); desk_folder_changed(d); }
            }
        } else if (command==DF_CUT || command==DF_COPY) { fm_select(m,(unsigned)index,0); r=fm_clipboard(m,command==DF_CUT); if (!r) r=fm_clipboard_save(m); }
        else if (command==DF_PROPERTIES) {
            char text[360], type[48]; const struct fm_entry *e=&m->entries[index];
            snprintf(text,sizeof(text),"%.180s\n\nType: %s\nLocation: C:\\Documents\\Desktop\nSize: %llu bytes",e->name,fm_type_name(e,type,sizeof(type)),(unsigned long long)e->meta.size);
            desk_shell_message(d,"Properties",text,0);
        }
    }
    if (r) error(d,r);
}
