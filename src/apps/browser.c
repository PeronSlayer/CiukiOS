/* CiukWeb: a CiukiOS desktop browser module. The interface and HTML text
 * rendering are native CAPP code. HTGET supplies HTTP until CiukiOS has its
 * own TCP client API; no MicroWeb process is involved. */
#include "app.h"

#define PAGE_PATH "C:\\NET\\CIUKWEB.HTM"
#define MAX_LINES 128
#define LINE_WIDTH 88
#define PAGE_BYTES 16384

static char url[76], command[128], message[96];
static char page[PAGE_BYTES];
static char lines[MAX_LINES][LINE_WIDTH + 1];
static char links[16][76];
static u8 line_link[MAX_LINES];
static int line_count, line_len, link_count, active_link, scroll, focused = 1;

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
    int split, n;
    char tail[LINE_WIDTH + 1];
    if (ch == '\r') return;
    if (ch == '\n') { if (line_len) new_line(); return; }
    if (ch == ' ' && (!line_len || lines[line_count][line_len - 1] == ' ')) return;
    if (line_len >= LINE_WIDTH - 1) {
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
    if (link_count >= 15) return;
    while (*p && !starts(p, "href")) ++p;
    if (!*p) return;
    p += 4;
    while (*p == ' ') ++p;
    if (*p++ != '=') return;
    while (*p == ' ') ++p;
    quote = *p == '\'' || *p == '"' ? *p++ : ' ';
    if (!starts(p, "http://")) return;  /* first version: absolute HTTP links */
    ++link_count;
    while (*p && *p != quote && *p != '>' && n < 75) links[link_count][n++] = *p++;
    links[link_count][n] = 0;
    active_link = link_count;
}
static void parse_page(int count)
{
    int i = 0, k, ch;
    char tag[160];
    line_count = line_len = link_count = active_link = scroll = 0;
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
            if (starts(tag, "script") || starts(tag, "style")) {
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
static void load_page(void)
{
    int h, n;
    h = dos_open(url[1] == ':' ? url : PAGE_PATH, 0);
    if (h < 0) { str_copy(message, "No page yet. Enter an HTTP address and select Go."); return; }
    n = dos_read(h, page, PAGE_BYTES - 16);
    dos_close(h);
    if (n <= 0) { str_copy(message, "The download is empty or unavailable."); return; }
    mem_set(page + n, 0, 16);
    parse_page(n);
    str_copy(message, "Loaded page. Select a link, or enter another HTTP address.");
    app_log("[CIUKWEB] rendered", url);
}
static void fetch(void)
{
    if (!starts(url, "http://")) {
        str_copy(message, "HTTP addresses only in this version.");
        return;
    }
    dos_delete(PAGE_PATH);
    str_copy(command, "C:\\NET\\HTGET.EXE -o ");
    str_cat(command, PAGE_PATH);
    str_cat(command, " ");
    str_cat(command, url);
    app_log("[CIUKWEB] fetch", url);
    app_command(command);
}
static void paint(void)
{
    int x = HOST.x + 4, y = HOST.y + TITLE_H, w = HOST.w - 8;
    int h = HOST.h - TITLE_H - 4, i, rows = (h - 116) / 18;
    if (rows < 1) rows = 1;
    ui_rect(x, y, w, h, C_FACE);
    ui_rect(x + 6, y + 8, w - 93, 29, focused ? C_PAPER : C_LIGHT);
    ui_inset(x + 6, y + 8, w - 93, 29);
    ui_text(x + 13, y + 15, url, C_INK);
    ui_button(x + w - 78, y + 8, 70, 29, "Go", 1);
    ui_text(x + 10, y + 45, message, C_INK);
    ui_inset(x + 7, y + 70, w - 14, h - 107);
    ui_rect(x + 10, y + 73, w - 20, h - 113, C_PAPER);
    for (i = 0; i < rows && scroll + i < line_count; ++i) {
        int n = scroll + i;
        ui_text(x + 17, y + 82 + i * 18, lines[n], line_link[n] ? C_BLUE : C_INK);
    }
    ui_text(x + 10, y + h - 28, "HTTP text and HTTP links  |  Up/Down: scroll  |  Ctrl+L: address", C_SHADOW);
}
static int key(int key)
{
    int ch = KEY_CHAR(key), scan = KEY_SCAN(key), len = str_len(url);
    if (scan == 0x48 || scan == 0x49) { scroll -= scan == 0x49 ? 8 : 1; if (scroll < 0) scroll = 0; return 1; }
    if (scan == 0x50 || scan == 0x51) { scroll += scan == 0x51 ? 8 : 1; if (scroll > line_count - 1) scroll = line_count > 0 ? line_count - 1 : 0; return 1; }
    if (ch == 12) { focused = 1; return 1; } /* Ctrl+L */
    if (ch == 13) { fetch(); return 1; }
    if (!focused) return 0;
    if (ch == 8 && len) { url[len - 1] = 0; return 1; }
    if (ch >= 32 && ch < 127 && len < 75) { url[len] = ch; url[len + 1] = 0; return 1; }
    return 0;
}
int app_event(int ev, int a, int b, int c)
{
    if (ev == EV_OPEN) {
        if (a == 2) return 1;
        str_copy(app_title, "CiukWeb");
        HDR_WIDTH = 720; HDR_HEIGHT = 500;
        if (starts(APP_ARG, "http://") || APP_ARG[1] == ':')
            str_ncopy(url, APP_ARG, sizeof url);
        else str_copy(url, "http://example.com/");
        line_count = scroll = 0;
        str_copy(message, "Enter an HTTP address and select Go. Start networking first.");
        if (a == 1 || url[1] == ':') load_page();
        return 1;
    }
    if (ev == EV_PAINT) { paint(); return 0; }
    if (ev == EV_KEY) return key(a);
    if (ev == EV_ACTION && a == 1) { fetch(); return 1; }
    if (ev == EV_MOUSE && a == MOUSE_DOWN) {
        int sx = HOST.x + b, sy = HOST.y + TITLE_H + c;
        int top = HOST.y + TITLE_H;
        if (sy >= top + 8 && sy < top + 37 && sx < HOST.x + HOST.w - 90) {
            focused = 1; return 1;
        }
        if (sy >= top + 82 && sy < top + HOST.h - TITLE_H - 33) {
            int n = scroll + (sy - top - 82) / 18;
            if (n >= 0 && n < line_count && line_link[n]) {
                str_copy(url, links[line_link[n]]);
                fetch(); return 1;
            }
        }
    }
    if (ev == EV_SUSPEND) { str_ncopy(APP_ARG, url, APP_ARG_BYTES); return 0; }
    return 0;
}
