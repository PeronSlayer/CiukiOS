/* SPDX-License-Identifier: GPL-2.0-only
 * Single UI owner: deskui chrome/dialogs; fbtk owns the content viewport. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "surface.h"
#include "profile.h"
#include "chrome.h"
#include "budget.h"
#include "logline.h"

enum { BACK=1,FORWARD,RELOAD,STOP,BOOKMARK,URL,STATUS,OPEN=20,FILE_PICKER,NOTICE };
static struct du_ui ui;
static struct ciuki_browser_surface *surface;
static bool active, loading, hotlist_ready, profile_ready, toolbar_keys;
static bool status_dirty;
static bool profile_requested;
static unsigned bookmarks;
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
static void navigate(const char *text)
{
    nsurl *url=NULL;
    nserror err=nsurl_create(text,&url);
    if(!err && browser())err=browser_window_navigate(browser(),url,NULL,BW_NAVIGATE_HISTORY,NULL,NULL,NULL);
    if(url)nsurl_unref(url);
    if(err)ciuki_chrome_status("Unable to open this address.");
}
static int save_hotlist(const char *path,void *unused)
{ (void)unused;return hotlist_export(path,"NetSurf Bookmarks")==NSERROR_OK?0:-1; }
static int save_choices(const char *path,void *unused)
{ (void)unused;return nsoption_write(path,nsoptions,nsoptions_default)==NSERROR_OK?0:-1; }
static void action(int id)
{
    struct browser_window *bw=browser();if(!bw)return;
    switch(id) {
    case BACK: if(browser_window_back_available(bw))browser_window_history_back(bw,false);break;
    case FORWARD:if(browser_window_forward_available(bw))browser_window_history_forward(bw,false);break;
    case RELOAD:ciuki_chrome_reload();break;
    case STOP:browser_window_stop(bw);ciuki_chrome_loading(false);break;
    case BOOKMARK:
        if(!hotlist_ready || bookmarks>=500) { ciuki_chrome_status("Bookmark limit reached or profile unavailable.");break; }
        if(!hotlist_has_url(browser_window_access_url(bw))) {
            if(hotlist_add_url(browser_window_access_url(bw))) { ciuki_chrome_status("Unable to add bookmark.");break; }
            bookmarks++;
        }
        if(ciuki_profile_replace("Bookmarks.html",save_hotlist,NULL))ciuki_chrome_status("Unable to save bookmarks; the previous file was kept.");
        else ciuki_chrome_status("Bookmark saved. Ctrl+B opens Bookmarks; Ctrl+H opens History.");
        break;
    }
}
static void event(struct du_ui *u,const struct du_event *e,void *unused)
{
    (void)unused;
    if(e->type==DU_EV_ACTIVATE || e->type==DU_EV_COMMIT) {
        if(e->id==URL) { navigate(du_get(u,URL)->text);u->focus=-1;toolbar_keys=false; }
        else action(e->id);
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
    for(unsigned i=0;i<5;i++)du_get(&ui,BACK+i)->box=(struct du_rect){4+(int)i*30,4,28,28};
    du_get(&ui,URL)->box=(struct du_rect){158,6,w-164,24};
    du_get(&ui,STATUS)->box=(struct du_rect){0,h-CIUKI_BROWSER_STATUS,w,CIUKI_BROWSER_STATUS};
}
void ciuki_chrome_init(struct ciuki_browser_surface *s)
{
    surface=s;active=status_dirty=true;du_init(&ui,s->current.back,s->current.info.width,s->current.info.height);
    ui.event=event;
    const char *labels[]={"Back","Forward","Reload","Stop","Bookmark"};
    const char *icons[]={"actions/go-previous","actions/go-next","actions/view-refresh","actions/process-stop","actions/bookmark-new"};
    for(unsigned i=0;i<5;i++) {
        struct du_widget *widget=du_add(&ui,DU_TOOLBUTTON,BACK+i,(struct du_rect){0},labels[i]);
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
void ciuki_chrome_render(void)
{
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
void ciuki_chrome_status(const char *text)
{ if(active) { du_set_text(du_get(&ui,STATUS),text?text:"");status_dirty=true; } }
/* The URL the core last displayed: for its internal query pages this is the
 * site being loaded (the core's loading parameters), not the about: page. */
static nsurl *shown_url;
void ciuki_chrome_url(nsurl *url)
{
    if(url) { if(shown_url)nsurl_unref(shown_url);shown_url=nsurl_ref(url); }
    if(active && url) { du_set_text(du_get(&ui,URL),nsurl_access(url));ui.dirty=1; }
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
    if(url && ciuki_profile_visit(nsurl_access(url)))
        ciuki_chrome_status("Unable to record this page in history.");
}
/* f6-18 test profile: gate, fixture anchors and the one-shot page report. */
#include "testmode.inc"
void ciuki_chrome_loading(bool value)
{
    loading=value;if(active)ui.dirty=1;
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
            if(ctrl && e.code==0x26) { du_focus(&ui,URL);du_get(&ui,URL)->anchor=0;du_get(&ui,URL)->cursor=strlen(du_get(&ui,URL)->text);toolbar_keys=true;return true; }
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
        if(!chrome && !ui.dialog.kind && ui.capture<0) { ui.mouse_x=e.value;ui.mouse_y=e.value2;return false; }
    }
    if(e.type==CIUKI_INPUT_BUTTON && !ui.dialog.kind && ui.capture<0 &&
       ui.mouse_y>=CIUKI_BROWSER_TOP && ui.mouse_y<(int)surface->current.info.height-CIUKI_BROWSER_STATUS) {
        if(e.value) { toolbar_keys=false;ui.focus=-1; }return false;
    }
    if(e.type==CIUKI_INPUT_TEXT && !toolbar_keys && !ui.dialog.kind)return false;
    struct du_widget *edit=NULL;
    if(ui.dialog.kind && ui.dialog.focus>=0 && (unsigned)ui.dialog.focus<ui.dialog.count) {
        struct du_widget *focused=&ui.dialog.items[ui.dialog.focus];
        if(focused->kind==DU_EDIT)edit=focused;
    } else if(!ui.dialog.kind && du_focused(&ui)==URL)edit=du_get(&ui,URL);
    if(e.type==CIUKI_INPUT_TEXT && edit) {
        char utf8[5];
        if(e.code>=32 && e.code!=127 && du_utf8_encode(e.code,utf8))du_edit_insert(edit,utf8);
        ui.dirty=1;return true;
    }
    /* Desktop TEXT owns printable input, including AltGr (right Alt or
     * Ctrl+right Alt). KEY still owns editing, navigation and shortcuts. */
    bool altgr=ui.keys[DU_KEY_RALT];
    if(e.type==CIUKI_INPUT_KEY && e.value && edit &&
       (!(ui.modifiers&(DU_CTRL|DU_ALT)) || altgr) && du_keymap(DU_LAYOUT_US,e.code,0))return true;
    if(e.type==CIUKI_INPUT_WHEEL && !ui.dialog.kind && ui.capture<0 &&
       ui.mouse_y>=CIUKI_BROWSER_TOP && ui.mouse_y<(int)surface->current.info.height-CIUKI_BROWSER_STATUS)return false;
    if(e.type==CIUKI_INPUT_KEY && e.value && e.code==DU_KEY_TAB && du_focused(&ui)==URL && !(ui.modifiers&DU_SHIFT)) {
        toolbar_keys=false;ui.focus=-1;return false;
    }
    du_input(&ui,&e);return true;
}
void ciuki_chrome_file(struct gui_window *window,struct hlcache_handle *content,struct form_control *gadget)
{
    (void)content;
    if(!active)return;
    file_window=window;file_gadget=gadget;
    if(du_file_dialog(&ui,FILE_PICKER,0,"Choose a file","/Documents","",NULL,NULL))ciuki_chrome_status("File picker unavailable.");
}
static struct gui_download_window *refuse_download(struct download_context *context,struct gui_window *window)
{
    (void)context;(void)window;
    ciuki_chrome_status("Downloads are unavailable in this local browsing milestone.");return NULL;
}
static nserror refuse_data(struct gui_download_window *window,const char *data,unsigned int size)
{ (void)window;(void)data;(void)size;return NSERROR_NOT_IMPLEMENTED; }
static void download_error(struct gui_download_window *window,const char *message)
{ (void)window;ciuki_chrome_status(message); }
static void download_done(struct gui_download_window *window)
{ (void)window;ciuki_chrome_status("Downloads are unavailable in this local browsing milestone."); }
struct gui_download_table ciuki_download_table={.create=refuse_download,.data=refuse_data,.error=download_error,.done=download_done};
void ciuki_chrome_defaults(void)
{
    nsoption_set_bool(enable_javascript,false);
    nsoption_set_int(memory_cache_size,2*1024*1024);nsoption_set_uint(disc_cache_size,0);
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
        else if(!strncmp(a,"--ciuki-log=",12)) { ciuki_netsurf_log_path=a+12;consumed=true; }
        else if(!strncmp(a,"--ciuki-clock-scale=",20)) {
            /* f6-20 T23 model: charge host layouts as a slower CPU. */
            char *end;unsigned long n=strtoul(a+20,&end,10);if(*end || !n || n>1000000) { exit_reason="bad-arguments";return -1; }
            ciuki_layout_clock_scale=(uint32_t)n;consumed=true;
        }
#endif
        if(consumed) { memmove(argv+i,argv+i+1,(size_t)(*argc-i)*sizeof(*argv));(*argc)--; } else i++;
    }
    /* Before netsurf_init registers the fetchers, which create the network
     * with the trust anchors the profile selects. */
    ciuki_test_arm();
    return 0;
}
int ciuki_chrome_start(void)
{
    if(ciuki_capture_path && !profile_requested)return 0;
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
__attribute__((format(printf,3,4)))
static int format_line(char *line,size_t size,const char *format,...)
{ va_list ap;va_start(ap,format);int n=ciuki_line_vformat(line,size,format,ap);va_end(ap);return n; }
static int exit_line(char *line,size_t size,bool surface)
{
    struct ciuki_browser_surface *s=surface?ciuki_browser_active:NULL;
    return format_line(line,size,"[netsurf] exit reason=%s detail=%d layout_rejections=%u layout_max_us=%llu local_max_us=%llu remote_max_us=%llu input_overflows=%u keepalive_pumps=%u network=%s",
            exit_reason?exit_reason:"unknown",exit_detail,ciuki_layout_rejections,
            (unsigned long long)(ciuki_layout_max_ns/1000u),(unsigned long long)(ciuki_layout_local_max_ns/1000u),
            (unsigned long long)(ciuki_layout_remote_max_ns/1000u),s?s->overflows:0u,s?s->pumps:0u,
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
void ciuki_chrome_exit_report(void)
{
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
