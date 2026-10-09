/* Control Panel: every system option in one place, as in Windows. The
 * main window shows the applets; each applet opens in a window of its own
 * (several can be open at once). Display, Wallpaper and Task Manager are
 * the desktop's own windows; Device Manager and Drivers are DEVICES.APP.
 * Settings are kept in \SYSTEM\UI\DESKTOP.CFG, which the shell applies at
 * once and at every start. */
#include "app.h"
#include "audio_settings.h"
#include "regstore.h"

static u8 inb(u16 port);
#pragma aux inb = "in al,dx" parm [dx] value [al];
static u16 inw(u16 port);
#pragma aux inw = "in ax,dx" parm [dx] value [ax];
static void outw(u16 port, u16 v);
#pragma aux outw = "out dx,ax" parm [dx] [ax];

enum { P_DISPLAY, P_WALLPAPER, P_APPEARANCE, P_FONTS, P_SOUND, P_MOUSE, P_KEYBOARD,
       P_DATETIME, P_SYSTEM, P_DEVICES, P_DRIVERS, P_TASKS, P_REGISTRY, P_NETWORK, P_COUNT };
struct appletdef { const char *label; const char *desc; int icon; const char *key; };
static const struct appletdef applets[P_COUNT] = {
    { "Display", "Monitor, resolution and display drivers.", ICON_DISPLAY, "display" },
    { "Wallpaper", "The picture on the desktop.", ICON_WALLPAPER, "wallpaper" },
    { "Appearance", "Colour schemes and the desktop icons.", ICON_THEME, "appearance" },
    { "Fonts", "Installed fonts, the system font, new fonts.", ICON_FONTS, "fonts" },
    { "Sound", "Startup sound, event sounds and volume.", ICON_SOUND, "sound" },
    { "Mouse", "Pointer speed, double-click speed, buttons.", ICON_MOUSE, "mouse" },
    { "Keyboard", "Key repeat delay and rate.", ICON_KEYBOARD, "keyboard" },
    { "Date and Time", "The computer's date and time.", ICON_CALENDAR, "datetime" },
    { "System", "About this computer and CiukiOS.", ICON_ABOUT, "system" },
    { "Device Manager", "The devices of this computer.", ICON_DEVICES, "devices" },
    { "Drivers", "Installed drivers; add new drivers.", ICON_PACKAGE, "drivers" },
    { "Task Manager", "Programs, processes and performance.", ICON_MONITOR, "tasks" },
    { "Settings Registry", "Application and system key/value settings.", ICON_PACKAGE, "registry" },
    { "Network", "IPv4, advanced settings and network adapters.", ICON_NETWORK, "network" } };

static struct deskcfg cfg, saved_cfg;
static char msg[160];
static char status[80];
static int an[P_COUNT];
static struct dctl *add(int k, int type, int x, int y, int w, int h, const char *text, int id);
static void show(int k, const char *title, int w, int h);
static void message(const char *title, const char *text);

/* Network settings share mTCP's profile. Keep unrelated mTCP options intact. */
enum { NET_IP, NET_MASK, NET_GATEWAY, NET_DNS, NET_HOST, NET_MTU, NET_FIELDS };
static const char *net_keys[NET_FIELDS] = {
    "IPADDR", "NETMASK", "GATEWAY", "NAMESERVER", "HOSTNAME", "MTU" };
static char net_values[NET_FIELDS][33];
static struct field net_fields[NET_FIELDS];
static char net_config[2048];
static char net_output[2048];
static char net_status[80];
static char net_hardware[64], net_pci[80], net_packet[64], net_address[48];
static int net_present;
static void net_read(void)
{
    int h, n, i;
    char *p, *q;
    for (i = 0; i < 4; i++) str_copy(net_values[i], "0.0.0.0");
    str_copy(net_values[NET_HOST], "ciukios");
    str_copy(net_values[NET_MTU], "1500");
    net_present = 0;
    h = dos_open("C:\\NET\\MTCP.CFG", 0);
    if (h < 0) return;
    n = dos_read(h, net_config, sizeof net_config - 1);
    dos_close(h);
    if (n < 0 || n >= sizeof net_config - 1) return;
    net_present = 1;
    net_config[n] = 0;
    p = net_config;
    while (*p) {
        q = p;
        while (*q == ' ' || *q == '\t') q++;
        for (i = 0; i < NET_FIELDS; i++) {
            int len = str_len(net_keys[i]), j = 0;
            if (str_nicmp(q, net_keys[i], len) || (q[len] != ' ' && q[len] != '\t')) continue;
            q += len;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != '\r' && *q != '\n' && *q != ' ' && *q != '\t' && j < 32)
                net_values[i][j++] = *q++;
            net_values[i][j] = 0;
            break;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}
static int net_ipv4(const char *p)
{
    int part, v, digits;
    for (part = 0; part < 4; part++) {
        v = digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) { v = v * 10 + *p++ - '0'; digits++; }
        if (!digits || v > 255) return 0;
        if (part < 3) { if (*p++ != '.') return 0; }
        else if (*p) return 0;
    }
    return 1;
}
static int net_pci_word(int bus, int devfn, int reg, u16 *v)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0xB109; r.bx = (u16)((bus << 8) | devfn); r.di = (u16)reg;
    r.ds = r.es = app_seg();
    if (intr(0x1A, &r) || (r.ax & 0xFF00)) return 0;
    *v = r.cx;
    return 1;
}
static void net_hex2(char *p, u8 v)
{
    static const char hex[] = "0123456789ABCDEF";
    p[0] = hex[v >> 4]; p[1] = hex[v & 15]; p[2] = 0;
}
static const char *net_pci_name(u16 vendor, u16 device)
{
    if (vendor == 0x10EC && device == 0x8139) return "Realtek RTL8139 Ethernet";
    if (vendor == 0x10EC && device == 0x8029) return "Realtek RTL8029 Ethernet";
    if (vendor == 0x1022 && device == 0x2000) return "AMD PCnet Ethernet";
    if (vendor == 0x8086 && device == 0x100E) return "Intel PRO/1000 MT Ethernet";
    if (vendor == 0x8086 && device == 0x10D3) return "Intel 82574L Ethernet";
    if (vendor == 0x8086) return "Intel network controller";
    if (vendor == 0x10B7) return "3Com network controller";
    if (vendor == 0x1106) return "VIA network controller";
    if (vendor == 0x1039) return "SiS network controller";
    return "PCI network controller";
}
static void net_scan(void)
{
    struct regs r;
    int bus, dev, fn, last_bus, vec, i;
    u16 vendor, device, cls, irq, bar;
    u8 mac[6];
    char t[12];
    str_copy(net_hardware, "Hardware: no PCI network adapter found");
    str_copy(net_pci, "PCI: an ISA adapter may still use a packet driver");
    str_copy(net_packet, "Packet API: no active physical driver");
    str_copy(net_address, "MAC address: unavailable");
    mem_set(&r, 0, sizeof r);
    r.ax = 0xB101; r.ds = r.es = app_seg();
    if (!intr(0x1A, &r) && !(r.ax & 0xFF00)) {
        last_bus = r.cx & 0xFF;
        for (bus = 0; bus <= last_bus && bus < 8; bus++) {
            for (dev = 0; dev < 32; dev++) {
                for (fn = 0; fn < 8; fn++) {
                    int devfn = (dev << 3) | fn;
                    if (!net_pci_word(bus, devfn, 0, &vendor) || vendor == 0xFFFF) {
                        if (!fn) break;
                        continue;
                    }
                    if (!net_pci_word(bus, devfn, 0x0A, &cls) || (cls >> 8) != 2) continue;
                    device = irq = bar = 0;
                    net_pci_word(bus, devfn, 2, &device);
                    net_pci_word(bus, devfn, 0x3C, &irq);
                    net_pci_word(bus, devfn, 0x10, &bar);
                    str_copy(net_hardware, "Hardware: ");
                    str_cat(net_hardware, net_pci_name(vendor, device));
                    str_copy(net_pci, "PCI ID ");
                    fmt_hex4(t, vendor); str_cat(net_pci, t); str_cat(net_pci, ":");
                    fmt_hex4(t, device); str_cat(net_pci, t);
                    if ((irq & 0xFF) < 16) {
                        str_cat(net_pci, "  IRQ ");
                        fmt_u32(t, irq & 0xFF); str_cat(net_pci, t);
                    }
                    if (bar & 1) {
                        str_cat(net_pci, "  I/O ");
                        fmt_hex4(t, bar & 0xFFFC); str_cat(net_pci, t);
                    }
                    goto pci_done;
                }
            }
        }
    }
pci_done:
    for (vec = 0x60; vec <= 0x6F; vec++) {
        u16 seg = peek16(0, vec * 4 + 2), off = peek16(0, vec * 4);
        if (!seg || peek8(seg, off + 3) != 'P' || peek8(seg, off + 4) != 'K' ||
            peek8(seg, off + 5) != 'T') continue;
        str_copy(net_packet, "Packet API: INT ");
        net_hex2(t, (u8)vec); str_cat(net_packet, t);
        str_cat(net_packet, "h active");
        mem_set(&r, 0, sizeof r);
        r.ax = 0x0600; r.cx = sizeof mac; r.ds = r.es = app_seg(); r.di = (u16)mac;
        if (!intr(vec, &r) && r.cx == sizeof mac) {
            str_copy(net_address, "MAC address: ");
            for (i = 0; i < 6; i++) {
                if (i) str_cat(net_address, ":");
                net_hex2(t, mac[i]); str_cat(net_address, t);
            }
        }
        break;
    }
    app_log("[CONTROL] network adapter", net_hardware);
}
static void network_open(void)
{
    int k = P_NETWORK, i;
    int compact = HOST.screen_h < 600;
    int adapter_y = compact ? 204 : 244;
    int button_y = compact ? 294 : 373;
    static const int lx[4] = { 24, 24, 284, 284 };
    static const int fx[4] = { 130, 130, 370, 370 };
    static const int fy[4] = { 34, 72, 34, 72 };
    static const char *labels[4] = { "IP address", "Subnet mask", "Gateway", "DNS server" };
    net_read();
    net_scan();
    str_copy(net_status, net_present ? "Apply saves the profile. Driver changes need a restart."
                                     : "Network tools are not installed on this image.");
    an[k] = 0;
    add(k, DC_GROUP, 8, 8, 528, 112, "IPv4 profile", 0);
    for (i = 0; i < 4; i++) {
        add(k, DC_LABEL, lx[i], fy[i] + 5, 0, 16, labels[i], 0);
        field_set(&net_fields[i], net_values[i], sizeof net_values[i], net_values[i]);
        add(k, DC_FIELD, fx[i], fy[i], 145, 23, 0, 0)->field = &net_fields[i];
    }
    add(k, DC_GROUP, 8, compact ? 124 : 129, 528, compact ? 74 : 105, "Advanced", 0);
    add(k, DC_LABEL, 24, compact ? 150 : 158, 0, 16, "Host name", 0);
    field_set(&net_fields[NET_HOST], net_values[NET_HOST], sizeof net_values[NET_HOST], net_values[NET_HOST]);
    add(k, DC_FIELD, 130, compact ? 145 : 153, 211, 23, 0, 0)->field = &net_fields[NET_HOST];
    add(k, DC_LABEL, 362, compact ? 150 : 158, 0, 16, "MTU", 0);
    field_set(&net_fields[NET_MTU], net_values[NET_MTU], sizeof net_values[NET_MTU], net_values[NET_MTU]);
    add(k, DC_FIELD, 410, compact ? 145 : 153, 105, 23, 0, 0)->field = &net_fields[NET_MTU];
    if (!compact) add(k, DC_LABEL, 24, 193, 0, 16,
                      "Host name: letters, numbers, hyphens. Ethernet MTU: 576-1500.", 0);
    add(k, DC_GROUP, 8, adapter_y, 528, compact ? 120 : 165, "Network adapter", 0);
    add(k, DC_LABEL, 24, adapter_y + (compact ? 16 : 26), 0, 16, net_hardware, 0);
    add(k, DC_LABEL, 24, adapter_y + (compact ? 34 : 49), 0, 16, net_pci, 0);
    add(k, DC_LABEL, 24, adapter_y + (compact ? 52 : 72), 0, 16, net_packet, 0);
    add(k, DC_LABEL, 24, adapter_y + (compact ? 70 : 95), 0, 16, net_address, 0);
    add(k, DC_BUTTON, 24, button_y, 95, 25, "&Rescan", 10);
    add(k, DC_BUTTON, 125, button_y, 174, 25, "&Device Manager...", 11);
    add(k, DC_BUTTON, 305, button_y, 115, 25, "D&rivers...", 12);
    add(k, DC_BUTTON, 426, button_y, 90, 25, "D&HCP...", 13);
    add(k, DC_LABEL, 16, compact ? 329 : 421, 0, 16, net_status, 0);
    add(k, DC_BUTTON, 352, compact ? 350 : 448, 84, 26, "&Apply", 3)->disabled = !net_present;
    add(k, DC_BUTTON, 442, compact ? 350 : 448, 84, 26, "Close", 2);
    show(k, "Network", 552, compact ? 390 : 506);
}
static int net_upsert(const char *key, const char *value)
{
    char *p = net_output;
    int key_len = str_len(key), value_len = str_len(value);
    while (*p) {
        char *q = p, *end;
        while (*q == ' ' || *q == '\t') q++;
        if (!str_nicmp(q, key, key_len) && (q[key_len] == ' ' || q[key_len] == '\t')) {
            q += key_len;
            while (*q == ' ' || *q == '\t') q++;
            end = q;
            while (*end && *end != '\r' && *end != '\n' && *end != ' ' && *end != '\t') end++;
            if (str_len(net_output) - (end - q) + value_len >= sizeof net_output) return 0;
            mem_move(q + value_len, end, str_len(end) + 1);
            mem_copy(q, value, value_len);
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    {
        int n = str_len(net_output);
        int extra = key_len + value_len + 6;
        if (n + extra >= sizeof net_output) return 0;
        if (n && net_output[n - 1] != '\n') str_cat(net_output, "\r\n");
        str_cat(net_output, key); str_cat(net_output, " ");
        str_cat(net_output, value); str_cat(net_output, "\r\n");
    }
    return 1;
}
static int net_hostname_valid(const char *p)
{
    int i, n = str_len(p);
    if (n < 1 || n > 32 || p[0] == '-' || p[n - 1] == '-') return 0;
    for (i = 0; i < n; i++) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}
static int net_mtu_valid(const char *p)
{
    int n = 0;
    if (!*p) return 0;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + *p++ - '0';
        if (n > 1500) return 0;
    }
    return n >= 576;
}
static int net_mask_valid(const char *p)
{
    u32 mask = 0, zeroes;
    int i;
    if (!net_ipv4(p)) return 0;
    for (i = 0; i < 4; i++) {
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
        mask = (mask << 8) | (u8)v;
        if (*p == '.') p++;
    }
    zeroes = ~mask;
    return (zeroes & (zeroes + 1)) == 0;
}
static void net_update_resident(void)
{
    u16 seg = peek16(0, 0x61 * 4 + 2), off = peek16(0, 0x61 * 4);
    u8 ip[4];
    const char *p = net_values[0];
    struct regs r;
    int i;
    if (!seg || peek8(seg, off + 3) != 'P' || peek8(seg, off + 4) != 'K' ||
        peek8(seg, off + 5) != 'T') return;
    for (i = 0; i < 4; i++) {
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
        ip[i] = (u8)v;
        if (*p == '.') p++;
    }
    mem_set(&r, 0, sizeof r);
    r.ax = 0xFE01; r.ds = r.es = app_seg(); r.si = (u16)ip;
    intr(0x61, &r);
}
static void network_apply(void)
{
    int i, h, len, written;
    const char *src = "C:\\NET\\MTCP.CFG", *tmp = "C:\\NET\\MTCP.NEW", *bak = "C:\\NET\\MTCP.BAK";
    for (i = 0; i < 4; i++) if (!net_ipv4(net_values[i])) {
        message("Network", "Enter four IPv4 addresses, each with four numbers from 0 to 255.");
        return;
    }
    if (!net_mask_valid(net_values[NET_MASK])) {
        message("Network", "The subnet mask must have contiguous network bits."); return;
    }
    if (!net_hostname_valid(net_values[NET_HOST])) {
        message("Network", "The host name must use 1-32 letters, numbers or hyphens."); return;
    }
    if (!net_mtu_valid(net_values[NET_MTU])) {
        message("Network", "The Ethernet MTU must be from 576 to 1500."); return;
    }
    if (!net_present) { message("Network", "Install the network tools before saving settings."); return; }
    str_copy(net_output, net_config);
    for (i = 0; i < NET_FIELDS; i++) if (!net_upsert(net_keys[i], net_values[i])) {
        message("Network", "The network profile is too large to update safely."); return;
    }
    if (!net_upsert("HOSTNAME_ASSIGNED", net_values[NET_HOST])) {
        message("Network", "The network profile is too large to update safely."); return;
    }
    len = str_len(net_output);
    h = dos_create(tmp);
    if (h < 0) { message("Network", "Cannot create the temporary network profile."); return; }
    written = dos_write(h, net_output, len);
    if (dos_close(h) < 0 || written != len) {
        dos_delete(tmp); message("Network", "Cannot finish writing the network profile."); return;
    }
    dos_delete(bak);
    if (dos_rename(src, bak) < 0) {
        dos_delete(tmp); message("Network", "Cannot replace the network profile."); return;
    }
    if (dos_rename(tmp, src) < 0) {
        dos_rename(bak, src);
        dos_delete(tmp);
        message("Network", "Cannot install the new network profile."); return;
    }
    dos_delete(bak);
    str_copy(net_config, net_output);
    net_update_resident();
    str_copy(net_status, "Saved. New programs use this profile; resident IP refreshed.");
    app_log("[CONTROL] network saved", net_values[0]);
}

/* ------------------------------------------------------------------ */
/* Applet windows                                                      */
#define MAXC 27
static struct dialog ad[P_COUNT];
static struct dctl ac[P_COUNT][MAXC];
static struct dctl *add(int k, int type, int x, int y, int w, int h, const char *text, int id)
{
    struct dctl *c = &ac[k][an[k]++];
    mem_set(c, 0, sizeof *c);
    c->type = type; c->x = x; c->y = y; c->w = w; c->h = h; c->text = text; c->id = id;
    return c;
}
static int ox(int k) { return ad[k].x + 8; }
static int oy(int k) { return ad[k].y + DIALOG_TITLE_H + 6; }
static int applet_here(void)
{
    int k;
    for (k = 0; k < P_COUNT; k++) if (ad[k].open && ad[k].win && HOST.window == ad[k].win) return k;
    return -1;
}
static void show(int k, const char *title, int w, int h)
{
    dialog_show(&ad[k], title, ac[k], an[k], w, h, 1, 2);
    ad[k].modeless = 1;
    dialog_sync(&ad[k]);
    app_log("[CONTROL] applet", applets[k].label);
}

/* A slider: value min..max on a track of w pixels at x, y (client). */
struct slider { int x, y, w, min, max, value; const char *left, *right; };
static void slider_draw(int k, struct slider *s)
{
    int x = ox(k) + s->x, y = oy(k) + s->y, t;
    ui_inset(x, y + 8, s->w, 5);
    t = x + (int)((long)(s->w - 11) * (s->value - s->min) / (s->max - s->min));
    ui_bevel(t, y, 11, 21, C_FACE);
    ui_rect(t + 5, y + 4, 1, 13, C_SHADOW);
    if (s->left) ui_text(x, y + 22, s->left, C_INK);
    if (s->right) ui_text(x + s->w - ui_measure(s->right), y + 22, s->right, C_INK);
}
static int slider_mouse(int k, struct slider *s, int kind, int sx, int sy)
{
    int x = ox(k) + s->x, y = oy(k) + s->y, v;
    if (kind != MOUSE_DOWN && kind != MOUSE_MOVE) return 0;
    if (kind == MOUSE_DOWN && (sx < x - 6 || sx >= x + s->w + 6 || sy < y - 2 || sy >= y + 24)) return 0;
    if (kind == MOUSE_MOVE && !(HOST.buttons & 1)) return 0;
    if (kind == MOUSE_MOVE && (sy < y - 30 || sy >= y + 50)) return 0;
    v = s->min + (int)((long)(sx - x - 5) * (s->max - s->min) / (s->w - 11) + (sx > x ? 0 : 0));
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    if (v == s->value && kind == MOUSE_MOVE) return 1;
    s->value = v;
    return 2;
}
/* A list: rows of 18 pixels at x, y (client), w wide. */
struct list { int x, y, w, rows, top, sel, count; };
static void list_frame(int k, struct list *l)
{
    int x = ox(k) + l->x, y = oy(k) + l->y;
    ui_inset(x, y, l->w, l->rows * 18 + 4);
    ui_rect(x + 2, y + 2, l->w - 4, l->rows * 18, C_PAPER);
    if (l->count > l->rows) draw_scroll(x + l->w - 18, y + 2, l->rows * 18, l->top, l->count, l->rows);
}
static int list_row(int k, struct list *l, int i, int *ry)
{
    if (i < l->top || i >= l->top + l->rows) return 0;
    *ry = oy(k) + l->y + 2 + (i - l->top) * 18;
    return 1;
}
static int list_mouse(int k, struct list *l, int kind, int sx, int sy)
{
    int x = ox(k) + l->x, y = oy(k) + l->y, i;
    if (kind != MOUSE_DOWN || sx < x || sx >= x + l->w || sy < y || sy >= y + l->rows * 18 + 4) return 0;
    if (l->count > l->rows && sx >= x + l->w - 18) {
        long p = scroll_hit(x + l->w - 18, y + 2, l->rows * 18, sy, l->count, l->rows);
        if (p == -1 && l->top > 0) l->top--;
        else if (p == -2 && l->top + l->rows < l->count) l->top++;
        else if (p >= 16) l->top = (int)(p - 16);
        return 1;
    }
    i = l->top + (sy - y - 2) / 18;
    if (i < 0 || i >= l->count) return 1;
    l->sel = i;
    return 2;
}
static void list_key(struct list *l, int key)
{
    int s = KEY_SCAN(key);
    if (KEY_CHAR(key) && KEY_CHAR(key) != 0xE0) return;
    if (s == K_UP && l->sel > 0) l->sel--;
    if (s == K_DOWN && l->sel + 1 < l->count) l->sel++;
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + l->rows) l->top = l->sel - l->rows + 1;
}

/* A modal message over the applets. */
static struct dialog dlg;
static void message(const char *title, const char *text)
{
    msgbox(&dlg, title, text, "OK");
    app_log("[CONTROL] message", text);
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
#define MAX_FONTS 32
struct fontinfo { char file[13]; char name[32]; u8 size; };
static struct fontinfo fonts[MAX_FONTS];
static int nfonts, preview_loaded = -1;
static struct list fn_list;
static char install_path[SYS_PATH], install_field_buf[SYS_PATH];
static struct field install_field;
static int fonts_install_at;
static const char fonts_dir[] = "C:\\SYSTEM\\FONTS\\";
static int font_header(const char *path, char *name, u8 *size)
{
    u8 h[64];
    int f = dos_open(path, 0), n;
    if (f < 0) return f;
    n = dos_read(f, h, 64);
    dos_close(f);
    if (n != 64 || str_nicmp((char *)h, "CIUKFNT1", 8) || h[46] != (6270 & 0xFF) || h[47] != (6270 >> 8)) return -13;
    str_ncopy(name, (char *)h + 8, 32);
    *size = h[42];
    return 0;
}
static void fonts_scan(void)
{
    struct dos_find f;
    char p[SYS_PATH];
    int r;
    nfonts = 0;
    dos_set_dta(&f);
    str_copy(p, fonts_dir); str_cat(p, "*.CFN");
    for (r = dos_find_first(p, A_ARCH | A_RDONLY); r >= 0 && nfonts < MAX_FONTS; r = dos_find_next()) {
        str_ncopy(fonts[nfonts].file, f.name, 13);
        nfonts++;
    }
    for (r = 0; r < nfonts; r++) {
        str_copy(p, fonts_dir); str_cat(p, fonts[r].file);
        if (font_header(p, fonts[r].name, &fonts[r].size) < 0) str_copy(fonts[r].name, fonts[r].file);
    }
    for (r = 1; r < nfonts; r++) {                    /* by name */
        int j;
        struct fontinfo t;
        mem_copy(&t, &fonts[r], sizeof t);
        for (j = r; j > 0 && str_icmp(t.name, fonts[j - 1].name) < 0; j--) mem_copy(&fonts[j], &fonts[j - 1], sizeof t);
        mem_copy(&fonts[j], &t, sizeof t);
    }
    fn_list.count = nfonts;
    preview_loaded = -1;
}
static int is_system_font(int i)
{
    if (!cfg.font[0]) return !str_icmp(fonts[i].file, "CIUKIOS.CFN");
    return !str_icmp(fonts[i].file, cfg.font);
}
static void fonts_buttons(void)
{
    int k = P_FONTS, sel = fn_list.sel;
    struct dctl *c = ac[k];
    int i;
    for (i = 0; i < an[k]; i++) {
        if (c[i].id == 10) c[i].disabled = sel < 0 || is_system_font(sel);
        if (c[i].id == 12) c[i].disabled = sel < 0 || is_system_font(sel) || !str_icmp(fonts[sel].file, "CIUKIOS.CFN");
    }
}
static void fonts_open(const char *install)
{
    int k = P_FONTS, i;
    cfg_load(&cfg);
    fonts_scan();
    an[k] = 0;
    fn_list.x = 0; fn_list.y = 22; fn_list.w = 230; fn_list.rows = 11; fn_list.top = 0; fn_list.sel = -1;
    for (i = 0; i < nfonts; i++) if (is_system_font(i)) fn_list.sel = i;
    add(k, DC_LABEL, 0, 0, 0, 16, "&Installed fonts:", 0);
    add(k, DC_LABEL, 246, 0, 0, 16, "Preview:", 0);
    add(k, DC_BUTTON, 246, 156, 190, 26, "&Use as System Font", 10);
    add(k, DC_BUTTON, 246, 186, 190, 26, "Restore &Default Font", 11);
    add(k, DC_BUTTON, 246, 216, 92, 26, "&Delete", 12);
    add(k, DC_GROUP, 0, 238, 440, 64, "Install New Font", 0);
    add(k, DC_LABEL, 10, 262, 0, 16, "&File:", 0);
    fonts_install_at = an[k];
    field_set(&install_field, install_field_buf, SYS_PATH, install ? install : "A:\\");
    add(k, DC_FIELD, 50, 260, 280, 22, 0, 0)->field = &install_field;
    add(k, DC_BUTTON, 340, 258, 90, 26, "&Install", 13);
    add(k, DC_BUTTON, 352, 314, 84, 26, "Close", 2);
    fonts_buttons();
    show(k, "Fonts", 460, 346);
    if (install) { ad[k].focus = fonts_install_at + 1; }
}
static void fonts_paint(void)
{
    int k = P_FONTS, i, ry, x, y;
    char t[48];
    list_frame(k, &fn_list);
    for (i = 0; i < nfonts; i++) {
        if (!list_row(k, &fn_list, i, &ry)) continue;
        if (i == fn_list.sel) ui_rect(ox(k) + 2, ry, fn_list.w - (nfonts > fn_list.rows ? 22 : 4), 18, C_TITLE);
        ui_rect(ox(k) + 6, ry + 3, 10, 12, i == fn_list.sel ? C_PAPER : C_BLUE);
        ui_text(ox(k) + 8, ry + 1, "A", i == fn_list.sel ? C_TITLE : C_PAPER);
        str_ncopy(t, fonts[i].name, 32);
        if (is_system_font(i)) str_cat(t, " *");
        draw_frame_text(ox(k) + 22, ry + 1, fn_list.w - 44, t, i == fn_list.sel ? C_PAPER : C_INK);
    }
    x = ox(k) + 246; y = oy(k) + 22;
    ui_inset(x, y, 190, 126);
    ui_rect(x + 2, y + 2, 186, 122, C_PAPER);
    if (fn_list.sel >= 0) {
        char p[SYS_PATH];
        str_copy(p, fonts_dir); str_cat(p, fonts[fn_list.sel].file);
        app_font(p, 1);                           /* the slot is shared: the shell caches it */
        draw_frame_text(x + 8, y + 6, 176, fonts[fn_list.sel].name, C_TITLE | ALTFONT | BOLD);
        ui_text(x + 8, y + 30, "AaBbCcDdEeFfGg", C_INK | ALTFONT);
        ui_text(x + 8, y + 50, "The quick brown fox", C_INK | ALTFONT);
        ui_text(x + 8, y + 68, "jumps over the lazy dog.", C_INK | ALTFONT);
        ui_text(x + 8, y + 90, "0123456789 !?&", C_INK | ALTFONT | BOLD);
        str_copy(t, fonts[fn_list.sel].file);
        str_cat(t, "  ");
        fmt_u32(t + str_len(t), fonts[fn_list.sel].size);
        str_cat(t, " px");
        ui_text(x + 8, y + 106, t, C_SHADOW);
    }
    ui_text(ox(k), oy(k) + 222, "* the system font", C_SHADOW);
}
static const char *base_of(const char *p)
{
    const char *b = p;
    while (*p) { if (*p == '\\' || *p == '/' || *p == ':') b = p + 1; p++; }
    return b;
}
static void font_install(void)
{
    char name[32], dst[SYS_PATH];
    u8 size;
    int r, i;
    str_ncopy(install_path, install_field.text, SYS_PATH);
    for (i = 0; install_path[i]; i++) install_path[i] = to_upper(install_path[i]);
    r = font_header(install_path, name, &size);
    if (r < 0) {
        message("Install New Font", r == -13 ? "This file is not a CiukiOS font (.CFN).\nConvert TrueType and OpenType fonts with\nscripts/build_fonts.py --convert." : dos_error_text(r));
        return;
    }
    str_copy(dst, fonts_dir); str_cat(dst, base_of(install_path));
    if (str_icmp(dst, install_path)) {
        r = tree_copy(install_path, dst, msg, sizeof msg);
        if (r < 0) { message("Install New Font", dos_error_text(r)); return; }
    }
    app_log("[CONTROL] font installed", base_of(install_path));
    fonts_scan();
    for (i = 0; i < nfonts; i++) if (!str_icmp(fonts[i].file, base_of(install_path))) fn_list.sel = i;
    str_copy(msg, "The font '"); str_cat(msg, name); str_cat(msg, "' was installed.");
    message("Install New Font", msg);
}
static void font_use(int i)
{
    if (i < 0) return;
    cfg_load(&cfg);
    if (!str_icmp(fonts[i].file, "CIUKIOS.CFN")) cfg.font[0] = 0;
    else str_ncopy(cfg.font, fonts[i].file, 13);
    if (!cfg.font[0]) app_font("C:\\SYSTEM\\FONTS\\CIUKIOS.CFN", 0);
    cfg_save(&cfg);
    app_log("[CONTROL] system font", fonts[i].file);
}
static int fonts_mouse(int kind, int sx, int sy)
{
    static unsigned last;
    static int last_i = -1;
    int r = list_mouse(P_FONTS, &fn_list, kind, sx, sy);
    if (r == 2) {
        if (fn_list.sel == last_i && (unsigned)(HOST.ticks - last) < (unsigned)HOST.dblclick) font_use(fn_list.sel);
        last_i = fn_list.sel; last = HOST.ticks;
        fonts_buttons();
    }
    return r != 0;
}
static void fonts_result(int r)
{
    int i = fn_list.sel;
    char p[SYS_PATH];
    if (r == 10) font_use(i);
    if (r == 11) {
        for (i = 0; i < nfonts; i++) if (!str_icmp(fonts[i].file, "CIUKIOS.CFN")) break;
        if (i < nfonts) { font_use(i); fn_list.sel = i; }
    }
    if (r == 12 && i >= 0 && !is_system_font(i)) {
        str_copy(p, fonts_dir); str_cat(p, fonts[i].file);
        if (dos_delete(p) >= 0) app_log("[CONTROL] font deleted", fonts[i].file);
        fonts_scan();
        fn_list.sel = -1;
    }
    if (r == 13) font_install();
    fonts_buttons();
}

/* ------------------------------------------------------------------ */
/* Mouse, Keyboard                                                     */
static struct slider ms_speed, ms_dbl, kb_delay, kb_rate;
static u8 kb_saved_delay, kb_saved_rate;
static int ms_swap_at, ms_test_open;
static unsigned ms_test_tick;
static int ms_scheme_at[3];
static struct field kb_test;
static char kb_test_buf[40];
static void mouse_action(int id)
{
    int k = P_MOUSE, i;
    if (id >= 10 && id <= 12) {
        for (i = 0; i < 3; i++) ac[k][ms_scheme_at[i]].value = (id - 10 == i);
        cfg.cursor_scheme = (u8)(id - 10);
        ui_dirty = 1;
    }
}
static void mouse_open(void)
{
    int k = P_MOUSE;
    cfg_load(&cfg);
    mem_copy(&saved_cfg, &cfg, sizeof cfg);
    an[k] = 0;
    add(k, DC_GROUP, 0, 0, 380, 68, "Pointer speed", 0);
    ms_speed.x = 20; ms_speed.y = 22; ms_speed.w = 250; ms_speed.min = 1; ms_speed.max = 4;
    ms_speed.value = cfg.mouse_speed ? cfg.mouse_speed : 2; ms_speed.left = "Slow"; ms_speed.right = "Fast";
    add(k, DC_GROUP, 0, 74, 380, 78, "Double-click speed", 0);
    ms_dbl.x = 20; ms_dbl.y = 96; ms_dbl.w = 250; ms_dbl.min = 0; ms_dbl.max = 14;
    ms_dbl.value = 18 - (cfg.dblclick ? cfg.dblclick : 9); ms_dbl.left = "Slow"; ms_dbl.right = "Fast";
    add(k, DC_LABEL, 20, 132, 0, 16, "Double-click the folder to test.", 0);
    ms_swap_at = an[k];
    add(k, DC_CHECK, 0, 158, 0, 17, "&Switch primary and secondary buttons", 0)->value = cfg.mouse_swap;
    add(k, DC_GROUP, 0, 180, 380, 84, "Pointer scheme", 0);
    ms_scheme_at[0] = an[k];
    add(k, DC_RADIO, 16, 198, 0, 17, "&Tango (Default)", 10)->value = (cfg.cursor_scheme == 0);
    ms_scheme_at[1] = an[k];
    add(k, DC_RADIO, 16, 218, 0, 17, "&Classic 95", 11)->value = (cfg.cursor_scheme == 1);
    ms_scheme_at[2] = an[k];
    add(k, DC_RADIO, 16, 238, 0, 17, "&3D Contrast", 12)->value = (cfg.cursor_scheme == 2);
    add(k, DC_BUTTON, 116, 274, 84, 26, "OK", 1);
    add(k, DC_BUTTON, 204, 274, 84, 26, "Cancel", 2);
    add(k, DC_BUTTON, 292, 274, 84, 26, "&Apply", 3);
    show(k, "Mouse", 400, 308);
}
static void mouse_paint(void)
{
    int k = P_MOUSE, px, py;
    slider_draw(k, &ms_speed);
    slider_draw(k, &ms_dbl);
    ui_icon(ox(k) + 300, oy(k) + 90, ms_test_open ? ICON_DESKTOP : ICON_FOLDER);
    px = ox(k) + 180; py = oy(k) + 196;
    ui_inset(px, py, 184, 60);
    ui_rect(px + 1, py + 1, 182, 58, C_PAPER);
    ui_text(px + 6, py + 4, "Preview:", C_SHADOW);
    ui_text(px + 10, py + 24, "Arrow", C_INK);
    ui_text(px + 68, py + 24, "Text", C_INK);
    ui_text(px + 118, py + 24, "Blocked", C_INK);
}
static int mouse_mouse(int kind, int sx, int sy)
{
    int k = P_MOUSE, r = slider_mouse(k, &ms_speed, kind, sx, sy);
    int px = ox(k) + 180, py = oy(k) + 196;
    if (!r) r = slider_mouse(k, &ms_dbl, kind, sx, sy);
    if (r) return 1;
    if (sx >= px && sx < px + 184 && sy >= py && sy < py + 60) {
        if (sx < px + 55) ui_cursor(CURSOR_ARROW);
        else if (sx < px + 105) ui_cursor(CURSOR_IBEAM);
        else ui_cursor(CURSOR_FORBIDDEN);
    }
    if (kind == MOUSE_DOWN && sx >= ox(k) + 300 && sx < ox(k) + 344 && sy >= oy(k) + 90 && sy < oy(k) + 132) {
        if ((unsigned)(HOST.ticks - ms_test_tick) < (unsigned)(18 - ms_dbl.value)) { ms_test_open = !ms_test_open; ms_test_tick = 0; }
        else ms_test_tick = HOST.ticks;
        return 1;
    }
    return 0;
}
static void mouse_save(void)
{
    char t[8]; int i;
    cfg_load(&cfg);
    cfg.mouse_speed = (u8)ms_speed.value;
    cfg.dblclick = (u8)(18 - ms_dbl.value);
    cfg.mouse_swap = (u8)ac[P_MOUSE][ms_swap_at].value;
    for (i = 0; i < 3; i++)
        if (ac[P_MOUSE][ms_scheme_at[i]].value) cfg.cursor_scheme = (u8)i;
    cfg_save(&cfg);
    fmt_u32(t, ms_speed.value);
    app_log("[CONTROL] mouse speed", t);
}
static void keyboard_open(void)
{
    int k = P_KEYBOARD;
    cfg_load(&cfg);
    kb_saved_delay = cfg.kbd_delay;
    kb_saved_rate = cfg.kbd_rate;
    an[k] = 0;
    add(k, DC_GROUP, 0, 0, 380, 70, "Repeat delay", 0);
    kb_delay.x = 20; kb_delay.y = 24; kb_delay.w = 250; kb_delay.min = 0; kb_delay.max = 3;
    kb_delay.value = 3 - (cfg.kbd_delay & 3); kb_delay.left = "Long"; kb_delay.right = "Short";
    add(k, DC_GROUP, 0, 80, 380, 70, "Repeat rate", 0);
    kb_rate.x = 20; kb_rate.y = 104; kb_rate.w = 250; kb_rate.min = 0; kb_rate.max = 31;
    kb_rate.value = 31 - (cfg.kbd_rate & 31); kb_rate.left = "Slow"; kb_rate.right = "Fast";
    add(k, DC_LABEL, 0, 164, 0, 16, "&Click here and hold down a key to test:", 0);
    field_set(&kb_test, kb_test_buf, sizeof kb_test_buf, "");
    add(k, DC_FIELD, 0, 184, 380, 22, 0, 0)->field = &kb_test;
    add(k, DC_BUTTON, 116, 220, 84, 26, "OK", 1);
    add(k, DC_BUTTON, 204, 220, 84, 26, "Cancel", 2);
    add(k, DC_BUTTON, 292, 220, 84, 26, "&Apply", 3);
    show(k, "Keyboard", 400, 252);
}
static void keyboard_apply_now(void)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0305;
    r.bx = (u16)(((3 - kb_delay.value) << 8) | (31 - kb_rate.value));
    r.ds = r.es = app_seg();
    intr(0x16, &r);
}
static void keyboard_apply_now(void);
static void keyboard_save(void)
{
    cfg_load(&cfg);
    cfg.kbd_delay = (u8)(3 - kb_delay.value);
    cfg.kbd_rate = (u8)(31 - kb_rate.value);
    cfg_save(&cfg);
    app_log("[CONTROL] saved", "keyboard");
}

/* ------------------------------------------------------------------ */
/* Sound                                                               */
static u16 nam;                                   /* AC'97 mixer I/O base */
static struct slider snd_master, snd_pcm;
static int snd_startup_at, snd_mute_at;
static const u16 ac97_ids[] = { 0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x266E, 0x27DE, 0x7195, 0 };
static void ac97_find(void)
{
    struct regs r;
    int i;
    nam = 0;
    for (i = 0; ac97_ids[i] && !nam; i++) {
        mem_set(&r, 0, sizeof r);
        r.ax = 0xB102; r.cx = ac97_ids[i]; r.dx = 0x8086; r.si = 0;
        r.ds = r.es = app_seg();
        if (intr(0x1A, &r) || (r.ax & 0xFF00)) continue;
        {
            u16 bus = r.bx;
            mem_set(&r, 0, sizeof r);
            r.ax = 0xB109; r.bx = bus; r.di = 0x10;         /* BAR0: mixer (NAM) */
            r.ds = r.es = app_seg();
            if (!intr(0x1A, &r) && !(r.ax & 0xFF00) && (r.cx & 1)) nam = r.cx & 0xFFFC;
        }
    }
}
static int boot_sound(void)
{
    char c = '1';
    int f = dos_open("\\SYSTEM\\BOOT.SND", 0);
    if (f >= 0) { dos_read(f, &c, 1); dos_close(f); }
    return c != '0';
}
static void sound_open(void)
{
    int k = P_SOUND;
    int saved_level, saved_mute;
    u16 v;
    ac97_find();
    an[k] = 0;
    add(k, DC_GROUP, 0, 0, 400, 78, "Sounds", 0);
    snd_startup_at = an[k];
    add(k, DC_CHECK, 12, 22, 0, 17, "Play the &startup sound", 0)->value = boot_sound();
    add(k, DC_LABEL, 12, 48, 0, 16, "Test:", 0);
    add(k, DC_BUTTON, 60, 44, 90, 24, "&Notice", 10);
    add(k, DC_BUTTON, 156, 44, 90, 24, "&Error", 11);
    add(k, DC_GROUP, 0, 88, 400, 142, "Volume (AC'97 mixer)", 0);
    snd_master.x = 110; snd_master.y = 112; snd_master.w = 270; snd_master.min = 0; snd_master.max = 31;
    snd_master.left = "Low"; snd_master.right = "High";
    snd_pcm = snd_master; snd_pcm.y = 164;
    add(k, DC_LABEL, 12, 114, 0, 16, "&Master:", 0);
    add(k, DC_LABEL, 12, 166, 0, 16, "&Wave:", 0);
    snd_mute_at = an[k];
    add(k, DC_CHECK, 12, 206, 0, 17, "M&ute", 0);
    if (nam) {
        int at;
        v = inw(nam + 0x02);
        at = (v >> 8) & 63;                       /* attenuation, 1.5 dB steps */
        snd_master.value = 31 - (at > 31 ? 31 : at);
        ac[k][snd_mute_at].value = (v & 0x8000) != 0;
        v = inw(nam + 0x18);
        snd_pcm.value = 31 - ((v >> 8) & 31);
    } else {
        snd_master.value = snd_pcm.value = 0;
        ac[k][snd_mute_at].disabled = 1;
    }
    if (audio_settings_load(&saved_level, &saved_mute)) {
        snd_master.value = saved_level;
        ac[k][snd_mute_at].value = saved_mute;
    }
    add(k, DC_BUTTON, 0, 242, 130, 26, "&Advanced...", 12);
    add(k, DC_BUTTON, 228, 242, 84, 26, "OK", 1);
    add(k, DC_BUTTON, 316, 242, 84, 26, "Cancel", 2);
    show(k, "Sound", 420, 274);
}
static void sound_mixer(void)
{
    u16 m, p;
    if (!nam) return;
    m = (u16)(((31 - snd_master.value) << 8) | (31 - snd_master.value));
    p = (u16)(((31 - snd_pcm.value) << 8) | (31 - snd_pcm.value));
    if (ac[P_SOUND][snd_mute_at].value) m |= 0x8000;
    outw(nam + 0x02, m);
    outw(nam + 0x18, p);
}
static void sound_paint(void)
{
    int k = P_SOUND;
    if (!nam) {
        ui_text(ox(k) + 110, oy(k) + 130, "No AC'97 mixer was found.", C_SHADOW);
        return;
    }
    slider_draw(k, &snd_master);
    slider_draw(k, &snd_pcm);
}
static int sound_mouse(int kind, int sx, int sy)
{
    int r;
    if (!nam) return 0;
    r = slider_mouse(P_SOUND, &snd_master, kind, sx, sy);
    if (!r) r = slider_mouse(P_SOUND, &snd_pcm, kind, sx, sy);
    if (r == 2) sound_mixer();
    return r != 0;
}
static void sound_save(void)
{
    char c = ac[P_SOUND][snd_startup_at].value ? '1' : '0';
    int f = dos_create("\\SYSTEM\\BOOT.SND");
    if (f >= 0) { dos_write(f, &c, 1); dos_close(f); }
    sound_mixer();
    audio_settings_save(snd_master.value, ac[P_SOUND][snd_mute_at].value);
    app_log("[CONTROL] saved", c == '1' ? "sound startup on" : "sound startup off");
}

/* ------------------------------------------------------------------ */
/* Date and Time                                                       */
static int dt_y, dt_m, dt_d, dt_h, dt_mi, dt_s, dt_fields, dt_auto_at, dt_sync_pending;
static struct field dt_field[6];
static char dt_buf[6][6];
static char dt_zone[48], dt_reply[768];
#define DT_CFG "C:\\SYSTEM\\UI\\TIMEZONE.CFG"
#define DT_REPLY "C:\\NET\\TIME.TXT"
static int datetime_auto_enabled(void)
{
    int h, n;
    char c = 0;
    h = dos_open(DT_CFG, 0);
    if (h < 0) return 0;
    n = dos_read(h, &c, 1);
    dos_close(h);
    return n == 1 && c == '1';
}
static int datetime_auto_save(int enabled)
{
    int h = dos_create(DT_CFG);
    char c = enabled ? '1' : '0';
    int n;
    if (h < 0) return 0;
    n = dos_write(h, &c, 1);
    dos_close(h);
    return n == 1;
}
static int dt_digits(const char *p, int n)
{
    int v = 0;
    while (n--) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + *p++ - '0';
    }
    return v;
}
static int datetime_network_read(void)
{
    int h, n, i = 0;
    char *p, *q;
    h = dos_open(DT_REPLY, 0);
    if (h < 0) return 0;
    n = dos_read(h, dt_reply, sizeof dt_reply - 1);
    dos_close(h);
    if (n < 0 || n >= sizeof dt_reply - 1) return 0;
    dt_reply[n] = 0;
    p = dt_reply;
    while (*p) {
        if (!str_nicmp(p, "datetime: ", 10)) {
            q = p + 10;
            if (q[4] != '-' || q[7] != '-' || q[10] != 'T' || q[13] != ':' || q[16] != ':') return 0;
            dt_y = dt_digits(q, 4); dt_m = dt_digits(q + 5, 2); dt_d = dt_digits(q + 8, 2);
            dt_h = dt_digits(q + 11, 2); dt_mi = dt_digits(q + 14, 2); dt_s = dt_digits(q + 17, 2);
            i = 1;
        }
        if (!str_nicmp(p, "timezone: ", 10)) {
            q = p + 10;
            str_ncopy(dt_zone, q, sizeof dt_zone);
            for (q = dt_zone; *q; q++) if (*q == '\r' || *q == '\n') { *q = 0; break; }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return i && dt_zone[0] && dt_y >= 1980 && dt_y <= 2099 &&
           dt_m >= 1 && dt_m <= 12 && dt_d >= 1 && dt_d <= 31 &&
           dt_h >= 0 && dt_h <= 23 && dt_mi >= 0 && dt_mi <= 59 && dt_s >= 0 && dt_s <= 59;
}
static void datetime_network_start(void)
{
    dos_delete(DT_REPLY);
    dt_sync_pending = 1;
    app_log("[CONTROL] time zone lookup", "worldtime.timezone.io");
    app_helper("C:\\NET\\HTGET.EXE -o C:\\NET\\TIME.TXT http://worldtime.timezone.io/api/ip.txt");
}
static const char *month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December" };
static int days_in(int y, int m)
{
    static const u8 d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1];
}
static int weekday(int y, int m, int d)         /* 0 Sunday */
{
    static const u8 t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static void dt_to_fields(void)
{
    int v[6], i;
    v[0] = dt_d; v[1] = dt_m; v[2] = dt_y; v[3] = dt_h; v[4] = dt_mi; v[5] = dt_s;
    for (i = 0; i < 6; i++) {
        char t[8];
        if (i == 2) fmt_u32(t, (u32)v[i]); else fmt_2(t, v[i]);
        field_set(&dt_field[i], dt_buf[i], i == 2 ? 5 : 3, t);
    }
}
static void datetime_open(void)
{
    int k = P_DATETIME, wd, i;
    static const char *labels[6] = { "&Day", "&Month", "&Year", "&Hours", "M&inutes", "&Seconds" };
    dos_get_date(&dt_y, &dt_m, &dt_d, &wd);
    dos_get_time(&dt_h, &dt_mi, &dt_s);
    dt_zone[0] = 0;
    dt_to_fields();
    an[k] = 0;
    add(k, DC_GROUP, 0, 0, 250, 232, "Date", 0);
    add(k, DC_GROUP, 262, 0, 168, 232, "Time", 0);
    dt_fields = an[k];
    for (i = 0; i < 6; i++) {
        int x = i < 3 ? 12 + i * 78 : 274, y = i < 3 ? 40 : 40 + (i - 3) * 52;
        add(k, DC_LABEL, x, y - 18, 0, 16, labels[i], 0);
        add(k, DC_FIELD, x, y, i == 2 ? 70 : 54, 22, 0, 0)->field = &dt_field[i];
    }
    dt_auto_at = an[k];
    add(k, DC_CHECK, 12, 239, 0, 17, "Detect automatically when opened", 0)->value = datetime_auto_enabled();
    add(k, DC_BUTTON, 12, 292, 142, 26, "Detect now", 10);
    add(k, DC_BUTTON, 170, 292, 84, 26, "OK", 1);
    add(k, DC_BUTTON, 258, 292, 84, 26, "Cancel", 2);
    add(k, DC_BUTTON, 346, 292, 84, 26, "&Apply", 3);
    show(k, "Date and Time", 450, 324);
    if (ad[k].c[dt_auto_at].value) datetime_network_start();
}
static void dt_from_fields(void)
{
    dt_d = parse_u16(dt_field[0].text); dt_m = parse_u16(dt_field[1].text); dt_y = parse_u16(dt_field[2].text);
    dt_h = parse_u16(dt_field[3].text); dt_mi = parse_u16(dt_field[4].text); dt_s = parse_u16(dt_field[5].text);
}
static void datetime_paint(void)
{
    int k = P_DATETIME, x = ox(k) + 12, y = oy(k) + 70, i, d, w, first, n;
    char t[24];
    static const char *wd = "SMTWTFS";
    dt_from_fields();
    if (dt_m < 1 || dt_m > 12 || dt_y < 1980 || dt_y > 2099) return;
    str_copy(t, month_names[dt_m - 1]); str_cat(t, " "); fmt_u32(t + str_len(t), (u32)dt_y);
    ui_text(x, y, t, C_TITLE | BOLD);
    y += 20;
    for (i = 0; i < 7; i++) { char c[2]; c[0] = wd[i]; c[1] = 0; ui_text(x + i * 32 + 10, y, c, C_SHADOW); }
    y += 18;
    first = weekday(dt_y, dt_m, 1);
    n = days_in(dt_y, dt_m);
    for (d = 1; d <= n; d++) {
        int cell = first + d - 1, cx = x + (cell % 7) * 32, cy = y + (cell / 7) * 18;
        fmt_u32(t, (u32)d);
        w = ui_measure(t);
        if (d == dt_d) { ui_rect(cx + 2, cy, 28, 17, C_TITLE); ui_text(cx + 16 - w / 2, cy, t, C_PAPER); }
        else ui_text(cx + 16 - w / 2, cy, t, C_INK);
    }
    /* A clock face: the time as set. */
    x = ox(k) + 380; y = oy(k) + 40;
    fmt_2(t, dt_h); str_cat(t, ":"); fmt_2(t + str_len(t), dt_mi); str_cat(t, ":"); fmt_2(t + str_len(t), dt_s);
    ui_inset(ox(k) + 274, oy(k) + 196, 144, 26);
    ui_text(ox(k) + 346 - ui_measure(t) / 2, oy(k) + 200, t, C_INK | BOLD);
    if (dt_zone[0]) {
        str_copy(t, "Time zone: ");
        ui_text(ox(k) + 12, oy(k) + 264, t, C_SHADOW);
        ui_text(ox(k) + 94, oy(k) + 264, dt_zone, C_INK);
    } else ui_text(ox(k) + 12, oy(k) + 264, "Time zone: local clock (network not detected)", C_SHADOW);
    (void)x; (void)y;
}
static int datetime_mouse(int kind, int sx, int sy)
{
    int k = P_DATETIME, x = ox(k) + 12, y = oy(k) + 108, first, cell, d;
    if (kind != MOUSE_DOWN || sx < x || sx >= x + 7 * 32 || sy < y || sy >= y + 6 * 18) return 0;
    dt_from_fields();
    if (dt_m < 1 || dt_m > 12) return 0;
    first = weekday(dt_y, dt_m, 1);
    cell = (sy - y) / 18 * 7 + (sx - x) / 32;
    d = cell - first + 1;
    if (d < 1 || d > days_in(dt_y, dt_m)) return 0;
    dt_d = d;
    {
        char t[4];
        fmt_2(t, d);
        field_set(&dt_field[0], dt_buf[0], 3, t);
    }
    return 1;
}
static u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static int datetime_save(void)
{
    struct regs r;
    char t[32];
    dt_from_fields();
    if (dt_y < 1980 || dt_y > 2099 || dt_m < 1 || dt_m > 12 || dt_d < 1 || dt_d > days_in(dt_y, dt_m) ||
        dt_h > 23 || dt_mi > 59 || dt_s > 59) {
        message("Date and Time", "The date or the time is not valid.");
        return 0;
    }
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0500; r.cx = (u16)((bcd(dt_y / 100) << 8) | bcd(dt_y % 100)); r.dx = (u16)((bcd(dt_m) << 8) | bcd(dt_d));
    r.ds = r.es = app_seg();
    intr(0x1A, &r);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0300; r.cx = (u16)((bcd(dt_h) << 8) | bcd(dt_mi)); r.dx = (u16)(bcd(dt_s) << 8);
    r.ds = r.es = app_seg();
    intr(0x1A, &r);
    /* The BIOS tick count follows the new time. */
    {
        u32 ticks = ((u32)dt_h * 3600 + (u32)dt_mi * 60 + dt_s) * 182 / 10;
        mem_set(&r, 0, sizeof r);
        r.ax = 0x0100; r.cx = (u16)(ticks >> 16); r.dx = (u16)ticks;
        r.ds = r.es = app_seg();
        intr(0x1A, &r);
    }
    fmt_u32(t, (u32)dt_y); str_cat(t, "-"); fmt_2(t + str_len(t), dt_m); str_cat(t, "-"); fmt_2(t + str_len(t), dt_d);
    str_cat(t, " "); fmt_2(t + str_len(t), dt_h); str_cat(t, ":"); fmt_2(t + str_len(t), dt_mi);
    app_log("[CONTROL] date set", t);
    if (!datetime_auto_save(ad[P_DATETIME].c[dt_auto_at].value)) {
        message("Date and Time", "The automatic setting could not be saved.");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* System                                                              */
static char sy_lines[8][64];
static void system_open(void)
{
    int k = P_SYSTEM, family, i;
    struct regs r;
    char t[24], vendor[13];
    long conv, xms = -1, ems = -1;
    u16 xs = 0, xo = 0, seg;
    mem_set(&r, 0, sizeof r); r.ds = r.es = app_seg();
    intr(0x12, &r); conv = r.ax;
    mem_set(&r, 0, sizeof r); r.ax = 0x4300; r.ds = r.es = app_seg();
    intr(0x2F, &r);
    if ((r.ax & 0xFF) == 0x80) {
        mem_set(&r, 0, sizeof r); r.ax = 0x4310; r.ds = r.es = app_seg();
        intr(0x2F, &r); xs = r.es; xo = r.bx;
        mem_set(&r, 0, sizeof r); r.ax = 0x0800; r.ds = r.es = app_seg();
        far_regs(xs, xo, &r); xms = r.dx;
    }
    seg = peek16(0, 0x67 * 4 + 2);
    if (seg && peek8(seg, 10) == 'E' && peek8(seg, 11) == 'M' && peek8(seg, 12) == 'M') {
        mem_set(&r, 0, sizeof r); r.ax = 0x4200; r.ds = r.es = app_seg();
        intr(0x67, &r);
        if (!(r.ax & 0xFF00)) ems = (long)r.dx * 16;
    }
    family = cpu_vendor(vendor);
    str_copy(sy_lines[0], "Processor: ");
    if (vendor[0]) { str_cat(sy_lines[0], vendor); str_cat(sy_lines[0], ", family "); fmt_u32(t, (u32)family); str_cat(sy_lines[0], t); }
    else str_cat(sy_lines[0], "80386 or compatible");
    str_copy(sy_lines[1], "Conventional memory: "); fmt_u32(t, (u32)conv); str_cat(sy_lines[1], t); str_cat(sy_lines[1], " KB");
    str_copy(sy_lines[2], "Free extended memory (XMS): ");
    if (xms >= 0) { fmt_u32_group(t, (u32)xms); str_cat(sy_lines[2], t); str_cat(sy_lines[2], " KB"); } else str_cat(sy_lines[2], "none");
    str_copy(sy_lines[3], "Expanded memory (EMS): ");
    if (ems >= 0) { fmt_u32_group(t, (u32)ems); str_cat(sy_lines[3], t); str_cat(sy_lines[3], " KB"); } else str_cat(sy_lines[3], "none");
    mem_set(&r, 0, sizeof r); r.ax = 0x1684; r.bx = 0x4349; r.ds = r.es = 0;
    intr(0x2F, &r);
    str_copy(sy_lines[4], "Virtual machines: ");
    str_cat(sy_lines[4], (r.es || r.di) ? "VM manager running (Jemm386 + CVSESSION)" : "VM manager not running");
    str_copy(sy_lines[5], "Screen: "); fmt_u32(t, (u32)HOST.screen_w); str_cat(sy_lines[5], t); str_cat(sy_lines[5], " x ");
    fmt_u32(t, (u32)HOST.screen_h); str_cat(sy_lines[5], t);
    mem_set(&r, 0, sizeof r); r.ax = 0x3000; r.ds = r.es = app_seg();
    intr(0x21, &r);
    str_copy(sy_lines[6], "DOS interface: version "); fmt_u32(t, (u32)(r.ax & 0xFF)); str_cat(sy_lines[6], t);
    str_cat(sy_lines[6], "."); fmt_2(t, (r.ax >> 8) & 0xFF); str_cat(sy_lines[6], t);
    an[k] = 0;
    add(k, DC_LABEL, 60, 4, 0, 16, "CiukiOS", 0);
    add(k, DC_LABEL, 60, 24, 0, 16, "A modern Retro OS, dedicated to Ciuki.", 0);
    add(k, DC_GROUP, 0, 60, 440, 176, "Computer", 0);
    for (i = 0; i < 7; i++) add(k, DC_LABEL, 12, 82 + i * 21, 0, 16, sy_lines[i], 0);
    add(k, DC_BUTTON, 0, 248, 150, 26, "&Device Manager...", 10);
    add(k, DC_BUTTON, 356, 248, 84, 26, "OK", 1);
    show(k, "System", 460, 280);
}
static void system_paint(void)
{
    ui_icon(ox(P_SYSTEM) - 4, oy(P_SYSTEM) - 2, ICON_ABOUT);
}

/* A key/value inspector with CiukiOS's own layout.  The storage contract is
 * separate from the future Win32 API bridge: an installer must call real
 * Advapi-compatible functions, not write the hive file itself. */
static struct field reg_key_field, reg_name_field, reg_type_field, reg_data_field;
static char reg_key[CREG_KEY_MAX], reg_name[CREG_NAME_MAX];
static char reg_type[12], reg_data_text[CREG_DATA_MAX];
static u8 reg_bytes[CREG_DATA_MAX];

static int reg_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void registry_open(void)
{
    int k = P_REGISTRY;
    an[k] = 0;
    field_set(&reg_key_field, reg_key, sizeof reg_key, "HKCU\\Software\\CiukiOS");
    field_set(&reg_name_field, reg_name, sizeof reg_name, "Example");
    field_set(&reg_type_field, reg_type, sizeof reg_type, "SZ");
    field_set(&reg_data_field, reg_data_text, sizeof reg_data_text, "");
    add(k, DC_LABEL, 8, 8, 0, 16, "Key (HKCU, HKLM, HKCR or HKU)", 0);
    add(k, DC_FIELD, 8, 27, 475, 23, 0, 0)->field = &reg_key_field;
    add(k, DC_LABEL, 8, 60, 0, 16, "Value name", 0);
    add(k, DC_FIELD, 8, 79, 305, 23, 0, 0)->field = &reg_name_field;
    add(k, DC_LABEL, 324, 60, 0, 16, "Type: SZ / DWORD / BINARY", 0);
    add(k, DC_FIELD, 324, 79, 159, 23, 0, 0)->field = &reg_type_field;
    add(k, DC_LABEL, 8, 112, 0, 16, "Data (decimal for DWORD, hex bytes for BINARY)", 0);
    add(k, DC_FIELD, 8, 132, 475, 23, 0, 0)->field = &reg_data_field;
    add(k, DC_LABEL, 8, 169, 0, 16, "Values are stored under C:\\SYSTEM\\CONFIG.", 0);
    add(k, DC_LABEL, 8, 188, 0, 16, "Changes persist after restart. Edit only keys you understand.", 0);
    add(k, DC_BUTTON, 8, 223, 90, 26, "&Read", 10);
    add(k, DC_BUTTON, 108, 223, 90, 26, "&Save", 11);
    add(k, DC_BUTTON, 208, 223, 90, 26, "&Delete", 12);
    add(k, DC_BUTTON, 390, 223, 93, 26, "Close", 1);
    show(k, "Ciuki Settings Registry", 503, 320);
}
static void registry_result(int r)
{
    int type, size, n, i, hi, lo;
    u32 value = 0;
    char t[40];
    if (r < 10 || r > 12) return;
    ad[P_REGISTRY].open = 1;
    if (r == 10) {
        size = sizeof reg_bytes;
        if (creg_get(reg_key, reg_name, &type, reg_bytes, &size)) {
            message("Settings Registry", "The requested value was not found."); return;
        }
        if (type == CREG_DWORD && size == 4) {
            value = (u32)reg_bytes[0] | ((u32)reg_bytes[1] << 8) |
                    ((u32)reg_bytes[2] << 16) | ((u32)reg_bytes[3] << 24);
            fmt_u32(t, value);
            field_set(&reg_type_field, reg_type, sizeof reg_type, "DWORD");
            field_set(&reg_data_field, reg_data_text, sizeof reg_data_text, t);
        } else if (type == CREG_BINARY) {
            static const char digits[] = "0123456789ABCDEF";
            n = 0;
            for (i = 0; i < size && i < 84; i++) {
                reg_data_text[n++] = digits[reg_bytes[i] >> 4];
                reg_data_text[n++] = digits[reg_bytes[i] & 15];
                if (i + 1 < size && i + 1 < 84) reg_data_text[n++] = ' ';
            }
            reg_data_text[n] = 0;
            field_set(&reg_type_field, reg_type, sizeof reg_type, "BINARY");
            field_set(&reg_data_field, reg_data_text, sizeof reg_data_text, reg_data_text);
            if (size > 84) message("Settings Registry", "Only the first 84 binary bytes are shown.");
        } else if (type == CREG_SZ && size > 0) {
            reg_bytes[sizeof reg_bytes - 1] = 0;
            field_set(&reg_type_field, reg_type, sizeof reg_type, "SZ");
            field_set(&reg_data_field, reg_data_text, sizeof reg_data_text, (char *)reg_bytes);
        } else message("Settings Registry", "This value has an unsupported type or length.");
        app_log("[REGISTRY] read", reg_key);
        return;
    }
    if (r == 12) {
        if (creg_delete(reg_key, reg_name)) message("Settings Registry", "The value could not be deleted.");
        else { app_log("[REGISTRY] delete", reg_key); message("Settings Registry", "Value deleted."); }
        return;
    }
    if (!str_icmp(reg_type, "SZ")) {
        type = CREG_SZ;
        size = str_len(reg_data_text) + 1;
        mem_copy(reg_bytes, reg_data_text, size);
    } else if (!str_icmp(reg_type, "DWORD")) {
        type = CREG_DWORD;
        if (!reg_data_text[0]) { message("Settings Registry", "Enter a decimal DWORD value."); return; }
        for (i = 0; reg_data_text[i]; i++) {
            int digit = reg_data_text[i] - '0';
            if (digit < 0 || digit > 9 || value > (0xFFFFFFFFUL - (u32)digit) / 10) {
                message("Settings Registry", "DWORD must be a decimal number from 0 to 4294967295."); return;
            }
            value = value * 10 + digit;
        }
        reg_bytes[0] = (u8)value; reg_bytes[1] = (u8)(value >> 8);
        reg_bytes[2] = (u8)(value >> 16); reg_bytes[3] = (u8)(value >> 24);
        size = 4;
    } else if (!str_icmp(reg_type, "BINARY")) {
        type = CREG_BINARY; size = 0; hi = -1;
        for (i = 0; reg_data_text[i]; i++) {
            if (reg_data_text[i] == ' ' || reg_data_text[i] == ',') continue;
            lo = reg_hex(reg_data_text[i]);
            if (lo < 0) { message("Settings Registry", "Binary data needs hexadecimal byte pairs."); return; }
            if (hi < 0) hi = lo;
            else { reg_bytes[size++] = (u8)((hi << 4) | lo); hi = -1; }
        }
        if (hi >= 0) { message("Settings Registry", "Binary data needs complete byte pairs."); return; }
    } else { message("Settings Registry", "Choose SZ, DWORD or BINARY."); return; }
    if (creg_set(reg_key, reg_name, type, reg_bytes, size))
        message("Settings Registry", "The value could not be saved.");
    else { app_log("[REGISTRY] saved", reg_key); message("Settings Registry", "Value saved."); }
}

/* ------------------------------------------------------------------ */
/* Opening applets                                                     */
static void open_applet(int k, const char *arg)
{
    if (k < 0 || k >= P_COUNT) return;
    app_log("[CONTROL] open", applets[k].label);
    switch (k) {
    case P_DISPLAY: app_open(WIN_DISPLAY, "screen"); return;
    case P_WALLPAPER: app_open(WIN_DISPLAY, "background"); return;
    case P_APPEARANCE: app_open(WIN_DISPLAY, "appearance"); return;
    case P_TASKS: shell_action(23); return;
    case P_DEVICES: app_open(WIN_DEVICES, ""); return;
    case P_DRIVERS: app_open(WIN_DEVICES, "drivers"); return;
    }
    if (ad[k].open && ad[k].win) {
        if (k == P_FONTS && arg) { field_set(&install_field, install_field_buf, SYS_PATH, arg); ad[k].focus = fonts_install_at + 1; }
        app_window_cmd(ad[k].win, 1);
        return;
    }
    switch (k) {
    case P_FONTS: fonts_open(arg); break;
    case P_SOUND: sound_open(); break;
    case P_MOUSE: mouse_open(); break;
    case P_KEYBOARD: keyboard_open(); break;
    case P_DATETIME: datetime_open(); break;
    case P_SYSTEM: system_open(); break;
    case P_REGISTRY: registry_open(); break;
    case P_NETWORK: network_open(); break;
    }
}
/* A result of an applet's buttons (1 OK, 2 Cancel or close, 3 Apply). */
static void applet_result(int k, int r)
{
    if (r < 0) return;
    switch (k) {
    case P_FONTS: fonts_result(r); if (r >= 10) ad[k].open = 1; break;
    case P_MOUSE:
        if (r >= 10 && r <= 12) { mouse_action(r); ad[k].open = 1; }
        else if (r == 1 || r == 3) mouse_save();
        break;
    case P_KEYBOARD:
        if (r == 1 || r == 3) keyboard_save();
        if (r == 2) {
            kb_delay.value = 3 - (kb_saved_delay & 3);
            kb_rate.value = 31 - (kb_saved_rate & 31);
            keyboard_apply_now();
        }
        break;
    case P_SOUND:
        if (r == 10) { app_sound(4); ad[k].open = 1; }
        if (r == 11) { app_sound(5); ad[k].open = 1; }
        if (r == 12) { shell_action(9); ad[k].open = 1; }
        if (r == 1) sound_save();
        break;
    case P_DATETIME:
        if (r == 10) { datetime_network_start(); ad[k].open = 1; }
        if ((r == 1 || r == 3) && !datetime_save()) ad[k].open = 1;
        break;
    case P_SYSTEM:
        if (r == 10) { app_open(WIN_DEVICES, ""); ad[k].open = 1; }
        break;
    case P_REGISTRY: registry_result(r); break;
    case P_NETWORK:
        if (r == 3) network_apply();
        if (r == 10) { net_scan(); str_copy(net_status, "Scan complete. Driver changes require a restart."); }
        if (r == 11) app_open(WIN_DEVICES, "network");
        if (r == 12) app_open(WIN_DEVICES, "drivers");
        if (r == 13) {
            app_helper("C:\\NET\\NETCFG.COM DHCP");
            net_scan();
            app_log("[CONTROL] network DHCP requested", 0);
        }
        if (r >= 10 && r != 13) ad[k].open = 1;
        break;
    }
    if (r == 3) ad[k].open = 1;
}
static void applet_paint(int k)
{
    dialog_draw(&ad[k]);
    switch (k) {
    case P_FONTS: fonts_paint(); break;
    case P_MOUSE: mouse_paint(); break;
    case P_SOUND: sound_paint(); break;
    case P_DATETIME: datetime_paint(); break;
    case P_SYSTEM: system_paint(); break;
    case P_KEYBOARD:
        slider_draw(k, &kb_delay);
        slider_draw(k, &kb_rate);
        break;
    }
}
static int applet_event(int k, int ev, int a, int b, int c)
{
    int r = -1, sx = HOST.x + b, sy = HOST.y + TITLE_H + c, used = 0;
    struct dialog *d = &ad[k];
    if (ev == EV_PAINT) { applet_paint(k); return 0; }
    dialog_place(d);
    if (ev == EV_CLOSE) { d->win = 0; d->open = 0; applet_result(k, 2); d->open = 0; return 0; }
    if (dlg.open) {                               /* a message first */
        if (ev == EV_MOUSE && a == MOUSE_DOWN) app_window_cmd(dlg.win, 1);
        return 0;
    }
    if (ev == EV_MOUSE) {
        if (a == MOUSE_HOVER) { ui_dirty = 0; dialog_hover(d, sx, sy); return ui_dirty; }
        switch (k) {
        case P_FONTS: used = fonts_mouse(a, sx, sy); break;
        case P_MOUSE: used = mouse_mouse(a, sx, sy); break;
        case P_SOUND: used = sound_mouse(a, sx, sy); break;
        case P_DATETIME: used = datetime_mouse(a, sx, sy); break;
        case P_KEYBOARD:
            used = slider_mouse(k, &kb_delay, a, sx, sy) || slider_mouse(k, &kb_rate, a, sx, sy);
            if (used) keyboard_apply_now();
            break;
        }
        if (!used) r = dialog_mouse(d, a, sx, sy);
    }
    if (ev == EV_KEY) {
        if (k == P_REGISTRY && KEY_CHAR(a) == 13 && d->focus >= 0 &&
            d->c[d->focus].type == DC_FIELD) return 0;
        if (k == P_FONTS && d->focus < fonts_install_at && (KEY_SCAN(a) == K_UP || KEY_SCAN(a) == K_DOWN) && !KEY_CHAR(a)) {
            list_key(&fn_list, a); fonts_buttons();
        } else r = dialog_key(d, a, HOST.shift);
    }
    applet_result(k, r);
    dialog_sync(d);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The main window: the applets as large icons                        */
#define CELL_W 104
#define CELL_H 78
static int sel = 0, hot = -1;
static unsigned last_click;
static int last_i = -1;
static int X0, Y0, W, H;
enum { M_OPEN = 1, M_CLOSE, M_ABOUT };
static struct menu_item m_file[] = { { "&Open", "Enter", M_OPEN, 0 }, { "", 0, 0, MI_SEP }, { "&Close", "Alt+F4", M_CLOSE, 0 } };
static struct menu_item m_help[] = { { "&About Control Panel", 0, M_ABOUT, 0 } };
static struct menu menus[] = { { "&File", m_file, 3 }, { "&Help", m_help, 1 } };
static struct menubar bar = { menus, 2, 0, 0, 0, -1, 0 };
static struct popup ctx;
static struct menu_item m_ctx[] = { { "&Open", 0, M_OPEN, 0 } };
static void layout(void)
{
    X0 = HOST.x + 3; Y0 = HOST.y + TITLE_H; W = HOST.w - 6; H = HOST.h - TITLE_H - 4;
    bar.x = X0; bar.y = Y0; bar.w = W;
}
static int cols(void) { int c = (W - 16) / CELL_W; return c < 1 ? 1 : c; }
static void cell(int i, int *x, int *y)
{
    *x = X0 + 10 + (i % cols()) * CELL_W;
    *y = Y0 + MENUBAR_H + 12 + (i / cols()) * CELL_H;
}
static int cell_at(int sx, int sy)
{
    int i, x, y;
    for (i = 0; i < P_COUNT; i++) {
        cell(i, &x, &y);
        if (sx >= x && sx < x + CELL_W - 4 && sy >= y && sy < y + CELL_H - 4) return i;
    }
    return -1;
}
static void paint_main(void)
{
    int i, x, y, tw;
    char t[24];
    layout();
    ui_rect(X0, Y0 + MENUBAR_H, W, H - MENUBAR_H - 22, C_PAPER);
    for (i = 0; i < P_COUNT; i++) {
        cell(i, &x, &y);
        if (i == hot && i != sel) ui_rect(x + 2, y, CELL_W - 8, CELL_H - 6, C_LIGHT);
        ui_icon(x + (CELL_W - 4 - 44) / 2, y + 4, applets[i].icon);
        text_fit(applets[i].label, CELL_W - 8, t);
        tw = ui_measure(t);
        if (i == sel) {
            ui_rect(x + (CELL_W - 4 - tw) / 2 - 3, y + 50, tw + 6, 18, C_TITLE);
            ui_text(x + (CELL_W - 4 - tw) / 2, y + 51, t, C_PAPER);
            if (HOST.active) draw_focus(x + (CELL_W - 4 - tw) / 2 - 3, y + 50, tw + 6, 18);
        } else ui_text(x + (CELL_W - 4 - tw) / 2, y + 51, t, C_INK);
    }
    y = Y0 + H - 20;
    ui_rect(X0, y - 1, W, 21, C_FACE);
    ui_inset(X0 + 2, y, W - 4, 19);
    draw_frame_text(X0 + 8, y + 1, W - 16, status[0] ? status : sel >= 0 ? applets[sel].desc : "", C_INK);
    menubar_draw(&bar);
    if (ctx.open) popup_draw(&ctx);
}
static void command(int id)
{
    status[0] = 0;
    if (id == M_OPEN) open_applet(sel, 0);
    if (id == M_CLOSE) app_close();
    if (id == M_ABOUT) message("About Control Panel", "CiukiOS Control Panel\nEvery system option in one place.\nA modern Retro OS");
}
static int main_mouse(int kind, int sx, int sy)
{
    int r, i;
    layout();
    if (dlg.open) { if (kind == MOUSE_DOWN) app_window_cmd(dlg.win, 1); return 0; }
    if (ctx.open) { r = popup_mouse(&ctx, kind, sx, sy); if (r >= 0) command(r); return 1; }
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    i = cell_at(sx, sy);
    if (kind == MOUSE_DOWN) {
        if (i >= 0 && i == last_i && (unsigned)(HOST.ticks - last_click) < (unsigned)HOST.dblclick) {
            last_i = -1; sel = i; open_applet(i, 0); return 1;
        }
        last_i = i; last_click = HOST.ticks;
        if (i >= 0) sel = i;
        return 1;
    }
    if (kind == MOUSE_RIGHT && i >= 0) {
        sel = i;
        popup_open(&ctx, m_ctx, 1, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        return 1;
    }
    return 0;
}
static int main_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), r, c;
    layout();
    if (ctx.open) { r = popup_key(&ctx, key); if (r >= 0) command(r); return 1; }
    r = menubar_key(&bar, key, HOST.shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    c = cols();
    if (ch == 13) { open_applet(sel, 0); return 1; }
    if (!ch || ch == 0xE0) {
        if (s == K_LEFT && sel > 0) sel--;
        else if (s == K_RIGHT && sel + 1 < P_COUNT) sel++;
        else if (s == K_UP && sel >= c) sel -= c;
        else if (s == K_DOWN && sel + c < P_COUNT) sel += c;
        else if (s == K_HOME) sel = 0;
        else if (s == K_END) sel = P_COUNT - 1;
        else return 0;
        return 1;
    }
    if (ch > ' ') {
        int i;
        for (i = 1; i <= P_COUNT; i++) {
            int k = (sel + i) % P_COUNT;
            if (to_upper(applets[k].label[0]) == to_upper((char)ch)) { sel = k; return 1; }
        }
    }
    return 0;
}
static char pending_arg[APP_ARG_BYTES];      /* opened after the main window */
/* Opened for one applet (the top bar, a desktop menu): that applet stands
 * alone. The main window stays hidden and the module ends with the applet. */
static int main_hidden, hide_main;
static void open_argument(const char *arg)
{
    int k;
    if (!arg[0]) return;
    if (!str_nicmp(arg, "font:", 5)) { open_applet(P_FONTS, arg + 5); return; }
    for (k = 0; k < P_COUNT; k++) if (!str_icmp(arg, applets[k].key)) { sel = k; open_applet(k, 0); return; }
}

int app_event(int ev, int a, int b, int c)
{
    int k, r, orig = ev;
    if (ev == EV_OPEN) {
        char arg[APP_ARG_BYTES];
        if (a == 2) {
            for (k = 0; k < P_COUNT; k++) {
                if (k == P_DATETIME && dt_sync_pending) continue;
                ad[k].win = 0; ad[k].open = 0;
            }
            dlg.win = 0; dialog_sync(&dlg);
            return 1;
        }
        str_copy(app_title, "Control Panel");
        if (!HDR_WIDTH) {
            HDR_WIDTH = 4 * CELL_W + 26;
            HDR_HEIGHT = TITLE_H + MENUBAR_H + 4 * CELL_H + 60;
            main_hidden = 1;                 /* a fresh load: not shown yet */
        }
        /* The shell shows the main window on every open. With an applet
         * argument it goes back into hiding unless the user had it open. */
        hide_main = APP_ARG[0] && main_hidden;
        if (!APP_ARG[0]) main_hidden = 0;
        str_ncopy(pending_arg, APP_ARG, APP_ARG_BYTES);
        APP_ARG[0] = 0;
        (void)arg;
        return 1;
    }
    /* A message box first (modal to everything here). */
    if (dialog_mine(&dlg)) {
        if (ev == EV_PAINT) { dialog_draw(&dlg); return 0; }
        if (ev == EV_CLOSE) { dlg.win = 0; dlg.open = 0; return 0; }
        if (ev == EV_KEY) dialog_key(&dlg, a, HOST.shift);
        if (ev == EV_MOUSE) {
            int sx = HOST.x + b, sy = HOST.y + TITLE_H + c;
            if (a == MOUSE_HOVER) { ui_dirty = 0; dialog_hover(&dlg, sx, sy); return ui_dirty; }
            dialog_mouse(&dlg, a, sx, sy);
        }
        dialog_sync(&dlg);
        return 1;
    }
    k = applet_here();
    if (k >= 0) { r = applet_event(k, ev, a, b, c); dialog_sync(&dlg); return r; }
    r = dialog_pre(&dlg, WIN_CONTROL, &ev, &a);
    if (r >= 0) return r;
    switch (ev) {
    case EV_PAINT: paint_main(); return 0;
    case EV_KEY: r = main_key(a); break;
    case EV_MOUSE:
        if (a == MOUSE_HOVER) {
            int sx = HOST.x + b, sy = HOST.y + TITLE_H + c, i;
            layout();
            ui_dirty = 0;
            if (ctx.open) popup_mouse(&ctx, MOUSE_HOVER, sx, sy);
            else if (menubar_open(&bar)) menubar_mouse(&bar, MOUSE_HOVER, sx, sy);
            else { i = cell_at(sx, sy); if (i != hot) { hot = i; ui_dirty = 1; } }
            return ui_dirty;
        }
        r = main_mouse(a, HOST.x + b, HOST.y + TITLE_H + c);
        break;
    case EV_CLOSE:
        return 0;
    case EV_SUSPEND:
        for (k = 0; k < P_COUNT; k++) if (ad[k].open) return 1;      /* an applet is open */
        return 0;
    case EV_POLL:
        if (dt_sync_pending) {
            dt_sync_pending = 0;
            if (datetime_network_read()) {
                dt_to_fields();
                datetime_save();
                app_log("[CONTROL] time zone detected", dt_zone);
            } else message("Date and Time", "Time zone lookup failed. Start networking and try again.");
            dialog_sync(&dlg);
            return 1;
        }
        if (pending_arg[0]) {
            char arg[APP_ARG_BYTES];
            str_copy(arg, pending_arg);
            pending_arg[0] = 0;
            open_argument(arg);
            if (hide_main) { hide_main = 0; app_window_cmd(WIN_CONTROL, 5); }
            dialog_sync(&dlg);
            return 1;
        }
        if (main_hidden && !dlg.open) {
            for (k = 0; k < P_COUNT; k++) if (ad[k].open) return 0;
            if (dt_sync_pending) return 0;
            /* Its last applet closed: end with the hidden main window.
             * app_close() would close whichever window is active instead. */
            main_hidden = 0;
            app_window_cmd(WIN_CONTROL, 2);
        }
        return 0;
    default: r = 0;
    }
    r = dialog_post(&dlg, WIN_CONTROL, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
