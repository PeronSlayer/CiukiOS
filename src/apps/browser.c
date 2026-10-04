/* CiukWeb: desktop-native text browser over the cooperative native network. */
#include "app.h"
#include "webnet.h"

#define PAGE_PATH "C:\\NET\\CIUKWEB.HTM"
#define MAX_LINES 128
#define LINE_WIDTH 127
#define PAGE_BYTES 16384

static char url[128], page_url[128], message[96], page_title[64];
static char page[PAGE_BYTES];
static char lines[MAX_LINES][LINE_WIDTH + 1];
static char links[16][128];
static char back_history[8][128], forward_history[8][128];
static u8 line_link[MAX_LINES];
static int line_count, line_len, link_count, active_link, scroll, focused = 1;
static int back_count, forward_count, caret, replace_address, loaded_once;
static int loading;
static int page_bytes, cached_width, wrap_width;

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static int starts(const char *a, const char *b)
{
    while (*b) if (lower(*a++) != lower(*b++)) return 0;
    return 1;
}
static void new_line(void)
{
    if (line_count < MAX_LINES - 1) ++line_count;
    line_len = 0;
    lines[line_count][0] = 0;
}
static void put_char(int ch)
{
    int split, n, wraps=0;
    char tail[LINE_WIDTH + 1];
    if (ch == '\r') return;
    if (ch == '\n') { if (line_len) new_line(); return; }
    if (ch == ' ' && (!line_len || lines[line_count][line_len - 1] == ' ')) return;
    if(line_len<LINE_WIDTH) {
        char candidate[LINE_WIDTH+1];
        str_ncopy(candidate,lines[line_count],sizeof candidate);
        candidate[line_len]=(char)ch; candidate[line_len+1]=0;
        wraps=ui_measure(candidate)>wrap_width;
    }
    if (line_len >= LINE_WIDTH || wraps) {
        split = line_len - 1;
        while (split > 0 && lines[line_count][split] != ' ') --split;
        if (split > 0 && line_count < MAX_LINES - 1) {
            n = 0;
            while (lines[line_count][split + 1 + n]) {
                tail[n] = lines[line_count][split + 1 + n]; ++n;
            }
            tail[n] = 0;
            lines[line_count][split] = 0;
            new_line();
            str_copy(lines[line_count], tail);
            line_len = n;
        } else new_line();
    }
    if (line_count >= MAX_LINES) return;
    lines[line_count][line_len++] = ch;
    lines[line_count][line_len] = 0;
    if (active_link) line_link[line_count] = active_link;
}
static void block_break(void)
{
    if (line_len) new_line();
}
static void parse_link(const char *tag)
{
    const char *p = tag;
    int n = 0, quote;
    char href[128], base[128];
    if (link_count >= 15) return;
    while (*p && !starts(p, "href")) ++p;
    if (!*p) return;
    p += 4;
    while (*p == ' ') ++p;
    if (*p++ != '=') return;
    while (*p == ' ') ++p;
    quote = *p == '\'' || *p == '"' ? *p++ : ' ';
    while(*p && *p!=quote && *p!='>' && n<126) href[n++]=*p++;
    href[n]=0;
    for(n=0;href[n];++n) {
        if(href[n]=='&' && starts(href+n+1,"amp;")) {
            mem_move(href+n,href+n+5,str_len(href+n+5)+1);
        }
    }
    n=str_len(href);
    if(starts(href,"http://") || starts(href,"https://")) str_copy(base,href);
    else if(!href[0] || href[0]=='#') return;
    else {
        int i=7,host_end,dir_end=0,j=0;
        while(url[i] && url[i]!='/' && url[i]!='?' && url[i]!='#') ++i;
        host_end=i;
        if(href[0]=='/') dir_end=host_end;
        else { dir_end=str_len(url); while(dir_end>host_end && url[dir_end-1]!='/') --dir_end; }
        while(j<dir_end && j<120) { base[j]=url[j]; ++j; }
        if(href[0]=='/' && j<120) base[j++]='/';
        for(i=(href[0]=='/'?1:0);href[i] && j<126;++i) base[j++]=href[i];
        base[j]=0;
    }
    ++link_count;
    str_ncopy(links[link_count],base,sizeof links[link_count]);
    active_link = link_count;
}
static void parse_page(int count)
{
    int i = 0, k, ch;
    char tag[160];
    line_count = line_len = link_count = active_link = scroll = 0;
    page_title[0]=0;
    mem_set(line_link, 0, sizeof line_link);
    lines[0][0] = 0;
    while (i < count && line_count < MAX_LINES - 1) {
        ch = (u8)page[i++];
        if (ch == '<') {
            k = 0;
            while (i < count && page[i] != '>') {
                if (k < sizeof tag - 1) tag[k++] = page[i];
                ++i;
            }
            if (i < count) ++i;
            tag[k] = 0;
            if (starts(tag,"title")) {
                int t=0;
                while(i<count && !starts(page+i,"</title>")) {
                    ch=(u8)page[i++];
                    if(ch>=32 && ch<127 && t<62) page_title[t++]=(char)ch;
                }
                if(i<count) while(i<count && page[i++]!='>') { }
                page_title[t]=0;
            } else if (starts(tag, "script") || starts(tag, "style")) {
                const char *end = starts(tag, "script") ? "</script>" : "</style>";
                while (i < count && !starts(page + i, end)) ++i;
                while (i < count && page[i++] != '>') { }
            } else if (starts(tag, "a ")) parse_link(tag);
            else if (starts(tag, "/a")) active_link = 0;
            else if (starts(tag, "br") || starts(tag, "p") || starts(tag, "/p") ||
                     starts(tag, "div") || starts(tag, "/div") || starts(tag, "h1") ||
                     starts(tag, "/h1") || starts(tag, "h2") || starts(tag, "/h2") ||
                     starts(tag, "li")) block_break();
        } else if (ch == '&') {
            if (starts(page + i, "amp;")) { ch = '&'; i += 4; }
            else if (starts(page + i, "lt;")) { ch = '<'; i += 3; }
            else if (starts(page + i, "gt;")) { ch = '>'; i += 3; }
            else if (starts(page + i, "quot;")) { ch = '"'; i += 5; }
            else if (starts(page + i, "nbsp;")) { ch = ' '; i += 5; }
            put_char(ch);
        } else if (ch == '\t' || ch == '\n') put_char(' ');
        else if (ch >= 32 && ch <= 126) put_char(ch);
    }
    if (line_len && line_count < MAX_LINES - 1) ++line_count;
}
static void render_page(int keep_scroll)
{
    int old=scroll;
    wrap_width=HOST.w-42;
    if(wrap_width<100) wrap_width=100;
    parse_page(page_bytes);
    if(keep_scroll) {
        scroll=old;
        if(scroll>line_count-1) scroll=line_count>0?line_count-1:0;
    }
    cached_width=HOST.w;
}
static void draw_fit_text(int x,int y,int width,const char *s,int color)
{
    char text[128]; int n=str_len(s);
    if(n>127) n=127;
    str_ncopy(text,s,n+1);
    while(n>0 && ui_measure(text)>width) text[--n]=0;
    ui_text(x,y,text,color);
}
static void paint_address(int x,int y,int width)
{
    char view[128],prefix[128];
    int start=0,n,visible_caret=0;
    if(replace_address) { draw_fit_text(x,y,width,url,C_INK); return; }
    while(start<caret) {
        str_ncopy(view,url+start,sizeof view);
        if(ui_measure(view)<=width-6) break;
        ++start;
    }
    str_ncopy(view,url+start,sizeof view);
    n=str_len(view);
    while(n>0 && ui_measure(view)>width-6) view[--n]=0;
    if(caret>=start) {
        str_ncopy(prefix,url+start,(caret-start)+1);
        visible_caret=ui_measure(prefix);
    }
    ui_text(x,y,view,C_INK);
    if(focused) ui_text(x+visible_caret,y,"|",C_BLUE);
}
static void load_page(void)
{
    int h, n;
    h = dos_open(url[1] == ':' ? url : PAGE_PATH, 0);
    if (h < 0) { str_copy(message, "No page yet. Enter an HTTP address and select Go."); return; }
    n = dos_read(h, page, PAGE_BYTES - 16);
    dos_close(h);
    if (n <= 0) { str_copy(message, "The download is empty or unavailable."); return; }
    mem_set(page + n, 0, 16);
    page_bytes=n; render_page(0);
    str_copy(message, "Loaded page. Select a link, or enter another HTTP address.");
    loaded_once=1; str_ncopy(page_url,url,sizeof page_url);
    app_log("[CIUKWEB] rendered", url);
}
static void load_download(void)
{
    int n=webnet_read();
    char stats[40];
    loading=0;
    if(n<=0) { str_copy(message,"The HTTP response was empty or could not be parsed."); return; }
    mem_set(page+n,0,1); page_bytes=n; render_page(0); loaded_once=1; str_ncopy(page_url,url,sizeof page_url);
    if(webnet_http_status()<200 || webnet_http_status()>=400) {
        str_copy(message,"HTTP response received (status code ");
        fmt_u32(message+str_len(message), (u32)webnet_http_status());
        str_cat(message,"). Select a link or enter another address.");
    } else if(page_title[0]) str_ncopy(message,page_title,sizeof message);
    else str_copy(message,"Page loaded. Select a link, or enter another address.");
    str_copy(stats,"status="); fmt_u32(stats+str_len(stats),(u32)webnet_http_status());
    str_cat(stats," bytes="); fmt_u32(stats+str_len(stats),(u32)n);
    app_log("[CIUKWEB] HTTP",stats);
    app_log("[CIUKWEB] rendered",url);
}
static void history_push(char hist[8][128],int *count,const char *value)
{
    int i;
    if(!value[0]) return;
    if(*count==8) { for(i=1;i<8;++i) str_copy(hist[i-1],hist[i]); --*count; }
    str_ncopy(hist[*count],value,128); ++*count;
}
static void fetch(void);
static void navigate(const char *target,int add_history)
{
    if(add_history && loaded_once && str_cmp(page_url,target)) {
        history_push(back_history,&back_count,page_url); forward_count=0;
    }
    if(target!=url) str_ncopy(url,target,sizeof url);
    caret=str_len(url); replace_address=0;
    fetch();
}
static void history_go(int back)
{
    char target[128];
    if(back && back_count) {
        history_push(forward_history,&forward_count,page_url);
        str_copy(target,back_history[--back_count]); navigate(target,0);
    } else if(!back && forward_count) {
        history_push(back_history,&back_count,page_url);
        str_copy(target,forward_history[--forward_count]); navigate(target,0);
    }
}
static void fetch(void)
{
    int result;
    if (loading) webnet_cancel();
    result=webnet_start(url,page,PAGE_BYTES-16);
    if(result==WEBNET_PENDING) {
        loading=1; str_copy(message,"Resolving host and connecting...");
        app_log("[CIUKWEB] fetch",url);
    } else str_copy(message,webnet_error());
}
static void paint(void)
{
    int x = HOST.x + 4, y = HOST.y + TITLE_H, w = HOST.w - 8;
    int h = HOST.h - TITLE_H - 4, i, rows = (h - 116) / 18;
    if(page_bytes>0 && cached_width!=HOST.w) render_page(1);
    if (rows < 1) rows = 1;
    ui_rect(x, y, w, h, C_FACE);
    ui_button(x + 6, y + 8, 24, 29, "<", 2);
    ui_button(x + 32, y + 8, 24, 29, ">", 3);
    ui_button(x + 58, y + 8, 26, 29, "R", 4);
    ui_button(x + 86, y + 8, 26, 29, "X", 5);
    ui_rect(x + 116, y + 8, w - 198, 29, focused ? C_PAPER : C_LIGHT);
    ui_inset(x + 116, y + 8, w - 198, 29);
    paint_address(x + 121, y + 15, w - 205);
    ui_button(x + w - 78, y + 8, 70, 29, "Go", 1);
    draw_fit_text(x + 10, y + 45, w - 20, message, C_INK);
    ui_inset(x + 7, y + 70, w - 14, h - 107);
    ui_rect(x + 10, y + 73, w - 20, h - 113, C_PAPER);
    for (i = 0; i < rows && scroll + i < line_count; ++i) {
        int n = scroll + i;
        ui_text(x + 17, y + 82 + i * 18, lines[n], line_link[n] ? C_BLUE : C_INK);
    }
    draw_fit_text(x + 10, y + h - 28, w - 20, "Links: Enter  Wheel: scroll  Ctrl+L: URL  Esc: stop/close", C_SHADOW);
}
static int key(int key)
{
    int ch = KEY_CHAR(key), scan = KEY_SCAN(key), len = str_len(url), i;
    if(ch==27) {
        if(loading) { webnet_cancel(); loading=0; str_copy(message,"Request stopped."); return 1; }
        return 0;
    }
    if (scan == 0x48 || scan == 0x49) { scroll -= scan == 0x49 ? 8 : 1; if (scroll < 0) scroll = 0; return 1; }
    if (scan == 0x50 || scan == 0x51) { scroll += scan == 0x51 ? 8 : 1; if (scroll > line_count - 1) scroll = line_count > 0 ? line_count - 1 : 0; return 1; }
    if (ch == 12) { focused = 1; caret=str_len(url); replace_address=1; return 1; } /* Ctrl+L */
    if (ch == 18) { navigate(page_url[0]?page_url:url,0); return 1; } /* Ctrl+R */
    if (ch == 13) { navigate(url,1); return 1; }
    if (!focused) return 0;
    if(scan==0x4B && caret>0) { --caret; return 1; }
    if(scan==0x4D && caret<len) { ++caret; return 1; }
    if(scan==0x47) { caret=0; return 1; }
    if(scan==0x4F) { caret=len; return 1; }
    if(ch==8 && caret>0) { mem_move(url+caret-1,url+caret,len-caret+1); --caret; return 1; }
    if(scan==0x53 && caret<len) { mem_move(url+caret,url+caret+1,len-caret); return 1; }
    if (ch >= 32 && ch < 127 && len < 126) {
        if(replace_address) { url[0]=0; len=caret=0; replace_address=0; }
        for(i=len;i>=caret;--i) url[i+1]=url[i];
        url[caret++]=(char)ch; return 1;
    }
    return 0;
}
int app_event(int ev, int a, int b, int c)
{
    if (ev == EV_OPEN) {
        int open_url;
        if (a == 2) return 1;
        str_copy(app_title, "CiukWeb");
        HDR_WIDTH = 720; HDR_HEIGHT = 500;
        open_url=starts(APP_ARG,"http://") || starts(APP_ARG,"https://");
        if (starts(APP_ARG, "http://") || starts(APP_ARG,"https://") || APP_ARG[1] == ':')
            str_ncopy(url, APP_ARG, sizeof url);
        else str_copy(url, "http://example.com/");
        line_count = scroll = page_bytes = cached_width = 0;
        back_count=forward_count=loaded_once=0; page_url[0]=0; caret=str_len(url); replace_address=0;
        loading=0;
        str_copy(message, "Enter a web address and select Go. Start networking first.");
        if (a == 1 || url[1] == ':') {
            if(url[1]==':') load_page(); else navigate(url,0);
        } else if(open_url) navigate(url,0);
        return 1;
    }
    if (ev == EV_PAINT) { paint(); return 0; }
    if (ev == EV_POLL && loading) {
        int net=webnet_poll();
        if(net==WEBNET_COMPLETE) { load_download(); return 1; }
        if(net==WEBNET_FAILED) { loading=0; str_copy(message,webnet_error()); return 1; }
        return 0;
    }
    if (ev == EV_KEY) return key(a);
    if (ev == EV_ACTION) {
        if(a==1) { navigate(url,1); return 1; }
        if(a==2) { history_go(1); return 1; }
        if(a==3) { history_go(0); return 1; }
        if(a==4) { navigate(page_url[0]?page_url:url,0); return 1; }
        if(a==5) { if(loading) { webnet_cancel(); loading=0; str_copy(message,"Request stopped."); } return 1; }
    }
    if (ev == EV_WHEEL) {
        scroll += a * 3;
        if(scroll<0) scroll=0;
        if(scroll>line_count-1) scroll=line_count>0?line_count-1:0;
        return 1;
    }
    if (ev == EV_MOUSE && a == MOUSE_DOWN) {
        int sx = HOST.x + b, sy = HOST.y + TITLE_H + c;
        int top = HOST.y + TITLE_H;
        int left=HOST.x+4;
        if(sy>=top+8 && sy<top+37) {
            if(sx>=left+116 && sx<left+HOST.w-94) {
                focused=1; caret=str_len(url); replace_address=0; return 1;
            }
        }
        if (sy >= top + 82 && sy < top + HOST.h - TITLE_H - 33) {
            int n = scroll + (sy - top - 82) / 18;
            if (n >= 0 && n < line_count && line_link[n]) {
                navigate(links[line_link[n]],1); return 1;
            }
        }
    }
    if (ev == EV_SUSPEND) { if(loading) webnet_cancel(); loading=0; str_ncopy(APP_ARG, url, APP_ARG_BYTES); return 0; }
    if (ev == EV_CLOSE && loading) { webnet_cancel(); loading=0; }
    return 0;
}
