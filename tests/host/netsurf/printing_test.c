/* SPDX-License-Identifier: GPL-2.0-only
 * Production browser shortcut/crop boundary; unrelated frontend is discarded. */
#include "../../../sdk/netsurf/port/chrome.c"
#include <assert.h>
/* ASan retains the exported download table when chrome.c is included.
 * This printing-only fixture must never enter the browser download core. */
const char *download_context_get_filename(const download_context *context)
{ (void)context;abort(); }
void download_context_destroy(download_context *context)
{ (void)context;abort(); }
fbtk_widget_t *fbtk;
void fbtk_request_redraw(fbtk_widget_t *w) { (void)w; }
void du_dialog_close(struct du_ui *u) { (void)u; }
static unsigned prints,setups,launches;
static unsigned sent_opcode,sent_bytes;
static uint8_t sent_payload[240];
static uint32_t token=7;
static const char *launch_url="https://ciuki.example/new";
const char *nsurl_access(const struct nsurl *url) { assert(url);return launch_url; }
uint32_t du3_request(struct du3_connection *connection,unsigned opcode,uint32_t w,uint32_t generation,
                     const void *payload,unsigned bytes,int fd)
{
    assert(connection==&surface->connection && w==surface->window && generation==surface->generation && fd==-1);
    assert(bytes<=sizeof(sent_payload));memcpy(sent_payload,payload,bytes);sent_bytes=bytes;sent_opcode=opcode;launches++;return token;
}
void cp_ui_setup(struct cp_ui *p,struct du_ui *u)
{ assert(p==&printing && u==&ui);setups++; }
struct gui_window *window_list;
void ciuki_print_document(struct cp_ui *p,struct du_ui *u,struct browser_window *bw)
{ assert(p==&printing && u==&ui && bw==window_list->bw);prints++; }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);static uint32_t pixels[640*480];
    static struct ciuki_browser_surface browser_surface;surface=&browser_surface;
    surface->ready=true;surface->current.back=pixels;surface->current.info.width=640;surface->current.info.height=480;
    struct gui_window window={.bw=(struct browser_window *)1};window_list=&window;
    assert(!print_key(0x19) && !prints && !setups);
    ui.modifiers=DU_CTRL;assert(!print_key(0x18));assert(print_key(0x19) && prints==1);
    ui.modifiers|=DU_SHIFT;assert(print_key(0x19) && setups==1 && prints==1);
    ui.modifiers=DU_CTRL;surface->ready=false;assert(print_key(0x19) && prints==1);
    surface->ready=true;surface->current.info.height=20;assert(print_key(0x19) && prints==2);
    du_init(&ui,pixels,640,480);du_add(&ui,DU_LABEL,STATUS,(struct du_rect){0,0,640,20},"");active=true;
    assert(!ciuki_chrome_new_window(NULL));assert(ciuki_chrome_new_window((void *)1) && launches==1);
    assert(sent_opcode==DESK_LAUNCH && sent_bytes==4+strlen(launch_url) && desk_get32(sent_payload)==strlen(launch_url));
    assert(!memcmp(sent_payload+4,launch_url,strlen(launch_url)));
    struct desk_packet answer={.request=token,.index=DESK_LAUNCH,.result=-ENOMEM};
    ciuki_chrome_launch_result(&answer);assert(!launch_request && strstr(du_get(&ui,STATUS)->text,"could not open"));
    token=0;assert(!ciuki_chrome_new_window((void *)1) && launches==2);token=8;
    static char long_url[DESK_ARG_MAX+2];memset(long_url,'a',sizeof(long_url)-1);launch_url=long_url;
    assert(!ciuki_chrome_new_window((void *)1) && launches==2);
    strcpy(saved_download,"/Documents/a file.txt");assert(open_download(1));
    assert(sent_opcode==DESK_V3_LAUNCH && desk_get32(sent_payload)==5 && !memcmp(sent_payload+4,"files",5));
    assert(desk_get32(sent_payload+9)==strlen("--file-action=348:/Documents/a file.txt"));
    assert(!memcmp(sent_payload+13,"--file-action=348:/Documents/a file.txt",sent_bytes-13));
    assert(open_download(0) && !memcmp(sent_payload+13,"--open=/Documents/a file.txt",sent_bytes-13));
    strcpy(saved_download,"/Documents/../bad");assert(!open_download(1));
    assert(!desk_browser_file_argument("--file-action=340:/Documents/file") && !desk_browser_file_argument("--open=relative"));
    assert(!desk_browser_file_argument("--open=/Documents//file") && !desk_browser_file_argument("--open=/Documents/./file"));
    puts("PASS G26C launch: automatic-window frontend dispatch, exact URL, long URL/send/late error, saved-file Open/Open With and traversal rejection");
    puts("PASS G31 NetSurf: Ctrl+P document dispatch, Ctrl+Shift+P setup, readiness and viewport-independent dispatch");
    return 0;
}
