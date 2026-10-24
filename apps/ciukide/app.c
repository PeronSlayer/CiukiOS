/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app.h"

static const struct du_menu_item project_menu[]={
    {"New Project: Hello &window...",IDE_NEW_WINDOW,0,0,0},
    {"New Project: Hello &console...",IDE_NEW_CONSOLE,0,0,0},
    {"&Open Project...",IDE_OPEN,0x18,DU_CTRL,0}, {"&Save",IDE_SAVE,0x1f,DU_CTRL,0},
    {NULL,0,0,0,DU_ITEM_SEPARATOR}, {"&Build",IDE_BUILD,0x41,0,0}, {"&Run",IDE_RUN,DU_KEY_F5,0,0},
    {"Open project.&cfg",IDE_FILES,0,0,0}};
static const struct du_menu_item edit_menu[]={
    {"&Undo",IDE_UNDO,0x2c,DU_CTRL,0}, {"Cu&t",IDE_CUT,0x2d,DU_CTRL,0},
    {"&Copy",IDE_COPY,0x2e,DU_CTRL,0}, {"&Paste",IDE_PASTE,0x2f,DU_CTRL,0}, {"Select &all",IDE_ALL,0x1e,DU_CTRL,0}};
static const struct du_menu_item help_menu[]={{"Developer &guide",IDE_GUIDE,0,0,0}};
static void failure(struct ide_app *a,int r)
{
    char text[256]; snprintf(text,sizeof(text),"%s (error %d)",strerror(r<0 ? -r : r),r<0 ? -r : r);
    du_message(a->ui,IDE_MESSAGE,"CiukIDE",text,DU_DLG_ICON_ERROR,NULL,0,0,0);
}
static int path(char *out,size_t cap,const char *folder,const char *name)
{
    int n=snprintf(out,cap,"%s/%s",folder,name); return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
static int project_folder(const char *folder)
{
    if (strncmp(folder,"/Documents/",11) || strlen(folder)>=IDE_NAME) return 0;
    const char *start=folder+11;
    for (const char *p=start;;++p) {
        unsigned char c=(unsigned char)*p;
        if (!c || c=='/') {
            size_t n=(size_t)(p-start);
            if (!n || (n==1 && *start=='.') || (n==2 && start[0]=='.' && start[1]=='.')) return 0;
            if (!c) return 1;
            start=p+1;
        } else if (c<32 || c>126 || c=='\\' || c==':') return 0;
    }
}
static int config(struct ide_app *a,const char *folder,struct ide_project *project)
{
    char full[IDE_NAME]; size_t n; int r=path(full,sizeof(full),folder,"project.cfg");
    char *data=malloc(IDE_CFG); if (!data) return -ENOMEM;
    if (!r) r=a->fs->load(a->fs->ctx,full,data,IDE_CFG,&n);
    if (!r) r=ide_project_parse(project,data,n);
    free(data); return r;
}
static uint32_t colour(void *ctx,size_t at,uint32_t fallback)
{
    struct ide_app *a=ctx;
    static const uint32_t inks[]={0,0x164e99,0xa23929,0x427542,0x814c91,0x705814};
    unsigned c=at<a->editor.len ? a->syntax[at] : 0;
    return c ? inks[c] : fallback;
}
static void highlight(struct ide_app *a)
{
    if (strcmp(a->file,"project.cfg")) ide_highlight(a->editor.text,a->editor.len,a->syntax);
    else memset(a->syntax,0,a->editor.len);
}
int ide_save(struct ide_app *a)
{
    if (!a->file[0] || !a->editor.modified) return 0;
    char full[IDE_NAME]; int r=path(full,sizeof(full),a->folder,a->file); if (r) return r;
    size_t n=cn_encoded_size(a->editor.text,a->editor.len,a->crlf);
    char *data=malloc(n ? n : 1); if (!data) return -ENOMEM;
    cn_encode(a->editor.text,a->editor.len,a->crlf,data);
    r=a->fs->save(a->fs->ctx,full,data,n); free(data);
    if (!r) a->editor.modified=0;
    return r;
}
static int open_file(struct ide_app *a,const char *name)
{
    int r=ide_save(a); if (r) return r;
    char full[IDE_NAME]; r=path(full,sizeof(full),a->folder,name); if (r) return r;
    char *raw=malloc(CN_LIMIT), *text=malloc(CN_LIMIT); size_t n,len=0; int crlf=0;
    if (!raw || !text) { free(raw); free(text); return -ENOMEM; }
    r=a->fs->load(a->fs->ctx,full,raw,CN_LIMIT,&n);
    if (!r && memchr(raw,0,n)) r=-EINVAL;
    if (!r) r=cn_decode(raw,n,text,CN_LIMIT,&len,&crlf);
    if (!r) r=du_ta_set_text(&a->editor,text,len);
    free(raw); free(text);
    if (!r) { snprintf(a->file,sizeof(a->file),"%s",name); a->crlf=crlf; highlight(a); du_focus(a->ui,IDE_EDITOR); du_invalidate_widget(a->ui,IDE_EDITOR); }
    return r;
}
static void file_list(struct ide_app *a)
{
    a->files[0].text[0]="project.cfg";
    for (unsigned i=0;i<a->model.project.source_count;++i) a->files[i+1].text[0]=a->model.project.sources[i];
    struct du_widget *w=du_get(a->ui,IDE_FILES); w->items=a->files; w->count=a->model.project.source_count+1;
    w->active=1; w->scroll=0;
}
int ide_open_project(struct ide_app *a,const char *folder)
{
    if (a->job.pid || a->model.state==IDE_LAUNCHING) return -EBUSY;
    if (!project_folder(folder)) return -EINVAL;
    int r=ide_save(a); if (r) return r;
    struct ide_project p; r=config(a,folder,&p); if (r) return r;
    char previous[IDE_NAME]; strcpy(previous,a->folder); strcpy(a->folder,folder);
    /* The previous document is saved before replacing the active project. */
    r=open_file(a,p.sources[0]);
    if (r) { strcpy(a->folder,previous); return r; }
    a->model.project=p; ide_changed(&a->model); file_list(a); ide_refresh(a); return 0;
}
void ide_adopt_project(struct ide_app *a, struct ide_app *loaded)
{
    du_ta_free(&a->editor); a->editor=loaded->editor; memset(&loaded->editor,0,sizeof(loaded->editor));
    a->editor.colour_ctx=a; a->model=loaded->model;
    memcpy(a->syntax,loaded->syntax,sizeof(a->syntax));
    strcpy(a->folder,loaded->folder); strcpy(a->file,loaded->file); a->crlf=loaded->crlf;
    file_list(a); du_invalidate_widget(a->ui,IDE_FILES); du_invalidate_widget(a->ui,IDE_EDITOR); ide_refresh(a);
}
int ide_new_project(struct ide_app *a,const char *folder,int window)
{
    if (a->job.pid || a->model.state==IDE_LAUNCHING) return -EBUSY;
    int r=ide_save(a); if (r) return r;
    if (!project_folder(folder) || strchr(folder+11,'/') || strlen(folder)>100) return -EINVAL;
    for (const char *p=folder;*p;++p) if ((unsigned char)*p<33 || (unsigned char)*p>126) return -EINVAL;
    if (mkdir(folder,0777)) return -errno; /* Never replace an existing project. */
    char *data=malloc(IDE_CFG); if (!data) { rmdir(folder); return -ENOMEM; }
    const char *names[]={"main.c","project.cfg"};
    for (unsigned i=0;i<2;++i) {
        char from[IDE_NAME],to[IDE_NAME]; size_t n;
        snprintf(from,sizeof(from),"/Documents/Developer/Samples/%s/%s",window ? "HelloWindow" : "HelloConsole",names[i]);
        r=path(to,sizeof(to),folder,names[i]);
        if (!r) r=a->fs->load(a->fs->ctx,from,data,IDE_CFG,&n);
        if (!r) r=a->fs->save(a->fs->ctx,to,data,n);
        if (r) { char old[IDE_NAME]; path(old,sizeof(old),folder,"main.c"); unlink(old); rmdir(folder); free(data); return r; }
    }
    free(data);
    return ide_open_project(a,folder);
}
/* Win98-style frame: icon toolbar, captioned panes, tabbed status bar. */
static const struct { int id; const char *icon; const char *tip; } ide_tools[]={
    {IDE_OPEN,"document-open","Open Project (Ctrl+O)"}, {IDE_SAVE,"document-save","Save (Ctrl+S)"},
    {IDE_BUILD,"applications-system","Build (F7)"}, {IDE_RUN,"media-playback-start","Run (F5)"},
    {IDE_GUIDE,"help-browser","Developer Guide"}};
static void layout(struct du_ui *u,void *ctx)
{
    (void)ctx; int w=u->canvas.width,h=u->canvas.height;
    const int top=73,bottom=h-25,strip=87,right=124,rw=w-128,half=(rw-4)/2,y0=bottom-strip;
    du_get(u,IDE_BAND1)->box=(struct du_rect){0,20,w,2};
    du_get(u,IDE_BAND2)->box=(struct du_rect){0,52,w,2};
    int x=4;
    for (unsigned i=0;i<sizeof(ide_tools)/sizeof(ide_tools[0]);++i) {
        du_get(u,ide_tools[i].id)->box=(struct du_rect){x,23,32,28}; x+=33;
        if (i==1 || i==3) { du_get(u,i==1 ? IDE_TSEP1 : IDE_TSEP2)->box=(struct du_rect){x+2,25,2,24}; x+=7; }
    }
    du_get(u,IDE_HDR_PROJECT)->box=(struct du_rect){4,56,116,17};
    du_get(u,IDE_FILES)->box=(struct du_rect){4,top,116,bottom-top};
    du_get(u,IDE_HDR_EDITOR)->box=(struct du_rect){right,56,rw,17};
    du_get(u,IDE_EDITOR)->box=(struct du_rect){right,top,rw,y0-3-top};
    du_get(u,IDE_HDR_ERRORS)->box=(struct du_rect){right,y0,half,17};
    du_get(u,IDE_ERROR_LIST)->box=(struct du_rect){right,y0+17,half,strip-17};
    du_get(u,IDE_HDR_OUTPUT)->box=(struct du_rect){right+half+4,y0,rw-half-4,17};
    du_get(u,IDE_OUTPUT)->box=(struct du_rect){right+half+4,y0+17,rw-half-4,strip-17};
    du_get(u,IDE_STATUS)->box=(struct du_rect){0,h-21,w,21};
    u->dirty=1;
}
void ide_refresh(struct ide_app *a)
{
    struct ide_model *m=&a->model; unsigned count=m->errors+m->warnings;
    for (unsigned i=0;i<count;++i) {
        struct ide_error *e=&m->error[i];
        snprintf(a->labels[i],sizeof(a->labels[i]),"%s:%u: %s%s",e->file,e->line,e->warning ? "warning: " : "",e->message);
        a->errors[i].text[0]=a->labels[i];
    }
    struct du_widget *list=du_get(a->ui,IDE_ERROR_LIST); list->items=a->errors; list->count=count;
    if (list->active>=count) list->active=0;
    static const char *states[]={"Open or create a project", "Building...", "Build succeeded - F5 to run", "Build failed", "Starting...", "Program launched"};
    unsigned line,col; du_ta_line_col(&a->editor,a->editor.cursor,&line,&col);
    /* Panels: build state, document, cursor, diagnostics (tab separated). */
    char status[DU_TEXT_MAX]; snprintf(status,sizeof(status),"%s\t%.60s%s\tLn %u, Col %u\t%u errors, %u warnings%s",
        states[m->state],a->file[0] ? a->file : "No file",a->editor.modified ? " *" : "",line,col,m->errors,m->warnings,
        m->log_truncated || m->dropped ? " (output limited)" : "");
    if (strcmp(du_get(a->ui,IDE_STATUS)->text,status)) { du_set_text(du_get(a->ui,IDE_STATUS),status); du_invalidate_widget(a->ui,IDE_STATUS); }
    int busy=m->state==IDE_BUILDING || m->state==IDE_LAUNCHING;
    du_get(a->ui,IDE_BUILD)->disabled=busy || !m->project.source_count;
    du_get(a->ui,IDE_RUN)->disabled=busy || !m->built;
    a->project_menu[5].flags=du_get(a->ui,IDE_BUILD)->disabled ? DU_ITEM_DISABLED : 0;
    a->project_menu[6].flags=du_get(a->ui,IDE_RUN)->disabled ? DU_ITEM_DISABLED : 0;
    du_invalidate_widget(a->ui,IDE_BUILD); du_invalidate_widget(a->ui,IDE_RUN);
    du_invalidate_widget(a->ui,IDE_ERROR_LIST);
}
static int build(struct ide_app *a)
{
    if (a->job.pid || a->model.state==IDE_LAUNCHING) return -EBUSY;
    ide_changed(&a->model); /* Any failed build attempt invalidates the previous output. */
    a->model.state=IDE_FAILED;
    int r=ide_save(a); if (r) return r;
    struct ide_project p; r=config(a,a->folder,&p); if (r) return r;
    a->model.project=p; file_list(a);
    char full[IDE_NAME]; r=path(full,sizeof(full),a->folder,p.output); if (r) return r;
    if (unlink(full) && errno!=ENOENT) return -errno;
    r=ide_job_start(&a->job,&a->model,a->process,a->process_ctx,"/Programs/tcc",a->folder);
    du_ta_set_text(&a->output,"",0); return r;
}
static int run(struct ide_app *a)
{
    int r=ide_run_begin(&a->model); if (r) return r;
    char full[IDE_NAME]; r=path(full,sizeof(full),a->folder,a->model.project.output);
    if (!r) r=a->launch(a->launch_ctx,full);
    if (r) ide_run_result(&a->model,r);
    return r;
}
static int jump(struct ide_app *a,unsigned index)
{
    if (index>=a->model.errors+a->model.warnings) return -EINVAL;
    struct ide_error *e=&a->model.error[index]; const char *file=e->file;
    size_t n=strlen(a->folder);
    if (!strncmp(file,a->folder,n) && file[n]=='/') file+=n+1;
    if (!ide_relative_path(file)) return -EINVAL;
    int found=0;
    for (unsigned i=0;i<a->model.project.source_count;++i) found|=!strcmp(file,a->model.project.sources[i]);
    if (!found) return -ENOENT;
    unsigned line=e->line; int r=open_file(a,file);
    if (!r && du_ta_goto_line(&a->editor,line)) r=-EINVAL;
    return r;
}
void ide_event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct ide_app *a=ctx; int r=0;
    if (e->type==DU_EV_CHANGE && e->id==IDE_EDITOR) { ide_changed(&a->model); highlight(a); }
    else if (e->type==DU_EV_DIALOG && e->index==0) {
        if (e->id==IDE_NEW_DIALOG) r=ide_new_project(a,e->text,a->template_window);
        if (e->id==IDE_OPEN_DIALOG) r=ide_open_project(a,e->text);
    } else if (e->type==DU_EV_OPEN || (e->type==DU_EV_SELECT && e->id==IDE_ERROR_LIST)) {
        if (e->id==IDE_FILES && e->index>=0 && (unsigned)e->index<du_get(u,IDE_FILES)->count)
            r=open_file(a,a->files[e->index].text[0]);
        if (e->id==IDE_ERROR_LIST && e->index>=0) r=jump(a,(unsigned)e->index);
    } else if (e->type==DU_EV_ACTIVATE) {
        size_t b,c;
        switch(e->id) {
        case IDE_NEW_WINDOW: case IDE_NEW_CONSOLE:
            a->template_window=e->id==IDE_NEW_WINDOW;
            du_input_box(u,IDE_NEW_DIALOG,"New Project", "Create a new folder under /Documents:",
                         a->template_window ? "/Documents/HelloWindow" : "/Documents/HelloConsole"); break;
        case IDE_OPEN: du_input_box(u,IDE_OPEN_DIALOG,"Open Project","Folder containing project.cfg:",
                                   a->folder[0] ? a->folder : "/Documents/Developer/Samples/HelloWindow"); break;
        case IDE_FILES: if (a->folder[0]) r=open_file(a,"project.cfg"); break;
        case IDE_SAVE: r=ide_save(a); break;
        case IDE_BUILD: r=build(a); break;
        case IDE_RUN: r=run(a); break;
        case IDE_GUIDE: r=a->launch(a->launch_ctx,"/Documents/Developer/Guide.txt"); break;
        case IDE_UNDO: du_ta_undo(&a->editor); ide_changed(&a->model); highlight(a); break;
        case IDE_COPY: case IDE_CUT:
            du_ta_selection(&a->editor,&b,&c);
            if (b<c) { char *text=malloc(c-b+1); if (!text) { r=-ENOMEM; break; }
                memcpy(text,a->editor.text+b,c-b); text[c-b]=0; du_clipboard_set(text); free(text);
                if (e->id==IDE_CUT) { du_ta_delete_selection(&a->editor); ide_changed(&a->model); highlight(a); } }
            break;
        case IDE_PASTE: { const char *text=du_clipboard_get(); if (text) {
            r=du_ta_insert(&a->editor,text,strlen(text),DU_TA_EDIT_OTHER); ide_changed(&a->model); highlight(a); } break; }
        case IDE_ALL: du_ta_select_all(&a->editor); break;
        default: break;
        }
    }
    if (r) failure(a,r);
    ide_refresh(a);
}
void ide_idle(struct ide_app *a)
{
    size_t before=a->model.log_len; int done=ide_job_poll(&a->job);
    if (a->model.log_len!=before || done) {
        du_ta_set_text(&a->output,a->model.log,a->model.log_len);
        du_ta_goto_line(&a->output,du_ta_lines(&a->output));
        du_invalidate_widget(a->ui,IDE_OUTPUT); ide_refresh(a);
    }
}
int ide_attach(struct ide_app *a,struct du_ui *u)
{
    a->ui=u; u->user=a; u->event=ide_event; u->layout=layout;
    if (du_ta_init(&a->editor,CN_LIMIT)) return -ENOMEM;
    if (du_ta_init(&a->output,IDE_LOG)) { du_ta_free(&a->editor); return -ENOMEM; }
    a->editor.line_numbers=1; a->editor.colour=colour; a->editor.colour_ctx=a;
    a->output.readonly=1; du_ta_set_wrap(&a->output,1);
    memcpy(a->project_menu,project_menu,sizeof(project_menu)); memcpy(a->edit_menu,edit_menu,sizeof(edit_menu));
    memcpy(a->help_menu,help_menu,sizeof(help_menu));
    a->menus[0]=(struct du_menu){"&Project",a->project_menu,8};
    a->menus[1]=(struct du_menu){"&Edit",a->edit_menu,5}; a->menus[2]=(struct du_menu){"&Help",a->help_menu,1};
    u->menus=a->menus; u->menu_count=3;
    du_add(u,DU_SEPARATOR,IDE_BAND1,(struct du_rect){0},""); du_add(u,DU_SEPARATOR,IDE_BAND2,(struct du_rect){0},"");
    for (unsigned i=0;i<sizeof(ide_tools)/sizeof(ide_tools[0]);++i) {
        struct du_widget *b=du_add(u,DU_TOOLBUTTON,ide_tools[i].id,(struct du_rect){0},"");
        b->icon=du_icon_argb(ide_tools[i].icon,22); b->icon_size=22;
        snprintf(b->tip,sizeof(b->tip),"%s",ide_tools[i].tip);
    }
    du_add(u,DU_SEPARATOR,IDE_TSEP1,(struct du_rect){0},""); du_add(u,DU_SEPARATOR,IDE_TSEP2,(struct du_rect){0},"");
    du_add(u,DU_HEADER,IDE_HDR_PROJECT,(struct du_rect){0},"Project"); du_add(u,DU_HEADER,IDE_HDR_EDITOR,(struct du_rect){0},"Editor");
    du_add(u,DU_HEADER,IDE_HDR_ERRORS,(struct du_rect){0},"Errors"); du_add(u,DU_HEADER,IDE_HDR_OUTPUT,(struct du_rect){0},"Output");
    du_add(u,DU_LISTVIEW,IDE_FILES,(struct du_rect){0},"")->view=DU_LIST;
    du_add(u,DU_TEXTAREA,IDE_EDITOR,(struct du_rect){0},"")->data=&a->editor;
    du_add(u,DU_LISTVIEW,IDE_ERROR_LIST,(struct du_rect){0},"")->view=DU_LIST;
    du_add(u,DU_TEXTAREA,IDE_OUTPUT,(struct du_rect){0},"")->data=&a->output;
    struct du_widget *st=du_add(u,DU_STATUS,IDE_STATUS,(struct du_rect){0},"");
    st->panels[0]=190; st->panels[1]=170; st->panels[2]=90;
    layout(u,a); du_focus(u,IDE_EDITOR); ide_refresh(a); return 0;
}
void ide_detach(struct ide_app *a) { du_ta_free(&a->editor); du_ta_free(&a->output); }
