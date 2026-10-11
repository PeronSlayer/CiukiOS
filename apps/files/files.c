/* SPDX-License-Identifier: MIT */
#include "model.h"
#include "../libdeskui/deskui.h"
#include "../desktop/shell_icons.h"
#include <ciuki/spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
enum { A_BACK=1,A_FORWARD,A_UP,A_CUT,A_COPY,A_PASTE,A_DELETE,A_PROPERTIES,A_VIEWS,
       A_NEW_FOLDER,A_NEW_TEXT,A_RENAME,A_REFRESH,A_ALL,A_INVERT,A_DETAILS,A_ICONS,A_LIST,
       A_OPEN,A_NOTE,A_EXIT,A_HELP,A_ABOUT,A_ADDRESS,A_FILES,A_PLACES,A_TREE,A_SCROLL,A_SPLIT,
       A_STATUS,A_INLINE,A_SORT_NAME,A_SORT_SIZE,A_SORT_TYPE,A_SORT_MODIFIED,
       D_DELETE=100,D_REPLACE,D_ERROR,D_PROGRESS,D_PROPERTIES,D_CLOSE };
static const struct du_menu_item file_items[]={
    {"Open",A_OPEN,0x1c,0,0},{"New Folder",A_NEW_FOLDER,0,0,0},{"New Text Document",A_NEW_TEXT,0,0,0},
    {"Rename",A_RENAME,0x3c,0,0},{"Delete...",A_DELETE,0x153,0,0},{"Properties",A_PROPERTIES,0x1c,DU_ALT,0},{"Close",A_EXIT,0,0,0}};
static const struct du_menu_item edit_items[]={
    {"Cut",A_CUT,0x2d,DU_CTRL,0},{"Copy",A_COPY,0x2e,DU_CTRL,0},{"Paste",A_PASTE,0x2f,DU_CTRL,0},
    {"Select All",A_ALL,0x1e,DU_CTRL,0},{"Invert Selection",A_INVERT,0,0,0}};
static const struct du_menu_item view_items[]={
    {"Details",A_DETAILS,0,0,0},{"Large Icons",A_ICONS,0,0,0},{"List",A_LIST,0,0,0},
    {"Sort by Name",A_SORT_NAME,0,0,0},{"Sort by Size",A_SORT_SIZE,0,0,0},{"Sort by Type",A_SORT_TYPE,0,0,0},{"Sort by Modified",A_SORT_MODIFIED,0,0,0},{"Refresh",A_REFRESH,0x3f,0,0}};
static const struct du_menu_item go_items[]={
    {"Back",A_BACK,0x14b,DU_ALT,0},{"Forward",A_FORWARD,0x14d,DU_ALT,0},{"Up",A_UP,0x148,DU_ALT,0},
    {"Parent Folder",A_UP,0x0e,0,0},{"Address",A_ADDRESS,0x20,DU_ALT,0},{"Address",A_ADDRESS,0x3e,0,0}};
static const struct du_menu_item help_items[]={{"Help Topics",A_HELP,0,0,0},{"About Files",A_ABOUT,0,0,0}};
static const struct du_menu menus[]={
    {"File",file_items,sizeof(file_items)/sizeof(*file_items)},{"Edit",edit_items,sizeof(edit_items)/sizeof(*edit_items)},
    {"View",view_items,sizeof(view_items)/sizeof(*view_items)},{"Go",go_items,sizeof(go_items)/sizeof(*go_items)},
    {"Help",help_items,sizeof(help_items)/sizeof(*help_items)}};
static const struct du_menu_item item_context[]={
    {"Open",A_OPEN,0,0,0},{"Open with CiukNote",A_NOTE,0,0,0},{"Cut",A_CUT,0,0,0},{"Copy",A_COPY,0,0,0},
    {"Paste into Folder",A_PASTE,0,0,0},{"Delete...",A_DELETE,0,0,0},{"Rename",A_RENAME,0,0,0},{"Properties",A_PROPERTIES,0,0,0}};
static const struct du_menu_item background_context[]={
    {"Details",A_DETAILS,0,0,0},{"Large Icons",A_ICONS,0,0,0},{"List",A_LIST,0,0,0},
    {"Sort by Name",A_SORT_NAME,0,0,0},{"Refresh",A_REFRESH,0,0,0},{"Paste",A_PASTE,0,0,0},
    {"New Folder",A_NEW_FOLDER,0,0,0},{"New Text Document",A_NEW_TEXT,0,0,0},{"Properties",A_PROPERTIES,0,0,0}};
struct display_row { struct du_row row; char size[32],modified[32]; };
struct place { char name[FM_NAME],path[FM_PATH]; unsigned depth; };
struct files_app {
    struct du_window window; struct du_ui ui; struct fm_model model; struct fm_job job;
    struct du_row *rows; struct display_row *data;
    struct place places[29],tree[256]; struct du_row place_rows[29],tree_rows[256]; unsigned place_count,tree_count;
    uint32_t ciuki[256]; int split,rename_index,job_active,properties,test,test_stage,close_pending;
    int last_click,context_folder; uint64_t click_ns; char status[512],rename_original[FM_NAME];
};
static void update(struct files_app *);
static void error(struct files_app *a,int r)
{ char s[256]; snprintf(s,sizeof(s),"%s (error %d)",strerror(r<0?-r:r),r<0?-r:r); du_dialog_show(&a->ui,DU_MESSAGE,D_ERROR,"Files",s,NULL); }
static void test_stage(struct files_app *a,int stage)
{
    if (!a->test || stage!=a->test_stage+1) return;
    if (!du_window_ping(&a->window,0xf2310000u+(unsigned)stage)) {
        a->test_stage=stage; fprintf(stdout,"Files checkpoint=%d path=%s\n",stage,a->model.path);
    }
}
static int selected_path(struct files_app *a,char *out)
{ if (!a->model.count || a->model.active>=a->model.count) return -ENOENT; return fm_join(out,a->model.path,a->model.entries[a->model.active].name); }
static int navigate(struct files_app *a,const char *path)
{
    int r=fm_navigate(&a->model,path); if (r) { error(a,r); return r; }
    du_get(&a->ui,A_FILES)->first=0; a->rename_index=-1; du_get(&a->ui,A_INLINE)->hidden=1;
    a->status[0]=0; update(a);
    if (!strcmp(a->model.path,"/system")) test_stage(a,1); return 0;
}
static void inline_rename(struct files_app *a)
{
    if (!a->model.count) return;
    unsigned index=a->model.active; struct du_widget *list=du_get(&a->ui,A_FILES),*edit=du_get(&a->ui,A_INLINE);
    a->rename_index=(int)index; strcpy(a->rename_original,a->model.entries[index].name);
    strcpy(edit->text,a->rename_original); edit->cursor=strlen(edit->text); edit->anchor=0; edit->offset=0;
    int x=list->box.x+22,y=list->box.y+22+(int)(index-list->first)*20,width=list->box.w*43/100-24;
    if (list->view==DU_LIST_MODE) { y=list->box.y+(int)(index-list->first)*20; width=list->box.w-24; }
    if (list->view==DU_ICONS) { unsigned cols=(unsigned)list->box.w/96; if (!cols) cols=1;
        x=list->box.x+(int)((index-list->first)%cols)*96; y=list->box.y+(int)((index-list->first)/cols)*64+38; width=96; }
    edit->box=(struct du_rect){x,y,width,24}; edit->hidden=0; du_focus(&a->ui,A_INLINE); a->ui.dirty=1;
}
static int tree_add(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct files_app *a=ctx; if (meta->type!=FM_DIR || a->tree_count==256) return 0;
    struct place *p=&a->tree[a->tree_count];
    /* The last path saved in status is only a temporary enumeration context. */
    if (fm_join(p->path,a->status,name)) return 0;
    strcpy(p->name,name); unsigned depth=0; for (const char *s=p->path;*s;s++) depth+=*s=='/'; p->depth=depth;
    a->tree_rows[a->tree_count]=(struct du_row){p->name,"","","",desk_icon_folder,!strcmp(p->path,a->model.path),p->depth,fm_beneath(a->model.path,p->path)};
    a->tree_count++; return 0;
}
static int place_add(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct files_app *a=ctx; if (meta->type!=FM_DIR || a->place_count==29) return 0;
    struct place *p=&a->places[a->place_count]; if (fm_join(p->path,"/mnt",name)) return 0;
    snprintf(p->name,sizeof(p->name),"Volume %s",name);
    a->place_rows[a->place_count]=(struct du_row){p->name,"","","",desk_icon_folder,0,0,0}; a->place_count++; return 0;
}
static void panes(struct files_app *a)
{
    const char *names[]={"CiukiOS","Applications","System"},*paths[]={"/","/bin","/system"};
    a->place_count=3;
    for (unsigned i=0;i<3;i++) {
        strcpy(a->places[i].name,names[i]); strcpy(a->places[i].path,paths[i]);
        a->place_rows[i]=(struct du_row){a->places[i].name,"","","",i?desk_icon_folder:a->ciuki,!strcmp(paths[i],a->model.path),0,0};
    }
    a->model.fs.list(a->model.fs.ctx,"/mnt",place_add,a);
    struct du_widget *places=du_get(&a->ui,A_PLACES); places->rows=a->place_rows; places->count=a->place_count;
    a->tree_count=1; strcpy(a->tree[0].name,"CiukiOS"); strcpy(a->tree[0].path,"/"); a->tree[0].depth=0;
    a->tree_rows[0]=(struct du_row){a->tree[0].name,"","","",a->ciuki,!strcmp(a->model.path,"/"),0,1};
    char saved[512]; strcpy(saved,a->status); strcpy(a->status,"/");
    a->model.fs.list(a->model.fs.ctx,"/",tree_add,a);
    for (unsigned i=1;i<a->tree_count && a->tree_count<256;i++) if (fm_beneath(a->model.path,a->tree[i].path)) {
        if (strlen(a->tree[i].path)>=sizeof(a->status)) continue;
        strcpy(a->status,a->tree[i].path); a->model.fs.list(a->model.fs.ctx,a->tree[i].path,tree_add,a);
    }
    strcpy(a->status,saved); struct du_widget *tree=du_get(&a->ui,A_TREE); tree->rows=a->tree_rows; tree->count=a->tree_count;
}
static void layout(struct files_app *a)
{
    int width=(int)a->ui.canvas.width,height=(int)a->ui.canvas.height;
    if (a->split<110) a->split=110; if (a->split>width-220) a->split=width-220;
    du_get(&a->ui,A_ADDRESS)->box=(struct du_rect){68,58,width-72,28};
    du_get(&a->ui,A_PLACES)->box=(struct du_rect){4,108,a->split-8,80};
    du_get(&a->ui,A_TREE)->box=(struct du_rect){4,208,a->split-8,height-236};
    du_get(&a->ui,A_SPLIT)->box=(struct du_rect){a->split,90,5,height-118};
    du_get(&a->ui,A_FILES)->box=(struct du_rect){a->split+6,90,width-a->split-26,height-118};
    du_get(&a->ui,A_SCROLL)->box=(struct du_rect){width-18,90,14,height-118};
    du_get(&a->ui,A_STATUS)->box=(struct du_rect){2,height-26,width-4,24};
    a->ui.dirty=1;
}
static void update(struct files_app *a)
{
    free(a->rows); free(a->data); a->rows=NULL; a->data=NULL;
    if (a->model.count) {
        a->rows=calloc(a->model.count,sizeof(*a->rows)); a->data=calloc(a->model.count,sizeof(*a->data));
        if (!a->rows || !a->data) { free(a->rows); free(a->data); a->rows=NULL; a->data=NULL; error(a,-ENOMEM); }
    }
    unsigned selected=0; uint64_t bytes=0;
    for (unsigned i=0;i<a->model.count;i++) {
        struct fm_entry *e=&a->model.entries[i]; selected+=e->selected; if (e->selected) bytes+=e->meta.size;
        if (!a->data) continue;
        struct display_row *r=&a->data[i]; snprintf(r->size,sizeof(r->size),"%llu",(unsigned long long)e->meta.size);
        time_t timestamp=(time_t)e->meta.modified; struct tm tm;
        if (gmtime_r(&timestamp,&tm)) strftime(r->modified,sizeof(r->modified),"%Y-%m-%d",&tm);
        r->row=(struct du_row){e->name,e->meta.type==FM_DIR?"":r->size,fm_type_name(e),r->modified,e->meta.type==FM_DIR?desk_icon_folder:!strcmp(fm_type_name(e),"Text document")?desk_icon_text:desk_icon_run,e->selected,0,0};
        a->rows[i]=r->row;
    }
    struct du_widget *list=du_get(&a->ui,A_FILES); list->rows=a->rows; list->count=a->rows?a->model.count:0; list->active=a->model.active;
    if (list->first>=list->count) list->first=0;
    struct du_widget *scroll=du_get(&a->ui,A_SCROLL); scroll->total=list->count; scroll->page=(unsigned)(list->box.h-22)/20; scroll->value=list->first;
    snprintf(du_get(&a->ui,A_ADDRESS)->text,DU_TEXT_MAX,"%s",a->model.path);
    struct du_widget *address=du_get(&a->ui,A_ADDRESS); address->cursor=address->anchor=strlen(address->text); address->offset=0;
    snprintf(du_get(&a->ui,A_STATUS)->text,DU_TEXT_MAX,"%u items | %u selected, %llu bytes%s%.400s",a->model.count,selected,(unsigned long long)bytes,*a->status?" | ":"",a->status);
    du_get(&a->ui,A_BACK)->disabled=!a->model.history_at;
    du_get(&a->ui,A_FORWARD)->disabled=a->model.history_at+1>=a->model.history_count;
    du_get(&a->ui,A_PASTE)->disabled=!a->model.clipboard_count;
    panes(a); a->ui.dirty=1;
}
static void open_item(struct files_app *a,int note)
{
    char path[FM_PATH]; int r=selected_path(a,path); if (r) { error(a,r); return; }
    struct fm_entry *e=&a->model.entries[a->model.active];
    if (e->meta.type==FM_DIR && !note) { navigate(a,path); return; }
    const char *program=path; int text=note || !strcmp(fm_type_name(e),"Text document");
    if (!text) { FILE *f=fopen(path,"rb"); unsigned char magic[4]; size_t n=f?fread(magic,1,4,f):0; if (f) fclose(f);
        if (n!=4 || memcmp(magic,"\177ELF",4)) text=1; }
    if (text) {
        struct stat st; if (stat("/bin/ciuknote",&st)) { error(a,-ENOENT); return; } program="/bin/ciuknote";
    }
    char *argv[]={(char *)program,text?path:NULL,NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","HOME=/home","TMPDIR=/tmp","PATH=/bin",NULL};
    if (ciuki_spawn(program,argv,env,NULL,0,0)<0) error(a,-errno);
}
static int start_job(struct files_app *a,enum fm_job_kind kind,char *const *paths,unsigned n,const char *target)
{
    if (a->job_active) return -EBUSY;
    fm_job_destroy(&a->job); int r=fm_job_start(&a->job,&a->model.fs,kind,paths,n,target); if (r) return r;
    a->job_active=1; a->properties=kind==FM_PROPERTIES;
    du_dialog_show(&a->ui,DU_JOB,D_PROGRESS,a->properties?"Properties":kind==FM_DELETE?"Deleting":kind==FM_MOVE?"Moving":"Copying","Preparing...",NULL); return 0;
}
static int selection_job(struct files_app *a,enum fm_job_kind kind)
{
    int r=fm_clipboard(&a->model,0); if (r) return r;
    return start_job(a,kind,a->model.clipboard,a->model.clipboard_count,NULL);
}
static void job_tick(struct files_app *a)
{
    if (!a->job_active) return;
    if (a->job.state==FM_RUNNING) fm_job_step(&a->job);
    struct fm_job *j=&a->job;
    if (j->state==FM_REPLACE && a->ui.dialog.id!=D_REPLACE) {
        du_dialog_show(&a->ui,DU_MESSAGE,D_REPLACE,"Replace file?",fm_basename(j->frames[j->depth-1].target),NULL);
        const char *buttons[]={"Yes","Yes to All","No","Cancel"}; du_dialog_buttons(&a->ui,buttons,4); return;
    }
    if (j->state==FM_RUNNING) {
        if (!a->ui.dialog.visible || a->ui.dialog.id!=D_PROGRESS) du_dialog_show(&a->ui,DU_JOB,D_PROGRESS,a->properties?"Properties":"File operation","",NULL);
        snprintf(a->ui.dialog.message,sizeof(a->ui.dialog.message),"%u of %u items; %llu bytes\n%.180s",j->completed,j->count,(unsigned long long)j->bytes,j->depth?fm_basename(j->frames[j->depth-1].source):"");
        a->ui.dialog.progress=j->completed; a->ui.dialog.total=j->count; a->ui.dirty=1; return;
    }
    if (j->state==FM_REPLACE) return;
    a->job_active=0; a->ui.dialog.visible=0;
    snprintf(a->status,sizeof(a->status),"%u completed, %u skipped%s",j->completed,j->skipped,j->metadata_warnings?"; copy metadata could not be preserved":"");
    if (j->state==FM_CANCELLED) snprintf(a->status,sizeof(a->status),"Cancelled; %u completed items retained",j->completed);
    if (j->state==FM_FAILED) { error(a,-j->error); if (j->cleanup_error) snprintf(a->status,sizeof(a->status),"Cleanup failed: %s",strerror(j->cleanup_error)); }
    else if (a->properties) {
        char message[512]; struct fm_meta m={0}; const char *path=j->sources[0]; a->model.fs.stat_path(a->model.fs.ctx,path,&m);
        snprintf(message,sizeof(message),"Type: %s; attributes: %s\nLocation: %.160s\n%llu bytes; %llu files, %llu folders",m.type==FM_DIR?"Folder":"File",m.mode&0200?"writable":"read-only",path,(unsigned long long)j->bytes,(unsigned long long)j->files,(unsigned long long)j->folders);
        du_dialog_show(&a->ui,DU_MESSAGE,D_PROPERTIES,"Properties",message,NULL);
    } else {
        int r=fm_refresh(&a->model); if (r) error(a,r);
        if (j->state==FM_DONE && !j->skipped) {
            if (j->kind==FM_COPY) test_stage(a,4);
            if (j->kind==FM_DELETE) test_stage(a,5);
            if (j->kind==FM_MOVE) { a->model.cut=0; }
        }
    }
    update(a);
    if (a->close_pending) du_window_close(&a->window);
}
static void event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct files_app *a=ctx; int r=0,id=e->id;
    if (e->type==DU_TICK) { job_tick(a); return; }
    if (e->type==DU_CONTEXT) {
        a->context_folder=0;
        if (id==A_FILES && e->index>=0) {
            if (!a->model.entries[e->index].selected) fm_select(&a->model,(unsigned)e->index,0);
            a->model.active=(unsigned)e->index; a->context_folder=a->model.entries[e->index].meta.type==FM_DIR;
            update(a); du_context(u,item_context,sizeof(item_context)/sizeof(*item_context),e->x,e->y);
        } else du_context(u,background_context,sizeof(background_context)/sizeof(*background_context),e->x,e->y);
        return;
    }
    if (e->type==DU_SORT) { fm_sort(&a->model,(unsigned)e->index,a->model.sort==(unsigned)e->index?!a->model.descending:0); update(a); return; }
    if (e->type==DU_SELECT) {
        if (id==A_PLACES && e->index>=0) navigate(a,a->places[e->index].path);
        else if (id==A_TREE && e->index>=0) navigate(a,a->tree[e->index].path);
        else if (id==A_FILES && e->index>=0) {
            uint64_t now=du_now(); int double_click=!(e->modifiers&(DU_CTRL|DU_SHIFT)) && a->last_click==e->index && now-a->click_ns<400000000u;
            fm_select(&a->model,(unsigned)e->index,e->modifiers); a->last_click=e->index; a->click_ns=now;
            update(a); if (double_click && u->buttons) open_item(a,0);
        } else if (id==A_FILES && !(e->modifiers&(DU_CTRL|DU_SHIFT))) { for (unsigned i=0;i<a->model.count;i++) a->model.entries[i].selected=0; update(a); }
        return;
    }
    if (e->type==DU_CHANGE) {
        if (id==A_SCROLL) { du_get(u,A_FILES)->first=(unsigned)e->index; u->dirty=1; }
        else if (id==A_SPLIT) { a->split=e->index; layout(a); }
        else if (id==-1) { fm_find(&a->model,(unsigned)e->index,du_now()); update(a); }
        return;
    }
    if (e->type==DU_CLOSE && id==A_INLINE) { du_get(u,A_INLINE)->hidden=1; a->rename_index=-1; du_focus(u,A_FILES); return; }
    if (e->type!=DU_ACTIVATE) return;
    if (id==D_REPLACE) { fm_job_answer(&a->job,(enum fm_replace)e->index); return; }
    if (id==D_PROGRESS) { fm_job_cancel(&a->job); return; }
    if (id==D_DELETE) { if (!e->index) { r=selection_job(a,FM_DELETE); if (r) error(a,r); } return; }
    if (id==D_CLOSE) { if (!e->index) { a->close_pending=1; fm_job_cancel(&a->job); } return; }
    if (id>=100) return;
    if (a->job_active) { error(a,-EBUSY); return; }
    switch (id) {
    case A_BACK: r=fm_history(&a->model,-1); break;
    case A_FORWARD: r=fm_history(&a->model,1); break;
    case A_UP: r=fm_up(&a->model); break;
    case A_ADDRESS:
        if (u->focus>=0 && u->widgets[u->focus].id==A_ADDRESS) { navigate(a,du_get(u,A_ADDRESS)->text); du_focus(u,A_FILES); return; }
        du_focus(u,A_ADDRESS); du_get(u,A_ADDRESS)->anchor=0; du_get(u,A_ADDRESS)->cursor=strlen(du_get(u,A_ADDRESS)->text); return;
    case A_FILES: case A_OPEN: open_item(a,0); return;
    case A_NOTE: open_item(a,1); return;
    case A_ALL: fm_select_all(&a->model,0); break;
    case A_INVERT: fm_select_all(&a->model,1); break;
    case A_CUT: case A_COPY: r=fm_clipboard(&a->model,id==A_CUT); break;
    case A_PASTE: {
        char target[FM_PATH]; strcpy(target,a->model.path); if (a->context_folder) { r=selected_path(a,target); a->context_folder=0; }
        if (!r) r=start_job(a,a->model.cut?FM_MOVE:FM_COPY,a->model.clipboard,a->model.clipboard_count,target); if (r) error(a,r); return;
    }
    case A_DELETE:
        du_dialog_show(u,DU_MESSAGE,D_DELETE,"Permanently delete?","Selected files and folders will be removed.",NULL);
        { const char *buttons[]={"Delete","Cancel"}; du_dialog_buttons(u,buttons,2); } return;
    case A_PROPERTIES: {
        char path[FM_PATH]; strcpy(path,a->model.path);
        unsigned n=0; for (unsigned i=0;i<a->model.count;i++) n+=a->model.entries[i].selected;
        if (n) r=selection_job(a,FM_PROPERTIES); else { char *paths[]={path}; r=start_job(a,FM_PROPERTIES,paths,1,NULL); }
        if (r) error(a,r); return;
    }
    case A_NEW_FOLDER: case A_NEW_TEXT: {
        const char *base=id==A_NEW_FOLDER?"New Folder":"New Text Document.txt"; char name[FM_NAME]; strcpy(name,base);
        for (unsigned n=1;n<100;n++) { r=fm_create(&a->model,name,id==A_NEW_FOLDER); if (r!=-EEXIST) break; snprintf(name,sizeof(name),"%s (%u)",base,n); }
        if (r) error(a,r); else { update(a); inline_rename(a); if (id==A_NEW_FOLDER) test_stage(a,2); } return;
    }
    case A_RENAME: inline_rename(a); return;
    case A_INLINE:
        if (a->rename_index>=0) r=fm_rename(&a->model,(unsigned)a->rename_index,du_get(u,A_INLINE)->text);
        if (r) { error(a,r); return; } du_get(u,A_INLINE)->hidden=1; a->rename_index=-1; du_focus(u,A_FILES); test_stage(a,3); break;
    case A_REFRESH: r=fm_refresh(&a->model); break;
    case A_DETAILS: case A_ICONS: case A_LIST: du_get(u,A_FILES)->view=id==A_DETAILS?DU_DETAILS:id==A_ICONS?DU_ICONS:DU_LIST_MODE; du_get(u,A_FILES)->first=0; break;
    case A_VIEWS: du_get(u,A_FILES)->view=(enum du_view)((du_get(u,A_FILES)->view+1)%3); break;
    case A_SORT_NAME: case A_SORT_SIZE: case A_SORT_TYPE: case A_SORT_MODIFIED: fm_sort(&a->model,(unsigned)(id-A_SORT_NAME),0); break;
    case A_HELP: du_dialog_show(u,DU_MESSAGE,D_ERROR,"Files Help","Enter opens. F2 renames. F5 refreshes.\nCtrl+C/X/V copies, cuts and pastes.\nDelete is permanent. Alt+D edits address.\nCopy metadata preservation is unavailable.",NULL); return;
    case A_ABOUT: du_dialog_show(u,DU_MESSAGE,D_ERROR,"About Files","CiukiOS Files - A modern Retro OS\nTango 0.8.90 icons: Public Domain.\nCiuki portrait: separate project asset.\nlibdeskui and Files code: MIT.",NULL); return;
    case A_EXIT: du_window_close(&a->window); return;
    }
    if (r) error(a,r); update(a);
}
static int close_request(struct du_window *w,void *ctx)
{
    (void)w; struct files_app *a=ctx;
    if (!a->job_active) return 1;
    du_dialog_show(&a->ui,DU_MESSAGE,D_CLOSE,"Close Files?","Cancel the current operation and close?",NULL);
    const char *buttons[]={"Close","Continue"}; du_dialog_buttons(&a->ui,buttons,2); return 0;
}
static void load_ciuki(struct files_app *a)
{
    FILE *f=fopen("/system/assets/ciuki-portrait.xrgb","rb");
    if (!f) return;
    for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) {
        uint32_t p; if (fseek(f,(long)((y*16*256+x*16)*4),SEEK_SET) || fread(&p,4,1,f)!=1) { fclose(f); return; }
        a->ciuki[y*16+x]=p|0xff000000u;
    }
    fclose(f);
}
int main(int argc,char **argv)
{
    int channel=3,test=0; const char *path="/";
    for (int i=1;i<argc;i++) {
        if (!strncmp(argv[i],"--channel-fd=",13)) { char *end; long n=strtol(argv[i]+13,&end,10); if (!argv[i][13] || *end || n<3 || n>=128) return 2; channel=(int)n; }
        else if (!strcmp(argv[i],"--test=files")) test=1;
        else if (argv[i][0]=='/') path=argv[i]; else return 2;
    }
    struct files_app *a=calloc(1,sizeof(*a)); if (!a) return 1;
    a->job.input=a->job.output=-1; a->rename_index=a->last_click=-1; a->split=142; a->test=test;
    fm_init(&a->model,fm_native_vfs());
    if (du_window_open(&a->window,&a->ui,channel,610,350,"Files")) { fprintf(stderr,"Files: desktop connection failed: %s\n",strerror(errno)); free(a); return 1; }
    a->window.close_request=close_request; a->window.user=a; a->ui.event=event; a->ui.user=a; a->ui.menus=menus; a->ui.menu_count=5;
    const char *labels[]={"","","","","","","","","Views"};
    const uint32_t *icons[]={desk_icon_back,desk_icon_forward,desk_icon_up,desk_icon_cut,desk_icon_copy,desk_icon_paste,desk_icon_delete,desk_icon_properties,desk_icon_views};
    for (unsigned i=0;i<9;i++) { struct du_widget *w=du_add(&a->ui,DU_BUTTON,A_BACK+(int)i,(struct du_rect){4+(int)i*38,26,i==8?84:34,28},labels[i]); w->icon=icons[i]; }
    du_add(&a->ui,DU_LABEL,80,(struct du_rect){6,64,62,20},"Address");
    du_add(&a->ui,DU_EDIT,A_ADDRESS,(struct du_rect){0},"/");
    du_add(&a->ui,DU_LABEL,81,(struct du_rect){6,90,100,18},"Places");
    du_add(&a->ui,DU_LIST,A_PLACES,(struct du_rect){0},"")->view=DU_LIST_MODE;
    du_add(&a->ui,DU_LABEL,82,(struct du_rect){6,188,100,18},"Folders");
    du_add(&a->ui,DU_TREE,A_TREE,(struct du_rect){0},"")->view=DU_LIST_MODE;
    du_add(&a->ui,DU_SPLITTER,A_SPLIT,(struct du_rect){0},"");
    du_add(&a->ui,DU_LIST,A_FILES,(struct du_rect){0},"");
    du_add(&a->ui,DU_SCROLLBAR,A_SCROLL,(struct du_rect){0},"");
    du_add(&a->ui,DU_STATUS,A_STATUS,(struct du_rect){0},"");
    du_add(&a->ui,DU_EDIT,A_INLINE,(struct du_rect){0},"")->hidden=1;
    layout(a); load_ciuki(a); navigate(a,path); du_focus(&a->ui,A_FILES);
    du_timer_start(&a->ui,1,du_now(),10000000u);
    if (test) du_window_move(&a->window,4,40);
    int r=1;
    while ((r=du_window_turn(&a->window,du_now()))>0) {
        while (waitpid(-1,NULL,WNOHANG)>0) { }
        struct timespec pause={.tv_sec=0,.tv_nsec=10000000}; nanosleep(&pause,NULL);
    }
    if (a->job_active) { fm_job_cancel(&a->job); fm_job_step(&a->job); }
    fm_job_destroy(&a->job); fm_destroy(&a->model); free(a->rows); free(a->data); du_window_destroy(&a->window); free(a);
    return r<0?1:0;
}
