/* SPDX-License-Identifier: MIT -- G8 controller, compiled into app.c. */
enum { D_OP_DEST=240,D_OP_LINK,D_OP_WITH,D_OP_BROWSE,D_OP_TYPES };
enum { OP_SEND_BASE=400,OP_NEW_BASE=460 };
struct files_operations {
    char **paths; unsigned count, type_index;
    int action, browse_with;
    char program[260], selected[FM_PATH];
    struct fm_filetype type;
    char targets[32][FM_PATH], labels[32][96], extensions[32][64];
    unsigned target_count, extension_count;
};
static struct files_operations *operations(struct files_app *a)
{
    if (!a->operations) a->operations=calloc(1,sizeof(*a->operations));
    return a->operations;
}
static void operations_paths_free(struct files_operations *o)
{
    for (unsigned i=0;i<o->count;i++) free(o->paths[i]);
    free(o->paths); o->paths=NULL; o->count=0;
}
static void operations_destroy(struct files_app *a)
{ if (a->operations) { operations_paths_free(a->operations); free(a->operations); a->operations=NULL; } }
static int operation_lister_emit(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct { struct du_file_entry *rows; unsigned count,capacity; int folders; } *l=ctx;
    if (!strcmp(name,".") || !strcmp(name,"..") || (l->folders && meta->type!=FM_DIR)) return 0;
    if (l->count==l->capacity) return 0;
    if (strlen(name)>=sizeof(l->rows[0].name)) return 0;
    struct du_file_entry *e=&l->rows[l->count++]; strcpy(e->name,name); e->directory=meta->type==FM_DIR; e->size=meta->size; return 0;
}
static int operation_list(void *ctx,const char *path,struct du_file_entry *out,unsigned cap,unsigned *count)
{
    struct files_app *a=ctx;
    struct { struct du_file_entry *rows; unsigned count,capacity; int folders; } l={out,0,cap,a->operations && (a->operations->action==FILES_COPY_TO || a->operations->action==FILES_MOVE_TO)};
    char *native=malloc(FM_PATH); if (!native) return -ENOMEM;
    int e=fm_posix_path(native,path);
    if (!e) e=a->model.fs.list(a->model.fs.ctx,native,FM_LIST_NAMES_ONLY,operation_lister_emit,&l);
    free(native); *count=l.count; return e;
}
static int operation_spawn_record(struct files_app *a,struct desk_launch_record *r)
{
    if (a->shell_record) return a->shell_record(a,r);
    char *argv[9]; argv[0]=(char *)fm_basename(r->target);
    int n=fm_launch_arguments(r->arguments,argv+1,7); if (n<0) return n;
    argv[n+1]=NULL; return spawn(a,r->target,argv);
}
static int operation_open(struct files_app *a,const char *path)
{
    if (a->shell_open) return a->shell_open(a,path);
    struct desk_launch_record *r=calloc(1,sizeof(*r)); if (!r) return -ENOMEM;
    int e=fm_launch_resolve(&a->model.fs,path,NULL,r);
    if (e==1) {
        char *arg[2]; int n=fm_launch_arguments(r->arguments,arg,1);
        e=n==1 ? files_navigate(a,arg[0]) : -EINVAL;
    } else if (!e) e=operation_spawn_record(a,r);
    free(r); return e;
}
static void operations_refreshed(struct files_app *a,int e)
{
    if (e<0) error_box(a,"Files","Cannot complete the file operation:",e);
    else { a->tree_dirty=1; scan_submit(a,SCAN_REFRESH,NULL,0); }
}
static int with_accept(struct du_ui *u,int button,void *ctx)
{
    struct files_app *a=ctx; struct files_operations *o=a->operations;
    if (button==1) return 0;
    if (strlen(u->dialog.items[3].text)>=sizeof(o->program)) return 1;
    strcpy(o->program,u->dialog.items[3].text);
    if (button==2) { o->browse_with=1; return 0; }
    char *native=malloc(FM_PATH); if (!native) return 1;
    int e=fm_posix_path(native,o->program); struct fm_meta meta;
    if (!e) e=a->model.fs.stat_path(a->model.fs.ctx,native,&meta);
    if (!e && meta.type!=FM_FILE) e=-EINVAL;
    if (!e && u->dialog.items[4].value) e=fm_associate(o->selected,native);
    if (!e) {
        struct desk_launch_record *r=calloc(1,sizeof(*r));
        if (!r) e=-ENOMEM;
        else {
            r->show=1; snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(o->selected));
            if (strlen(native)>=sizeof(r->target) || strlen(o->selected)+3>sizeof(r->arguments) || strchr(o->selected,'"')) e=-E2BIG;
            else { strcpy(r->target,native); snprintf(r->arguments,sizeof(r->arguments),"\"%s\"",o->selected); e=operation_spawn_record(a,r); }
            free(r);
        }
    }
    free(native);
    if (e<0) { snprintf(u->dialog.text,sizeof(u->dialog.text),"Cannot open the file or save the association: %s (%d)",strerror(-e),-e); return 1; }
    return 0;
}
static void open_with(struct files_app *a,const char *path)
{
    struct files_operations *o=operations(a); if (!o) return;
    if (path!=o->selected) snprintf(o->selected,sizeof(o->selected),"%s",path);
    if (!o->program[0]) strcpy(o->program,"/Programs/ciuknote");
    o->action=FILES_OPEN_WITH; o->browse_with=0;
    static const char *const buttons[]={"&Open","Cancel","&Browse..."};
    du_message(a->ui,D_OP_WITH,"Open With","Choose the program to open this file.",DU_DLG_ICON_NONE,buttons,3,0,1);
    struct du_dialog *d=&a->ui->dialog; int w=a->width-12<510?a->width-12:510;
    d->box=(struct du_rect){(a->width-w)/2,(a->height-210)/2,w,210};
    for (unsigned i=0;i<3;i++) d->items[i].box=(struct du_rect){d->box.x+w-260+(int)i*82,d->box.y+176,78,23};
    bin_control(a,5,DU_LABEL,"&Program (for example /Programs/ciuknote or C:\\Windows\\App.exe):",14,66,w-28,38);
    bin_control(a,3,DU_EDIT,o->program,14,108,w-28,23);
    bin_control(a,4,DU_CHECKBOX,"&Always use this program to open this type of file",14,140,w-28,24);
    d->focus=3; d->accept=with_accept; d->accept_ctx=a;
    du_invalidate(a->ui,(struct du_rect){d->box.x-4,d->box.y-4,d->box.w+12,d->box.h+12});
}
enum { FT_EXT=4,FT_DESC=5,FT_ICON=6,FT_VERB=7,FT_OPEN=8,FT_EDIT=9,FT_PRINT=10,FT_CONFIRM=11 };
static void type_show(struct files_app *a)
{
    struct fm_filetype *t=&a->operations->type; struct du_dialog *d=&a->ui->dialog;
    du_set_text(&d->items[FT_EXT],t->extension); du_set_text(&d->items[FT_DESC],t->description);
    du_set_text(&d->items[FT_ICON],t->icon); du_set_text(&d->items[FT_VERB],t->verb);
    for (unsigned i=0;i<3;i++) du_set_text(&d->items[FT_OPEN+i],t->commands[i]);
    d->items[FT_CONFIRM].value=!(t->edit_flags&0x10000u);
}
static int type_accept(struct du_ui *u,int button,void *ctx)
{
    struct files_app *a=ctx; struct files_operations *o=a->operations; struct fm_filetype *t=&o->type;
    if (button==1) return 0;
    snprintf(t->extension,sizeof(t->extension),"%.63s",u->dialog.items[FT_EXT].text);
    if (button==2 || button==3) {
        int e=0;
        if (button==3) {
            for (unsigned i=0;i<4096;i++) {
                char name[260]; e=fm_registry_enum("",o->type_index++,name,sizeof(name));
                if (e) { o->type_index=0; break; }
                if (name[0]=='.' && strlen(name)<sizeof(t->extension)) { strcpy(t->extension,name); break; }
            }
        }
        if (!e) e=fm_filetype_get(t);
        if (e) snprintf(u->dialog.text,sizeof(u->dialog.text),"Cannot read this registered type: %s (%d)",strerror(-e),-e);
        else { u->dialog.text[0]=0; type_show(a); }
        return 1;
    }
    if (!t->progid[0]) snprintf(t->progid,sizeof(t->progid),"CiukiOS%s",t->extension);
    snprintf(t->description,sizeof(t->description),"%.259s",u->dialog.items[FT_DESC].text);
    snprintf(t->icon,sizeof(t->icon),"%.259s",u->dialog.items[FT_ICON].text);
    snprintf(t->verb,sizeof(t->verb),"%.63s",u->dialog.items[FT_VERB].text);
    for (unsigned i=0;i<3;i++) snprintf(t->commands[i],sizeof(t->commands[i]),"%s",u->dialog.items[FT_OPEN+i].text);
    if (u->dialog.items[FT_CONFIRM].value) t->edit_flags&=~0x10000u; else t->edit_flags|=0x10000u;
    int e=fm_filetype_set(t);
    if (e) { snprintf(u->dialog.text,sizeof(u->dialog.text),"Cannot save this file type: %s (%d)",strerror(-e),-e); return 1; }
    return 0;
}
static void file_types(struct files_app *a)
{
    struct files_operations *o=operations(a); if (!o) return;
    memset(&o->type,0,sizeof(o->type)); strcpy(o->type.extension,".txt");
    (void)fm_filetype_get(&o->type); if (!o->type.verb[0]) strcpy(o->type.verb,"open");
    static const char *const buttons[]={"&Save","Cancel","&Load","&Next Type"};
    du_message(a->ui,D_OP_TYPES,"Folder Options - File Types","",0,buttons,4,0,1);
    struct du_dialog *d=&a->ui->dialog; int w=a->width-12<600?a->width-12:600;
    int h=a->height-12<390?a->height-12:390;
    d->box=(struct du_rect){(a->width-w)/2,(a->height-h)/2,w,h};
    for (unsigned i=0;i<4;i++) d->items[i].box=(struct du_rect){d->box.x+w-340+(int)i*82,d->box.y+h-30,78,23};
    static const char *const labels[]={"Extension:","Description:","Icon:","Default action:","Open command:","Edit command:","Print command:"};
    int y=62,step=(h-108)/8;
    for (unsigned i=0;i<7;i++) {
        bin_control(a,12+i,DU_LABEL,labels[i],12,y,112,22);
        bin_control(a,FT_EXT+i,DU_EDIT,"",128,y,w-142,22); y+=step;
    }
    bin_control(a,FT_CONFIRM,DU_CHECKBOX,"&Confirm open after download",128,y,w-142,22);
    d->focus=FT_EXT; d->accept=type_accept; d->accept_ctx=a; type_show(a);
    du_invalidate(a->ui,(struct du_rect){d->box.x-4,d->box.y-4,d->box.w+12,d->box.h+12});
}
static int sendto_emit(const char *name,const struct fm_meta *meta,void *ctx)
{
    (void)meta; struct files_app *a=ctx; struct files_operations *o=a->operations;
    if (o->target_count>=32 || name[0]=='.') return 0;
    unsigned i=o->target_count;
    if (fm_join(o->targets[i],FM_SENDTO,name)) return 0;
    snprintf(o->labels[i],sizeof(o->labels[i]),"%.95s",name);
    o->target_count++; return 0;
}
static void sendto_menu(struct files_app *a)
{
    struct files_operations *o=operations(a); if (!o) return;
    o->target_count=0; a->popup_count=0;
    static const char *const names[]={"Desktop (create shortcut)","My Documents","Floppy (A:)","Mail Recipient","Briefcase"};
    for (unsigned i=0;i<5;i++) { snprintf(o->labels[i],sizeof(o->labels[i]),"%s",names[i]); o->targets[i][0]=0; }
    o->target_count=5;
    int e=a->model.fs.mkdir(a->model.fs.ctx,FM_SENDTO);
    if (!e || e==-EEXIST) (void)a->model.fs.list(a->model.fs.ctx,FM_SENDTO,FM_LIST_NAMES_ONLY,sendto_emit,a);
    for (unsigned i=0;i<o->target_count;i++) popup_add(a,o->labels[i],OP_SEND_BASE+(int)i,0);
    du_popup(a->ui,a->popup,a->popup_count,40,60,W_LIST);
}
static int operations_send(struct files_app *a,unsigned index)
{
    struct files_operations *o=a->operations; if (!o || index>=o->target_count) return -EINVAL;
    operations_paths_free(o); int e=selection_paths(a,&o->paths,&o->count); if (e) return e;
    if (index==0) {
        struct fm_model *desktop=calloc(1,sizeof(*desktop)); if (!desktop) e=-ENOMEM;
        else { fm_init(desktop,&a->model.fs); snprintf(desktop->path,sizeof(desktop->path),"/Documents/Desktop"); e=fm_link_selection(desktop,o->paths,o->count); fm_destroy(desktop); free(desktop); }
    } else if (index==1 || index==2) e=start_job(a,FM_COPY,o->paths,o->count,index==1?"/Documents":"/mnt/a");
    else if (index==3 || index==4) {
        du_message(a->ui,D_INFO,"Send To",index==3 ? "No mail program is registered. Install and register a mail application in SendTo." : "Briefcase synchronization is not installed. Add a destination folder to SendTo to copy files there.",DU_DLG_ICON_INFO,NULL,0,0,0);
    } else {
        struct desk_launch_record *r=calloc(1,sizeof(*r)); struct fm_meta meta;
        if (!r) e=-ENOMEM;
        else {
            e=a->model.fs.stat_path(a->model.fs.ctx,o->targets[index],&meta);
            if (!e && meta.type==FM_DIR) e=start_job(a,FM_COPY,o->paths,o->count,o->targets[index]);
            else if (!e) {
                e=fm_launch_resolve(&a->model.fs,o->targets[index],NULL,r);
                if (e==1) {
                    char *args[1]; int n=fm_launch_arguments(r->arguments,args,1);
                    e=n==1?start_job(a,FM_COPY,o->paths,o->count,args[0]):-EINVAL;
                } else if (!e) {
                    size_t at=strlen(r->arguments);
                    for (unsigned i=0;i<o->count && !e;i++) {
                        size_t n=strlen(o->paths[i]);
                        if (at+n+4>sizeof(r->arguments) || strchr(o->paths[i],'"')) e=-E2BIG;
                        else { if (at) r->arguments[at++]=' '; r->arguments[at++]='"'; memcpy(r->arguments+at,o->paths[i],n); at+=n; r->arguments[at++]='"'; r->arguments[at]=0; }
                    }
                    if (!e) e=operation_spawn_record(a,r);
                }
            }
            free(r);
        }
    }
    operations_paths_free(o); return e;
}
static void registered_new(struct files_app *a)
{
    struct files_operations *o=operations(a); if (!o) return;
    o->extension_count=0; a->popup_count=0;
    for (unsigned i=0;i<4096 && o->extension_count<32;i++) {
        char ext[260],key[300],value[FM_PATH]; int e=fm_registry_enum("",i,ext,sizeof(ext)); if (e) break;
        if (ext[0]!='.' || strlen(ext)>=64) continue;
        snprintf(key,sizeof(key),"%s\\ShellNew",ext);
        if (fm_registry_get(key,"FileName",value,sizeof(value)) && fm_registry_get(key,"NullFile",value,sizeof(value))) continue;
        unsigned n=o->extension_count++; strcpy(o->extensions[n],ext);
        snprintf(o->labels[n],sizeof(o->labels[n]),"New %s document",ext+1);
        popup_add(a,o->labels[n],OP_NEW_BASE+(int)n,0);
    }
    if (a->popup_count) du_popup(a->ui,a->popup,a->popup_count,40,60,W_LIST);
    else du_message(a->ui,D_INFO,"New","No additional document templates are registered.",DU_DLG_ICON_INFO,NULL,0,0,0);
}
static int operations_undo_prepare(void *ctx)
{
    struct files_app *a=ctx; fm_job_destroy(&a->job);
    return fm_undo_start(&a->job,&a->model.fs);
}
static int operations_command(struct files_app *a,int id)
{
    if (!((id>=FILES_UNDO && id<=FILES_NEW_REGISTERED) || (id>=OP_SEND_BASE && id<OP_SEND_BASE+32) || (id>=OP_NEW_BASE && id<OP_NEW_BASE+32))) return 0;
    if (a->loading || a->job_active || du_dialog_id(a->ui)) return 1;
    struct files_operations *o=operations(a); if (!o) { error_box(a,"Files","Cannot open the file operation:",-ENOMEM); return 1; }
    if (id==FILES_TYPES) { file_types(a); return 1; }
    if (id==FILES_UNDO) {
        int e=job_begin(a); char *paths[]={FM_UNDO_FILE};
        if (!e) e=fm_job_start(&a->job,&a->model.fs,FM_MOVE,paths,1,a->model.path[0]?a->model.path:"/");
        if (!e) { a->transfer->prepare=operations_undo_prepare; a->transfer->ctx=a; job_launch(a); }
        else error_box(a,"Undo","Cannot undo the last file operation:",e);
        return 1;
    }
    if (!a->model.path[0] || fm_is_recycle(a->model.path)) return 1;
    /* A results view has no creation/paste destination. Selected-item actions
     * below use fm_entry_path and retain each result's actual parent. */
    if (a->model.searching && (id==FILES_PASTE_SHORTCUT || id==FILES_NEW_SHORTCUT ||
        id==FILES_NEW_BITMAP || id==FILES_NEW_REGISTERED ||
        (id>=OP_NEW_BASE && id<OP_NEW_BASE+32))) return 1;
    int e=0;
    if (id==FILES_SEND_TO) { sendto_menu(a); return 1; }
    if (id==FILES_NEW_REGISTERED) { registered_new(a); return 1; }
    if (id>=OP_SEND_BASE && id<OP_SEND_BASE+32) e=operations_send(a,(unsigned)(id-OP_SEND_BASE));
    else if (id>=OP_NEW_BASE && id<OP_NEW_BASE+32) {
        unsigned index=(unsigned)(id-OP_NEW_BASE); char *path=malloc(FM_PATH);
        e=!path?-ENOMEM:index>=o->extension_count?-EINVAL:fm_shellnew(&a->model,o->extensions[index],path); free(path);
    } else if (id==FILES_NEW_SHORTCUT) {
        du_input_box(a->ui,D_OP_LINK,"Create Shortcut","Type the target path (quote a program path followed by arguments):",""); return 1;
    } else if (id==FILES_NEW_BITMAP) {
        char name[FM_NAME]; e=fm_unique(&a->model,"New Bitmap Image",".bmp",name); if (!e) e=fm_bitmap(&a->model,name);
    } else if (id==FILES_COPY_TO || id==FILES_MOVE_TO) {
        operations_paths_free(o); e=selection_paths(a,&o->paths,&o->count);
        if (!e) { o->action=id; e=du_folder_dialog(a->ui,D_OP_DEST,id==FILES_COPY_TO?"Copy To Folder":"Move To Folder",a->model.path,operation_list,a); }
        if (!e) return 1;
    } else if (id==FILES_OPEN_WITH) {
        if (a->model.active<a->model.count && !fm_entry_path(&a->model,a->model.active,o->selected)) open_with(a,o->selected);
        return 1;
    } else if (id==FILES_PASTE_SHORTCUT) {
        e=fm_clipboard_load(&a->model); if (!e) e=fm_link_selection(&a->model,a->model.clipboard,a->model.clipboard_count);
    } else if (id==FILES_SHORTCUT) {
        operations_paths_free(o); e=selection_paths(a,&o->paths,&o->count);
        if (!e) e=fm_link_selection(&a->model,o->paths,o->count);
        operations_paths_free(o);
    }
    operations_refreshed(a,e); return 1;
}
static int operations_result(struct files_app *a,const struct du_event *event)
{
    if (event->id<D_OP_DEST || event->id>D_OP_TYPES) return 0;
    struct files_operations *o=a->operations; if (!o) return 1;
    int e=0;
    if (event->id==D_OP_DEST) {
        if (event->index==0) {
            char *destination=malloc(FM_PATH); e=destination?fm_posix_path(destination,event->text):-ENOMEM;
            if (!e) e=start_job(a,o->action==FILES_MOVE_TO?FM_MOVE:FM_COPY,o->paths,o->count,destination);
            free(destination);
        }
        operations_paths_free(o);
    } else if (event->id==D_OP_LINK && event->index==0) {
        struct desk_launch_record *r=calloc(1,sizeof(*r)); char *path=malloc(FM_PATH);
        if (!r || !path) e=-ENOMEM;
        else {
            e=fm_command_record(event->text,"Shortcut",r);
            if (!e) e=fm_posix_path(path,r->target);
            if (!e && strlen(path)>=sizeof(r->target)) e=-ENAMETOOLONG;
            if (!e) { strcpy(r->target,path); snprintf(r->label,sizeof(r->label),"%.63s",fm_basename(r->target)); e=fm_shortcut(&a->model,r,path); }
        }
        free(r); free(path); operations_refreshed(a,e); return 1;
    } else if (event->id==D_OP_WITH && o->browse_with) {
        o->browse_with=0; o->action=FILES_OPEN_WITH;
        e=du_file_dialog(a->ui,D_OP_BROWSE,0,"Choose a program","/Programs","",operation_list,a);
    } else if (event->id==D_OP_TYPES && event->index==0) {
        operations_refreshed(a,0);
    } else if (event->id==D_OP_BROWSE) {
        if (event->index==0) snprintf(o->program,sizeof(o->program),"%.259s",event->text);
        open_with(a,o->selected);
    }
    if (e<0) error_box(a,"Files","Cannot complete the file operation:",e);
    return 1;
}
void files_shell_action(struct files_app *a,int action,const char *path)
{
    if (a->shell_count) { a->shell_action=action; return; }
    if (path && path[0]) {
        char parent[FM_PATH]; if (!fm_parent(parent,path)) {
            /* The initial scan publishes before it replays the action. */
            snprintf(a->shell_action_path,sizeof(a->shell_action_path),"%s",path);
            a->shell_action=action;
            if (!a->loading) { fm_select_name(&a->model,fm_basename(path),0); command(a,action); a->shell_action=0; }
            return;
        }
    }
    if (a->loading) a->pending_command=action; else command(a,action);
}
