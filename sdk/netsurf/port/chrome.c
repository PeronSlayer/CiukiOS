/* SPDX-License-Identifier: GPL-2.0-only
 * Single UI owner: deskui chrome/dialogs; fbtk owns the content viewport. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libnsfb.h>
#include <libnsfb_event.h>
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "utils/file.h"
#include "netsurf/browser_window.h"
#include "netsurf/download.h"
#include "desktop/download.h"
#include "download_store.h"
#include "apps/control/internet_storage.h"
#include "apps/control/profile_lock.h"
#include "netsurf/content.h"
#include "content/hlcache.h"
#include "content/content.h"
#include "desktop/browser_history.h"
#include "desktop/hotlist.h"
#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/framebuffer.h"
#include "apps/libdeskui/deskui.h"
#include "apps/libdeskui/internal.h"
#include "apps/libdeskui/keymap.h"
#include "apps/libprint/dialog.h"
#include "printing.h"
#include "apps/desktop/browser_launch.h"
#include "surface.h"
void ciuki_font_prefetch(void); /* font.c (fx-15) */
#include "profile.h"
#include "js_memory.h"
#include "chrome.h"
#include "budget.h"
#include "logline.h"
#include "pointer.h"

enum { BACK=1,FORWARD,RELOAD,STOP,BOOKMARK,URL,STATUS,HOME,WALLPAPER,NEW_WINDOW=10,OPEN=20,FILE_PICKER,NOTICE,SAVE_DOWNLOAD,DOWNLOAD_PROGRESS,SAVED_DOWNLOAD };
static const int buttons[]={BACK,FORWARD,RELOAD,STOP,HOME,BOOKMARK,NEW_WINDOW,WALLPAPER};
static struct du_ui ui;
static struct cp_ui printing;
static struct ciuki_browser_surface *surface;
static bool active, loading, hotlist_ready, profile_ready, toolbar_keys;
static bool status_dirty;
static bool profile_requested;
static struct cp_internet_options browser_internet;
static char saved_download[128];
static uint32_t launch_request;
static unsigned input_records;
static unsigned bookmarks;
struct gui_download_window { struct download_context *context; struct ciuki_download_store store; bool failed;uint64_t total; };
static struct gui_download_window *downloads[4], *saving_download,*progress_download;
static void downloads_poll(void);
static void downloads_finish(void);
static void download_release(struct gui_download_window *);
static unsigned content_cursor=DESK_CURSOR_ARROW;
static struct form_control *file_gadget;
static struct gui_window *file_window;
const char *ciuki_resource_path="/system/netsurf";
const char *ciuki_capture_path;
/* f6-22: C:\SYSTEM\LOGS\NETSURF.LOG keeps one line per browser exit. Host
 * runs write it only when --ciuki-log= names a file. */
#ifdef CIUKI_HOST
const char *ciuki_netsurf_log_path;
#else
const char *ciuki_netsurf_log_path="/system/logs/NETSURF.LOG";
#endif
#define CIUKI_NETSURF_LOG_MAX (64u*1024u)
static const char *profile_path="/Documents/NetSurf";
#ifndef CIUKI_HOST
/* sec-user-05 (user-security-contract.md 8.2, A1-10): in a profile session
 * the browser's choices, bookmarks and history live in the user's
 * Application Data and its exit/fetch log in Local Settings\Application
 * Data, both below the committed profile root the kernel launch generated
 * (USERPROFILE). A profile-less bootstrap desktop keeps the former shared
 * locations. Missing folders are created inside the profile. */
static char user_profile_path[160],user_log_path[200];
static bool ensure_tree(char *path)
{
    for(char *p=path+1;;p++){
        if(*p && *p!='/')continue;
        char c=*p;*p=0;struct stat st;
        bool ok=!stat(path,&st)?S_ISDIR(st.st_mode):errno==ENOENT && (!mkdir(path,0700) || errno==EEXIST);
        *p=c;if(!ok)return false;if(!c)return true;
    }
}
static void select_user_paths(void)
{
    const char *root=getenv("USERPROFILE");
    if(!root || strncmp(root,"/Windows/Profiles/",18) || !root[18] || strlen(root)>=96 || strstr(root,"/.."))return;
    char log_dir[180];
    snprintf(user_profile_path,sizeof(user_profile_path),"%s/Application Data/NetSurf",root);
    snprintf(log_dir,sizeof(log_dir),"%s/Local Settings/Application Data/NetSurf",root);
    snprintf(user_log_path,sizeof(user_log_path),"%s/NETSURF.LOG",log_dir);
    profile_path=user_profile_path;
    ciuki_netsurf_log_path=ensure_tree(log_dir) ? user_log_path : NULL;
    (void)ensure_tree(user_profile_path);
}
#endif
const char *ciuki_chrome_choices(void)
{
    static char path[560];snprintf(path,sizeof(path),"%s/Choices",profile_path);
    struct stat st;
    if(!stat(path,&st) && (st.st_size<0 || st.st_size>65536))return "/system/netsurf/Choices";
    return path;
}
extern fbtk_widget_t *fbtk;
#ifdef CIUKI_HOST
struct ciuki_browser_surface *ciuki_browser_active;
int ciuki_browser_channel=-1;
#endif
static struct browser_window *browser(void) { return window_list?window_list->bw:NULL; }
static nserror count_bookmark(void *context,nsurl *url,const char *title)
{ (void)context;(void)url;(void)title;bookmarks++;return bookmarks>500?NSERROR_BAD_SIZE:NSERROR_OK; }
size_t fetch_ciuki_describe(const nsurl *,char *,size_t);
static void navigate(const char *text)
{
    nsurl *url=NULL;
    nserror err=nsurl_create(text,&url);
    if(!err && browser())err=browser_window_navigate(browser(),url,NULL,BW_NAVIGATE_HISTORY,NULL,NULL,NULL);
    if(err) {
        /* No address text (privacy, as the input records); the nserror tells
         * an unparsable address from a missing fetcher (NO_FETCH_HANDLER) or
         * a refused fetch setup (NOMEM) when no fetch record follows, and the
         * fetcher table (patch 0013) why a scheme has no fetcher. */
        ciuki_log_line("[netsurf] navigate error=%d parsed=%d window=%d",(int)err,url!=NULL,browser()!=NULL);
        if(url && err==NSERROR_NO_FETCH_HANDLER) {
            char table[CIUKI_LOG_LINE_MAX-32]="";
            fetch_ciuki_describe(url,table,sizeof(table));
            ciuki_log_line("[netsurf] fetchers %s",table);
        }
        ciuki_chrome_status("Unable to open this address.");
    }
    if(url)nsurl_unref(url);
}
static int save_hotlist(const char *path,void *unused)
{ (void)unused;return hotlist_export(path,"NetSurf Bookmarks")==NSERROR_OK?0:-1; }
static int save_choices(const char *path,void *unused)
{ (void)unused;return nsoption_write(path,nsoptions,nsoptions_default)==NSERROR_OK?0:-1; }
bool ciuki_chrome_new_window(struct nsurl *url)
{
    if(!active || !url)return false;
    const char *address=nsurl_access(url);size_t n=strlen(address);
    if(n>DESK_ARG_MAX) { ciuki_chrome_status("The address is too long for a new window.");return false; }
    uint8_t payload[4+DESK_ARG_MAX];desk_put32(payload,(uint32_t)n);memcpy(payload+4,address,n);
    launch_request=du3_request(&surface->connection,DESK_LAUNCH,surface->window,surface->generation,payload,4+n,-1);
    if(!launch_request) {
        ciuki_chrome_status("Unable to request a new browser window.");return false;
    }
    ciuki_chrome_status("Opening a new browser window...");return true;
}
void ciuki_chrome_launch_result(const struct desk_packet *p)
{
    if(p->request==launch_request && (p->index==DESK_LAUNCH || p->index==DESK_V3_LAUNCH)) {
        launch_request=0;
        if(p->result)ciuki_chrome_status("The desktop could not open the requested window.");
    }
}
static bool open_download(int with)
{
    char argument[160];
    if(with)snprintf(argument,sizeof(argument),"--file-action=%d:%s",FILES_OPEN_WITH,saved_download);
    else snprintf(argument,sizeof(argument),"--open=%s",saved_download);
    if(!desk_browser_file_argument(argument)) { ciuki_chrome_status("The saved file path cannot be opened.");return false; }
    unsigned n=strlen(argument);uint8_t payload[240];
    desk_put32(payload,5);memcpy(payload+4,"files",5);desk_put32(payload+9,n);memcpy(payload+13,argument,n);
    launch_request=du3_request(&surface->connection,DESK_V3_LAUNCH,surface->window,surface->generation,payload,13+n,-1);
    ciuki_chrome_status(launch_request?"Opening the saved file in Files...":"Unable to request Files.");return launch_request!=0;
}
static void action(int id)
{
    struct browser_window *bw=browser();if(!bw)return;
    switch(id) {
    case BACK: if(browser_window_back_available(bw))browser_window_history_back(bw,false);break;
    case FORWARD:if(browser_window_forward_available(bw))browser_window_history_forward(bw,false);break;
    case RELOAD:ciuki_chrome_reload();break;
    case STOP:browser_window_stop(bw);ciuki_chrome_loading(false);break;
    case HOME:navigate(nsoption_charp(homepage_url)?nsoption_charp(homepage_url):"about:welcome");break;
    case NEW_WINDOW:ciuki_chrome_new_window(browser_window_access_url(bw));break;
    case BOOKMARK: {
        if(!profile_ready) { ciuki_chrome_status("Profile unavailable.");break; }
        struct internet_profile_lock lock;
        if(ciuki_profile_transaction_begin(&lock)) { ciuki_chrome_status("Bookmarks are busy. Try again.");break; }
        if(hotlist_ready)hotlist_fini();
        hotlist_ready=hotlist_init(ciuki_profile_path("Bookmarks.html"),NULL)==NSERROR_OK;
        bookmarks=0;
        int error=!hotlist_ready || hotlist_iterate(NULL,NULL,count_bookmark,NULL)!=NSERROR_OK;
        if(!error && !hotlist_has_url(browser_window_access_url(bw))) {
            error=bookmarks>=500 || hotlist_add_url(browser_window_access_url(bw))!=NSERROR_OK;
            if(!error)bookmarks++;
        }
        if(!error)error=ciuki_profile_replace("Bookmarks.html",save_hotlist,NULL);
        internet_profile_unlock(&lock);
        ciuki_chrome_status(error?"Unable to save bookmarks; the previous file was kept.":"Bookmark saved. Ctrl+B opens Bookmarks; Ctrl+H opens History.");
        break;
    case WALLPAPER: {
        nsurl *current=browser_window_access_url(bw);
        const char *url=current?nsurl_access(current):NULL;
        if(!url || strncmp(url,"file:///",8)) { ciuki_chrome_status("Open a local PNG, JPEG or BMP image first.");break; }
        char path[512];size_t at=0;
        for(const char *p=url+7;*p && *p!='?' && *p!='#';p++) {
            unsigned ch=(unsigned char)*p;
            if(ch=='%') {
                int hi=p[1]>='0'&&p[1]<='9'?p[1]-'0':p[1]>='a'&&p[1]<='f'?p[1]-'a'+10:p[1]>='A'&&p[1]<='F'?p[1]-'A'+10:-1;
                int lo=p[2]>='0'&&p[2]<='9'?p[2]-'0':p[2]>='a'&&p[2]<='f'?p[2]-'a'+10:p[2]>='A'&&p[2]<='F'?p[2]-'A'+10:-1;
                if(hi<0||lo<0){ciuki_chrome_status("The local image path is invalid.");return;}ch=(unsigned)(hi*16+lo);p+=2;
            }
            if(ch<32||ch==127||at+1>=sizeof(path)){ciuki_chrome_status("The local image path is too long or invalid.");return;}
            path[at++]=(char)ch;
        }
        path[at]=0;const char *dot=strrchr(path,'.');
        if(!dot || (strcasecmp(dot,".png")&&strcasecmp(dot,".jpg")&&strcasecmp(dot,".jpeg")&&strcasecmp(dot,".bmp"))) {
            ciuki_chrome_status("The current local page is not a supported wallpaper image.");break;
        }
        char arg[160];int n=snprintf(arg,sizeof(arg),"--set-wallpaper=%s",path);
        if(n<=0||(size_t)n>=sizeof(arg)){ciuki_chrome_status("The wallpaper path is too long.");break;}
        unsigned name_n=7,arg_n=(unsigned)n;uint8_t payload[240];
        desk_put32(payload,name_n);memcpy(payload+4,"control",name_n);
        desk_put32(payload+4+name_n,arg_n);memcpy(payload+8+name_n,arg,arg_n);
        ciuki_chrome_status(du3_request(&surface->connection,DESK_V3_LAUNCH,surface->window,
            surface->generation,payload,8+name_n+arg_n,-1) ?
            "Control Panel opened to apply this image." : "The wallpaper request could not be sent.");
        break;
    }
    }
    }
}
static void event(struct du_ui *u,const struct du_event *e,void *unused)
{
    (void)unused;
    if(cp_ui_event(&printing,u,e)) { fbtk_request_redraw(fbtk);return; }
    if(e->type==DU_EV_ACTIVATE || e->type==DU_EV_COMMIT) {
        if(e->id==URL) { u->focus=-1;toolbar_keys=false;navigate(du_get(u,URL)->text); }
        else action(e->id);
    } else if(e->type==DU_EV_DIALOG && e->id==DOWNLOAD_PROGRESS) {
        struct gui_download_window *d=progress_download;progress_download=NULL;
        du_dialog_close(u);
        if(d) { if(d->context)download_context_abort(d->context);download_release(d); }
        ciuki_chrome_status("Download cancelled.");fbtk_request_redraw(fbtk);
    } else if(e->type==DU_EV_DIALOG && e->id==SAVE_DOWNLOAD) {
        struct gui_download_window *d=saving_download;saving_download=NULL;
        bool saved=false;
        if(d && e->index==0 && u->dialog.text[0]) {
            if(ciuki_download_save(&d->store,u->dialog.text))
                ciuki_chrome_status(errno==EEXIST ? "That file already exists. Choose another name." : "Unable to save the download. Choose another location.");
            else {
                if(strlen(u->dialog.text)<sizeof(saved_download)) {
                    strcpy(saved_download,u->dialog.text);saved=true;
                }
                ciuki_chrome_status("Download saved.");download_release(d);
            }
        } else if(d) { ciuki_chrome_status("Download cancelled.");download_release(d); }
        du_dialog_close(u);
        if(saved) {
            static const char *const choices[]={"Open","Open With...","Close"};
            du_message(u,SAVED_DOWNLOAD,"Download Complete",saved_download,DU_DLG_ICON_INFO,choices,3,2,2);
        }
        fbtk_request_redraw(fbtk);
    } else if(e->type==DU_EV_DIALOG && e->id==SAVED_DOWNLOAD) {
        if(e->index==0 || e->index==1)open_download(e->index==1);
        du_dialog_close(u);saved_download[0]=0;fbtk_request_redraw(fbtk);
    } else if(e->type==DU_EV_DIALOG && (e->id==OPEN || e->id==FILE_PICKER)) {
        if(e->index==0 && u->dialog.text[0]) {
            if(e->id==FILE_PICKER && file_window && file_gadget)
                browser_window_set_gadget_filename(file_window->bw,file_gadget,u->dialog.text);
            else {
                nsurl *url=NULL;
                if(netsurf_path_to_nsurl(u->dialog.text,&url)==NSERROR_OK) { navigate(nsurl_access(url));nsurl_unref(url); }
            }
        }
        du_dialog_close(u);file_window=NULL;file_gadget=NULL;fbtk_request_redraw(fbtk);
    } else if(e->type==DU_EV_DIALOG) { du_dialog_close(u);fbtk_request_redraw(fbtk); }
}
static void layout(void)
{
    int w=surface->current.info.width,h=surface->current.info.height;
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);i++)du_get(&ui,buttons[i])->box=(struct du_rect){4+(int)i*30,4,28,28};
    int url_x=8+(int)(sizeof(buttons)/sizeof(*buttons))*30;   /* the address box follows the toolbar buttons */
    du_get(&ui,URL)->box=(struct du_rect){url_x,6,w-url_x-6,24};
    du_get(&ui,STATUS)->box=(struct du_rect){0,h-CIUKI_BROWSER_STATUS,w,CIUKI_BROWSER_STATUS};
}
void ciuki_chrome_init(struct ciuki_browser_surface *s)
{
    content_cursor=DESK_CURSOR_ARROW;
    surface=s;active=status_dirty=true;toolbar_keys=false;input_records=0;
    du_init(&ui,s->current.back,s->current.info.width,s->current.info.height);
    ui.event=event;
    const char *labels[]={"Back","Forward","Reload","Stop","Home","Bookmark","New window","Wallpaper"};
    const char *icons[]={"actions/go-previous","actions/go-next","actions/view-refresh","actions/process-stop","actions/go-home","actions/bookmark-new","actions/document-new","preferences-desktop-wallpaper"};
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);i++) {
        struct du_widget *widget=du_add(&ui,DU_TOOLBUTTON,buttons[i],(struct du_rect){0},"");
        snprintf(widget->tip,sizeof(widget->tip),"%s",labels[i]);   /* icon-only buttons like IE5's compact bar; the name is the tooltip */
        widget->icon=du_icon_argb(icons[i],22);widget->icon_size=22;
    }
    du_add(&ui,DU_EDIT,URL,(struct du_rect){0},"");
    du_add(&ui,DU_STATUS,STATUS,(struct du_rect){0},"Ready");
    layout();
}
void ciuki_chrome_resize(void)
{
    if(!active)return;
    du_resize(&ui,surface->current.back,surface->current.info.width,surface->current.info.height);layout();
    status_dirty=true;
}
static void chrome_cursor(void)
{
    if(!active)return;
    int chrome=ui.dialog.kind || ui.capture>=0 || ui.mouse_y<CIUKI_BROWSER_TOP ||
        ui.mouse_y>=(int)surface->current.info.height-CIUKI_BROWSER_STATUS;
    unsigned shape=chrome ? du_cursor_at(&ui) : content_cursor;
    if(!chrome && loading && shape==DESK_CURSOR_ARROW)shape=DESK_CURSOR_ARROW_BUSY;
    (void)ciuki_browser_cursor(surface,shape);
}
void ciuki_chrome_pointer(unsigned shape)
{ content_cursor=ciuki_pointer_shape(shape);chrome_cursor(); }
void ciuki_chrome_render(void)
{
    if(active)cp_ui_poll(&printing,&ui);
    downloads_poll();
    chrome_cursor(); /* also retry a refused cursor send on an otherwise idle UI turn */
    if(!active || (!ui.dirty && !status_dirty))return;
    struct browser_window *bw=browser();
    du_get(&ui,BACK)->disabled=!bw || !browser_window_back_available(bw);
    du_get(&ui,FORWARD)->disabled=!bw || !browser_window_forward_available(bw);
    du_get(&ui,RELOAD)->disabled=!bw || loading;du_get(&ui,STOP)->disabled=!bw || !loading;
    if(ui.dialog.kind) {
        du_render(&ui);ciuki_browser_damage(surface,(nsfb_bbox_t){0,0,ui.canvas.width,ui.canvas.height});status_dirty=false;return;
    }
    /* Render only chrome strips, preserving the framebuffer's owned content. */
    if(ui.dirty) {
        ui.dirty=3;ui.invalid=(struct du_rect){0,0,ui.canvas.width,CIUKI_BROWSER_TOP};du_render(&ui);
        ciuki_browser_damage(surface,(nsfb_bbox_t){0,0,ui.canvas.width,CIUKI_BROWSER_TOP});
    }
    if(status_dirty) {
        ui.dirty=3;ui.invalid=(struct du_rect){0,ui.canvas.height-CIUKI_BROWSER_STATUS,ui.canvas.width,CIUKI_BROWSER_STATUS};du_render(&ui);
        ciuki_browser_damage(surface,(nsfb_bbox_t){0,ui.canvas.height-CIUKI_BROWSER_STATUS,ui.canvas.width,ui.canvas.height});status_dirty=false;
    }
}
/* fx-nsperf: libnsfb updates only collect damage (surface.c). The frame
 * drawn in a main-loop turn is presented at its end, before the scheduler
 * can run a long layout; while input is still queued (several wheel
 * detents) the next turns draw first and one present covers them all. */
void ciuki_chrome_present(void)
{ if(active && surface && !surface->count)ciuki_browser_present(surface); }
void ciuki_chrome_status(const char *text)
{ if(active) { du_set_text(du_get(&ui,STATUS),text?text:"");status_dirty=true; } }
/* The URL the core last displayed: for its internal query pages this is the
 * site being loaded (the core's loading parameters), not the about: page. */
static nsurl *shown_url;
void ciuki_chrome_url(nsurl *url)
{
    if(url) { if(shown_url)nsurl_unref(shown_url);shown_url=nsurl_ref(url); }
    /* A late loading notification must not overwrite an address being edited. */
    if(active && url && !(toolbar_keys && du_focused(&ui)==URL)) {
        du_set_text(du_get(&ui,URL),nsurl_access(url));ui.dirty=1;
    }
}
/* browser_window_reload re-fetches the content URL. On the core's internal
 * error pages (about:query/...) that navigation needs the page's form data
 * and browser_window_navigate refuses it (NSERROR_NEED_DATA), so a reload
 * would never retry the site. Navigate to the displayed site URL instead,
 * without a history entry or POST replay: a reload after "not ready" then
 * retries HTTPS with a fresh entropy request. */
void ciuki_chrome_reload(void)
{
    struct browser_window *bw=browser();if(!bw)return;
    nsurl *current=browser_window_access_url(bw);
    if(current && shown_url && !strncmp(nsurl_access(current),"about:query/",12) && strncmp(nsurl_access(shown_url),"about:query/",12)) {
        if(browser_window_navigate(bw,shown_url,NULL,BW_NAVIGATE_NONE,NULL,NULL,NULL)!=NSERROR_OK)ciuki_chrome_status("Unable to reload this address.");
        return;
    }
    browser_window_reload(bw,true);
}
void ciuki_chrome_complete(struct gui_window *window)
{
    if(!profile_ready || !window || window!=window_list)return;
    struct hlcache_handle *content=browser_window_get_content(window->bw);
    /* READY may have scheduled a layout error that has not broadcast yet. */
    if(!content || content_is_layout_rejected(content))return;
    nsurl *url=browser_window_access_url(window->bw);
    if(url && (ciuki_profile_visit(nsurl_access(url)) || ciuki_profile_history_save()))
        ciuki_chrome_status("Unable to record this page in history.");
}
/* f6-18 test profile: gate, fixture anchors and the one-shot page report. */
#include "testmode.inc"
void ciuki_chrome_loading(bool value)
{
    loading=value;if(active)ui.dirty=1;
    chrome_cursor();
    if(!value)ciuki_test_page_done();
}
bool ciuki_chrome_busy(void) { return loading; }
void ciuki_chrome_title(struct gui_window *window,const char *title)
{
    (void)window;
    if(!active || !surface->ready)return;
    unsigned n=title?strlen(title):0;if(n>DESK_TITLE_MAX)n=DESK_TITLE_MAX;
    while(n && !desk_utf8_valid((const uint8_t *)title,n))n--;
    uint8_t b[4+DESK_TITLE_MAX];desk_put32(b,n);if(n)memcpy(b+4,title,n);
    du3_request(&surface->connection,DESK_SET_TITLE,surface->window,surface->generation,b,n+4,-1);
}
void ciuki_chrome_release(void)
{ if(active)du_release(&ui); }
bool ciuki_chrome_modal(void) { return active && ui.dialog.kind; }
static void open_profile_page(const char *name)
{
    const char *path=ciuki_profile_path(name);nsurl *url=NULL;
    if(!strcmp(name,"History.html") && ciuki_profile_history_save()) {
        ciuki_chrome_status("Unable to save history; the previous file was kept.");return;
    }
    if(path && netsurf_path_to_nsurl(path,&url)==NSERROR_OK) { navigate(nsurl_access(url));nsurl_unref(url); }
}
static void focus_address(void)
{
    du_focus(&ui,URL);toolbar_keys=true;
    struct du_widget *edit=du_get(&ui,URL);
    edit->anchor=0;edit->cursor=strlen(edit->text);edit->selection.ns=0;ui.dirty=1;
}
static bool print_key(int code)
{
    if(code!=0x19 || !(ui.modifiers&DU_CTRL))return false;
    if(ui.modifiers&DU_SHIFT)cp_ui_setup(&printing,&ui);
    else if(surface && surface->ready)ciuki_print_document(&printing,&ui,browser());
    return true;
}
bool ciuki_chrome_input(const struct ciuki_input_event *input)
{
    if(!active)return false;
    struct ciuki_input_event e=*input;
    if(e.type==CIUKI_INPUT_RESYNC) { ciuki_chrome_release();return false; }
    if(e.type==CIUKI_INPUT_KEY && e.code>=0 && e.code<=0x200) {
        /* Keep toolkit modifiers current even while content owns keyboard. */
        if(e.code==DU_KEY_LCTRL || e.code==DU_KEY_RCTRL || e.code==DU_KEY_LSHIFT || e.code==DU_KEY_RSHIFT || e.code==DU_KEY_LALT || e.code==DU_KEY_RALT) {
            du_input(&ui,&e);return false;
        }
        bool ctrl=ui.modifiers&DU_CTRL,alt=ui.modifiers&DU_ALT;
        if(e.value && !ui.dialog.kind) {
            if(print_key(e.code))return true;
            if(ctrl && e.code==0x26) { focus_address();return true; }
            if(ctrl && e.code==0x31) { action(NEW_WINDOW);return true; }
            if(ctrl && e.code==0x13) { action(RELOAD);return true; }
            if(e.code==DU_KEY_ESC) { action(STOP);return true; }
            if(alt && e.code==DU_KEY_LEFT) { action(BACK);return true; }
            if(alt && e.code==DU_KEY_RIGHT) { action(FORWARD);return true; }
            if(ctrl && e.code==0x18) { du_file_dialog(&ui,OPEN,0,"Open local page","/Documents","",NULL,NULL);return true; }
            if(ctrl && e.code==0x30) { open_profile_page("Bookmarks.html");return true; }
            if(ctrl && e.code==0x23) { open_profile_page("History.html");return true; }
        }
        if(!ui.dialog.kind && !toolbar_keys)return false;
    }
    if(e.type==CIUKI_INPUT_MOTION) {
        bool chrome=e.value2<CIUKI_BROWSER_TOP || e.value2>=(int)surface->current.info.height-CIUKI_BROWSER_STATUS;
        if(!chrome && !ui.dialog.kind && ui.capture<0) { ui.mouse_x=e.value;ui.mouse_y=e.value2;chrome_cursor();return false; }
    }
    if(e.type==CIUKI_INPUT_BUTTON && !ui.dialog.kind && ui.capture<0 &&
       ui.mouse_y>=CIUKI_BROWSER_TOP && ui.mouse_y<(int)surface->current.info.height-CIUKI_BROWSER_STATUS) {
        if(e.value) { if(toolbar_keys || ui.focus>=0)ui.dirty=1;toolbar_keys=false;ui.focus=-1; }return false;
    }
    if(e.type==CIUKI_INPUT_TEXT && !toolbar_keys && !ui.dialog.kind)return false;
    struct du_widget *edit=NULL;
    if(ui.dialog.kind && ui.dialog.focus>=0 && (unsigned)ui.dialog.focus<ui.dialog.count) {
        struct du_widget *focused=&ui.dialog.items[ui.dialog.focus];
        if(focused->kind==DU_EDIT)edit=focused;
    } else if(!ui.dialog.kind && du_focused(&ui)==URL)edit=du_get(&ui,URL);
    /* Desktop TEXT owns printable input, including AltGr (right Alt or
     * Ctrl+right Alt). KEY still owns editing, navigation and shortcuts. */
    bool altgr=ui.keys[DU_KEY_RALT];
    if(e.type==CIUKI_INPUT_TEXT && edit) {
        /* QEMU 2026-10-25: the desktop also sends the character of a
         * Ctrl or Alt chord, so Ctrl+L selected the address and then typed
         * "l" over it ("lhttp://..."). As in a Windows edit control, a Ctrl
         * or (left) Alt chord is a command, never text; AltGr still types. */
        if((ui.modifiers&(DU_CTRL|DU_ALT)) && !altgr)return true;
        char utf8[5];
        if(e.code>=32 && e.code!=127 && du_utf8_encode(e.code,utf8))du_edit_insert(edit,utf8);
        ui.dirty=1;return true;
    }
    if(e.type==CIUKI_INPUT_KEY && e.value && edit &&
       (!(ui.modifiers&(DU_CTRL|DU_ALT)) || altgr) && du_keymap(DU_LAYOUT_US,e.code,0))return true;
    if(e.type==CIUKI_INPUT_WHEEL && !ui.dialog.kind && ui.capture<0 &&
       ui.mouse_y>=CIUKI_BROWSER_TOP && ui.mouse_y<(int)surface->current.info.height-CIUKI_BROWSER_STATUS)return false;
    if(e.type==CIUKI_INPUT_KEY && e.value && e.code==DU_KEY_TAB && du_focused(&ui)==URL && !(ui.modifiers&DU_SHIFT)) {
        toolbar_keys=false;ui.focus=-1;ui.dirty=1;return false;
    }
    bool entering_address=e.type==CIUKI_INPUT_BUTTON && e.code==1 && e.value && !ui.dialog.kind &&
        (!toolbar_keys || du_focused(&ui)!=URL) && du_contains(du_get(&ui,URL)->box,ui.mouse_x,ui.mouse_y);
    if(e.type==CIUKI_INPUT_BUTTON && e.code==1 && e.value && !ui.dialog.kind) {
        int hit=du_hit(&ui,ui.mouse_x,ui.mouse_y);
        /* deskui toolbuttons deliberately do not take keyboard focus. Blur
         * the edit before their navigation can report the new page's URL. */
        if(hit>=0 && ui.widgets[hit].kind==DU_TOOLBUTTON) {
            toolbar_keys=false;ui.focus=-1;ui.dirty=1;
        }
    }
    du_input(&ui,&e);
    if(e.type==CIUKI_INPUT_BUTTON && e.value && !ui.dialog.kind)toolbar_keys=du_focused(&ui)!=0;
    if(entering_address)focus_address();
    chrome_cursor();
    return true;
}
/* Both owners need the latest client position, even when one consumes motion.
 * Only the page translator subtracts CIUKI_BROWSER_TOP. A held page button
 * keeps its release/drag; deskui's own capture keeps chrome selections. */
void ciuki_chrome_dispatch(struct ciuki_browser_surface *s,const struct ciuki_input_event *e)
{
    if(e->type==CIUKI_INPUT_MOTION) {
        s->mouse_x=ui.mouse_x=e->value;s->mouse_y=ui.mouse_y=e->value2;
    }
    bool page_drag=s->buttons && (e->type==CIUKI_INPUT_MOTION || e->type==CIUKI_INPUT_BUTTON);
    int id=0;
    if(active) {
        int hit=e->type==CIUKI_INPUT_BUTTON?du_hit(&ui,ui.mouse_x,ui.mouse_y):ui.focus;
        if(hit>=0 && (unsigned)hit<ui.count)id=ui.widgets[hit].id;
        if(ui.capture>=0 && e->type==CIUKI_INPUT_BUTTON)id=ui.widgets[ui.capture].id;
    }
    bool chrome=!page_drag && ciuki_chrome_input(e);
    if(!chrome)ciuki_browser_input(s,e);
    /* First 512 edges per window, then silence; no motion or typed text data. */
    if(e->type==CIUKI_INPUT_BUTTON || e->type==CIUKI_INPUT_KEY || e->type==CIUKI_INPUT_TEXT) {
        if(input_records<512) {
            const char *widget="toolbar";
            if(!chrome)widget="content";
            else if(ui.dialog.kind)widget="dialog";
            else if(id==URL || (!id && du_focused(&ui)==URL))widget="address";
            else if(id==BACK)widget="back";else if(id==FORWARD)widget="forward";
            else if(id==RELOAD)widget="reload";else if(id==STOP)widget="stop";
            else if(id==HOME)widget="home";else if(id==BOOKMARK)widget="bookmark";
            else if(id==STATUS)widget="status";
            ciuki_log_line("[netsurf] input target=%s widget=%s type=%u code=%d value=%d last=%u",
                chrome?"chrome":"page",widget,(unsigned)e->type,
                e->type==CIUKI_INPUT_TEXT?0:e->code,e->value,input_records==511);
            input_records++;
        }
    }
}
void ciuki_chrome_file(struct gui_window *window,struct hlcache_handle *content,struct form_control *gadget)
{
    (void)content;
    if(!active)return;
    file_window=window;file_gadget=gadget;
    if(du_file_dialog(&ui,FILE_PICKER,0,"Choose a file","/Documents","",NULL,NULL))ciuki_chrome_status("File picker unavailable.");
}
static void download_release(struct gui_download_window *d)
{
    for(unsigned i=0;i<4;i++)if(downloads[i]==d)downloads[i]=NULL;
    if(saving_download==d)saving_download=NULL;
    if(progress_download==d) { progress_download=NULL;if(ui.dialog.id==DOWNLOAD_PROGRESS)du_dialog_close(&ui);fbtk_request_redraw(fbtk); }
    ciuki_download_discard(&d->store);
    if(d->context)download_context_destroy(d->context);
    free(d);
}
static struct gui_download_window *download_create(struct download_context *context,struct gui_window *window)
{
    (void)window;
    unsigned slot=0;while(slot<4 && downloads[slot])slot++;
    if(slot==4 || !active) { ciuki_chrome_status("Finish a download before starting another.");return NULL; }
    struct gui_download_window *d=calloc(1,sizeof(*d));if(!d)return NULL;
    if(ciuki_download_begin(&d->store,"/Documents",download_context_get_filename(context))) {
        free(d);ciuki_chrome_status("Unable to create the download file.");return NULL;
    }
    d->context=context;d->total=download_context_get_total_length(context);downloads[slot]=d;ciuki_chrome_status("Downloading... A Save dialog will open when the file is ready.");return d;
}
static nserror download_data(struct gui_download_window *d,const char *data,unsigned int size)
{
    if(d->failed || ciuki_download_write(&d->store,data,size)) {
        d->failed=true;ciuki_chrome_status("Download failed: storage full or file too large.");return NSERROR_SAVE_FAILED;
    }
    return NSERROR_OK;
}
static void download_error(struct gui_download_window *d,const char *message)
{ ciuki_chrome_status(message);download_release(d); }
static void download_done(struct gui_download_window *d)
{
    if(d->failed || ciuki_download_complete(&d->store)) {
        ciuki_chrome_status("The download could not be completed.");download_release(d);return;
    }
    download_context_destroy(d->context);d->context=NULL;
    if(progress_download==d) { progress_download=NULL;if(ui.dialog.id==DOWNLOAD_PROGRESS)du_dialog_close(&ui);fbtk_request_redraw(fbtk); }
    ciuki_chrome_status("Download complete. Choose where to save it.");
}
static void downloads_poll(void)
{
    for(unsigned i=0;i<4;i++)if(downloads[i] && downloads[i]->failed) {
        download_context_abort(downloads[i]->context);download_release(downloads[i]);
    }
    if(!active)return;
    if(progress_download && ui.dialog.id==DOWNLOAD_PROGRESS) {
        struct gui_download_window *d=progress_download;char message[256];
        snprintf(message,sizeof(message),"%s\n%llu of %llu bytes received",d->store.name,(unsigned long long)d->store.bytes,(unsigned long long)d->total);
        unsigned value=d->total?(unsigned)((uint64_t)d->store.bytes*1000/d->total):0;if(value>1000)value=1000;
        du_progress(&ui,DOWNLOAD_PROGRESS,"File Download",message,value,d->total?1000:0);return;
    }
    if(ui.dialog.kind || saving_download)return;
    for(unsigned i=0;i<4;i++)if(downloads[i] && downloads[i]->store.complete) {
        struct gui_download_window *d=downloads[i];
        if(!du_file_dialog(&ui,SAVE_DOWNLOAD,1,"Save download","/Documents",d->store.name,NULL,NULL))saving_download=d;
        return;
    }
    for(unsigned i=0;i<4;i++)if(downloads[i] && downloads[i]->context) {
        progress_download=downloads[i];du_progress(&ui,DOWNLOAD_PROGRESS,"File Download",progress_download->store.name,0,0);return;
    }
}
static void downloads_finish(void)
{
    for(unsigned i=0;i<4;i++)if(downloads[i]) {
        if(downloads[i]->context)download_context_abort(downloads[i]->context);
        download_release(downloads[i]);
    }
}
struct gui_download_table ciuki_download_table={.create=download_create,.data=download_data,.error=download_error,.done=download_done};
void ciuki_chrome_defaults(void)
{
    struct cp_internet_options internet;
    internet_defaults(&browser_internet);
    if(!internet_load(INTERNET_OPTIONS_PATH,&internet)) {
        browser_internet=internet;
        ciuki_profile_history_days(internet.history_days);
        nsoption_set_charp(homepage_url,strdup(internet.home));
        nsoption_set_int(memory_cache_size,internet.cache_mb*1024*1024);
        nsoption_set_bool(http_proxy,internet.proxy[0]!=0);
        nsoption_set_charp(http_proxy_host,internet.proxy[0]?strdup(internet.proxy):NULL);
        nsoption_set_int(http_proxy_port,internet.port);
    } else nsoption_set_int(memory_cache_size,2*1024*1024);
    /* On again since 2026-10-25: the fault of the first remote page with a
     * script (QEMU, duk_hobject_find_entry_tval_ptr_stridx at 0x200) was
     * Duktape's packed values copied through x87 registers (patch 0014,
     * docs/design/netsurf-port-plan.md). A profile Choices file with
     * enable_javascript:0 turns it off. */
    nsoption_set_bool(enable_javascript,true);
    nsoption_set_uint(disc_cache_size,0);
    nsoption_set_charp(disc_cache_path,NULL);nsoption_set_charp(cookie_file,NULL);nsoption_set_charp(cookie_jar,NULL);
    nsoption_set_int(max_fetchers,8);nsoption_set_int(max_fetchers_per_host,2);
    nsoption_set_charp(fb_toolbar_layout,strdup("q"));
}
static const char *exit_reason;
static int exit_detail;
static bool exit_reported;
static void exit_fallback(void);
int ciuki_chrome_arguments(int *argc,char **argv)
{
    /* f6-22: every complete stderr line, NetSurf's own log included, leaves
     * as one write (see logline.h); C allows a line-buffered stderr. */
    static char stderr_buffer[1024];
    setvbuf(stderr,stderr_buffer,_IOLBF,sizeof(stderr_buffer));
    atexit(exit_fallback);
    for(int i=1;i<*argc;) {
        const char *a=argv[i];bool consumed=false;
        if(!strncmp(a,"--channel-fd=",13)) {
            char *end;long n=strtol(a+13,&end,10);if(*end || n<3 || n>63) { exit_reason="bad-arguments";return -1; }
            ciuki_browser_channel=n;consumed=true;
        }
        else if(!strcmp(a,"--test=browser")) { test_requested=true;consumed=true; }
#ifdef CIUKI_HOST
        else if(!strncmp(a,"--ciuki-resources=",18)) { ciuki_resource_path=a+18;consumed=true; }
        else if(!strncmp(a,"--ciuki-profile=",16)) { profile_path=a+16;profile_requested=true;consumed=true; }
        else if(!strncmp(a,"--ciuki-capture=",16)) { ciuki_capture_path=a+16;consumed=true; }
        else if(!strcmp(a,"--ciuki-frame-bench")) { ciuki_frame_bench=true;consumed=true; }
        else if(!strncmp(a,"--ciuki-log=",12)) { ciuki_netsurf_log_path=a+12;consumed=true; }
        else if(!strncmp(a,"--ciuki-clock-scale=",20)) {
            /* f6-20 T23 model: charge host layouts as a slower CPU. */
            char *end;unsigned long n=strtoul(a+20,&end,10);if(*end || !n || n>1000000) { exit_reason="bad-arguments";return -1; }
            ciuki_layout_clock_scale=(uint32_t)n;consumed=true;
        }
#endif
        if(consumed) { memmove(argv+i,argv+i+1,(size_t)(*argc-i)*sizeof(*argv));(*argc)--; } else i++;
    }
#ifndef CIUKI_HOST
    select_user_paths(); /* sec-user-05 */
#endif
    /* Before netsurf_init registers the fetchers, which create the network
     * with the trust anchors the profile selects. */
    ciuki_test_arm();
    /* fx-15: the body faces load while the rest of start-up runs. */
    if(!ciuki_capture_path && !ciuki_frame_bench)ciuki_font_prefetch();
    return 0;
}
int ciuki_chrome_start(void)
{
    if((ciuki_capture_path || ciuki_frame_bench) && !profile_requested)return 0;
    profile_ready=ciuki_profile_init(profile_path)==0;
    if(!profile_ready)return -1;
    const char *bookmarks_path=ciuki_profile_path("Bookmarks.html");
    if(ciuki_profile_admit(bookmarks_path,512*1024,500) && errno!=ENOENT)return -1;
    hotlist_ready=hotlist_init(bookmarks_path,NULL)==NSERROR_OK;
    if(hotlist_ready && hotlist_iterate(NULL,NULL,count_bookmark,NULL)!=NSERROR_OK) {
        hotlist_fini();hotlist_ready=false;return -1;
    }
    return hotlist_ready?0:-1;
}
void ciuki_chrome_finish(void)
{
    cp_ui_destroy(&printing);
    downloads_finish();
    if(profile_ready) {
        if(ciuki_profile_history_save() || ciuki_profile_replace("Choices",save_choices,NULL))
            fprintf(stderr,"NetSurf: unable to save profile; previous files retained.\n");
    }
    if(hotlist_ready)hotlist_fini();
    if(shown_url) { nsurl_unref(shown_url);shown_url=NULL; }
    ciuki_profile_fini();active=profile_ready=hotlist_ready=false;
}
void ciuki_chrome_exit_reason(const char *reason)
{
    if(exit_reason)return;
    if(reason) { exit_reason=reason;return; }
    struct ciuki_browser_surface *s=ciuki_browser_active;
    exit_reason=s && s->close_reason?s->close_reason:"quit-event";
    exit_detail=s && s->close_reason?s->close_detail:0;
}
/* Builds without the native fetcher (unit harnesses) report "unknown". */
__attribute__((weak)) const char *ciuki_net_state_word(void);
__attribute__((weak)) size_t ciuki_net_fetch_summary(char *,size_t);
__attribute__((weak)) unsigned ciuki_net_connect_retries(void);
__attribute__((format(printf,3,4)))
static int format_line(char *line,size_t size,const char *format,...)
{ va_list ap;va_start(ap,format);int n=ciuki_line_vformat(line,size,format,ap);va_end(ap);return n; }
static int exit_line(char *line,size_t size,bool surface)
{
    struct ciuki_browser_surface *s=surface?ciuki_browser_active:NULL;
    char fetches[128]=" ";
    if(!ciuki_net_fetch_summary || !ciuki_net_fetch_summary(fetches+1,sizeof(fetches)-1))fetches[0]=0;
    /* fx-nsperf: present work (DAMAGE messages, refused sends) and connects
     * retried after the system refused a socket. */
    return format_line(line,size,"[netsurf] exit reason=%s detail=%d layout_rejections=%u layout_max_us=%llu local_max_us=%llu remote_max_us=%llu input_overflows=%u keepalive_pumps=%u presents=%llu damage_messages=%llu present_refused=%llu box_slices=%llu box_elements=%llu box_slice_max_us=%llu box_element_max_us=%llu image_reuse=%u connect_retries=%u%s network=%s",
            exit_reason?exit_reason:"unknown",exit_detail,ciuki_layout_rejections,
            (unsigned long long)(ciuki_layout_max_ns/1000u),(unsigned long long)(ciuki_layout_local_max_ns/1000u),
            (unsigned long long)(ciuki_layout_remote_max_ns/1000u),s?s->overflows:0u,s?s->pumps:0u,
            (unsigned long long)(s?s->stats.presents:0),(unsigned long long)(s?s->stats.messages:0),(unsigned long long)(s?s->stats.refused:0),
            (unsigned long long)ciuki_box_slices,(unsigned long long)ciuki_box_elements,(unsigned long long)(ciuki_box_slice_max_ns/1000u),(unsigned long long)(ciuki_box_element_max_ns/1000u),ciuki_speculative_adoptions,
            ciuki_net_connect_retries?ciuki_net_connect_retries():0u,fetches,
            ciuki_net_state_word?ciuki_net_state_word():"unknown");
}
/* Appends "<UTC time> <line>"; past 64 KiB the file starts over. */
static void netsurf_log(const char *line)
{
    if(!ciuki_netsurf_log_path)return;
    struct stat st;int flags=O_WRONLY|O_CREAT|O_APPEND;
    if(!stat(ciuki_netsurf_log_path,&st) && st.st_size>=(off_t)CIUKI_NETSURF_LOG_MAX)flags=O_WRONLY|O_CREAT|O_TRUNC;
    int fd=open(ciuki_netsurf_log_path,flags,0666);if(fd<0)return;
    char stamp[32]="unknown-time ";struct timespec now;struct tm utc;
    if(!clock_gettime(CLOCK_REALTIME,&now) && gmtime_r(&now.tv_sec,&utc))
        snprintf(stamp,sizeof(stamp),"%04d-%02d-%02dT%02d:%02d:%02dZ ",utc.tm_year+1900,utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec);
    ciuki_write_all(fd,stamp,strlen(stamp));ciuki_write_all(fd,line,strlen(line));close(fd);
}
/* fx-11: net.c's fetch error records (stage, errno, TLS codes, RTC state),
 * at most 16 per browser session so a failing page cannot fill the file. */
void ciuki_netsurf_log_record(const char *line)
{
    static unsigned records;
    if(records>=16)return;
    records++;netsurf_log(line);
}
void ciuki_chrome_exit_report(void)
{
#ifndef CIUKI_HOST
    /* Called right before netsurf_exit(): see ciuki_js_exiting. */
    ciuki_js_exiting=true;
#endif
    char line[CIUKI_LOG_LINE_MAX];
    if(exit_reported || exit_line(line,sizeof(line),true)<=0)return;
    exit_reported=true;
    fflush(stderr);ciuki_write_all(2,line,strlen(line));
    netsurf_log(line);
}
/* die() and early returns: stderr already has their message; the log file
 * still gets an exit line. The surface may be gone, so it is not read. */
static void exit_fallback(void)
{
    char line[CIUKI_LOG_LINE_MAX];
    if(exit_reported)return;
    exit_reported=true;
    if(!exit_reason)exit_reason="fatal";
    if(exit_line(line,sizeof(line),false)>0)netsurf_log(line);
}
void ciuki_chrome_capture(void *context)
{
#ifdef CIUKI_HOST
    if(!ciuki_capture_path)return;
    extern unsigned ciuki_file_slices;
    fprintf(stderr,"NetSurf local capture: layout_max_ns=%llu local_max_ns=%llu remote_max_ns=%llu clock_scale=%u file_slices=%u\n",
            (unsigned long long)ciuki_layout_max_ns,(unsigned long long)ciuki_layout_local_max_ns,
            (unsigned long long)ciuki_layout_remote_max_ns,(unsigned)ciuki_layout_clock_scale,ciuki_file_slices);
    if(browser())fprintf(stderr,"NetSurf local title: %s\n",browser_window_get_title(browser()));
    nsfb_t *fb=context;uint8_t *pixels;int stride;
    nsfb_get_buffer(fb,&pixels,&stride);
    int w,h;nsfb_get_geometry(fb,&w,&h,NULL);
    FILE *out=fopen(ciuki_capture_path,"wb");if(!out)return;
    for(int y=0;y<h;y++)fwrite(pixels+(size_t)y*stride,4,w,out);
    fclose(out);
#else
    (void)context;
#endif
}

/* Called before a document enables its script engine, including frames. */
bool ciuki_chrome_scripts(const char *host)
{ return browser_internet.scripts[internet_zone(&browser_internet,host?host:"")]!=0; }

const struct cp_internet_options *ciuki_chrome_internet(void) { return &browser_internet; }
