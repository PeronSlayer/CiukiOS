/* Devices: the Device Manager (the devices of this computer by class, with
 * their properties) and the installed drivers (enable, disable, remove, and
 * Add New Driver from a package folder with DRIVER.INF).
 *
 * \DRIVERS\DRIVERS.CFG lists the installed drivers, one per line:
 *     E CLASS NAME [@PCI|@PCI=VVVV:DDDD,...|@APM] COMMAND...
 * E is 1 enabled or 0 disabled. @PCI matches Hardware= in DRIVER.INF;
 * %PKT%, %IRQ%, and %IO% are supplied by LOADDRV for the matched device.
 * \DRIVERS\LOADDRV.COM runs the enabled ones at startup and writes
 * \DRIVERS\LOADDRV.LOG. Each driver's files and its DRIVER.INF are kept in
 * \DRIVERS\<CLASS>\<NAME>. */
#include "app.h"

/* ------------------------------------------------------------------ */
/* Devices                                                             */
enum { K_PROC, K_SYSTEM, K_DISPLAY, K_SOUND, K_NET, K_STORAGE, K_USB, K_KEYB, K_MOUSE,
       K_DISK, K_PORTS, K_OTHER, K_COUNT };
static const char *class_names[K_COUNT] = {
    "Processors", "System devices", "Display adapters", "Sound, video and game controllers",
    "Network adapters", "Storage controllers", "USB controllers", "Keyboards",
    "Mice and other pointing devices", "Disk drives", "Ports (COM and LPT)", "Other devices" };
static const u8 class_icons[K_COUNT] = { ICON_COMPUTER, ICON_COMPUTER, ICON_DISPLAY, ICON_AUDIO, ICON_NETWORK,
    ICON_DISK, ICON_USB, ICON_KEYBOARD, ICON_MOUSE, ICON_DISK, ICON_PROGRAM, ICON_DEVICES };
#define MAX_DEV 48
struct device {
    u8 kind, pci, bus, devfn;
    u16 vendor, device, cls, io, irq;
    char name[44];
    char where[28];
};
static struct device devs[MAX_DEV];
static int ndev;
static u8 open_class[K_COUNT];
static char active_audio[16], active_video[8];

struct known { u16 vendor, device; const char *name; };
static const struct known known[] = {
    { 0x8086, 0x1237, "Intel 440FX host bridge" }, { 0x8086, 0x7000, "Intel PIIX3 ISA bridge" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE controller" }, { 0x8086, 0x7020, "Intel PIIX3 USB controller (UHCI)" },
    { 0x8086, 0x7113, "Intel PIIX4 power management" }, { 0x8086, 0x7110, "Intel PIIX4 ISA bridge" },
    { 0x8086, 0x7111, "Intel PIIX4 IDE controller" }, { 0x8086, 0x7112, "Intel PIIX4 USB controller" },
    { 0x8086, 0x29C0, "Intel Q35 host bridge" }, { 0x8086, 0x2918, "Intel ICH9 LPC bridge" },
    { 0x8086, 0x2922, "Intel ICH9 SATA controller (AHCI)" }, { 0x8086, 0x2930, "Intel ICH9 SMBus controller" },
    { 0x8086, 0x293E, "Intel ICH9 High Definition Audio" }, { 0x8086, 0x2668, "Intel ICH6 High Definition Audio" },
    { 0x8086, 0x2415, "Intel 82801AA AC'97 Audio" }, { 0x8086, 0x2445, "Intel 82801BA AC'97 Audio" },
    { 0x8086, 0x24C5, "Intel 82801DB AC'97 Audio" }, { 0x8086, 0x24D5, "Intel 82801EB AC'97 Audio" },
    { 0x8086, 0x100E, "Intel PRO/1000 MT Ethernet (82540EM)" }, { 0x8086, 0x10D3, "Intel 82574L Ethernet" },
    { 0x10EC, 0x8139, "Realtek RTL8139 Ethernet" }, { 0x10EC, 0x8029, "Realtek RTL8029 (NE2000) Ethernet" },
    { 0x1234, 0x1111, "Standard VGA with VBE (Bochs)" }, { 0x1013, 0x00B8, "Cirrus Logic GD5446" },
    { 0x1274, 0x5000, "Ensoniq AudioPCI ES1370" }, { 0x1274, 0x1371, "Ensoniq AudioPCI ES1371" },
    { 0x125D, 0x1978, "ESS Maestro-2E (ES1978)" }, { 0x1002, 0x4C4D, "ATI Rage Mobility P/M" },
    { 0x1B36, 0x000D, "QEMU xHCI USB controller" }, { 0x1AF4, 0x1000, "Virtio network device" },
    { 0x1AF4, 0x1001, "Virtio block device" }, { 0, 0, 0 } };
struct vendor { u16 id; const char *name; };
static const struct vendor vendors[] = {
    { 0x8086, "Intel" }, { 0x1022, "AMD" }, { 0x1002, "ATI" }, { 0x10DE, "NVIDIA" }, { 0x10EC, "Realtek" },
    { 0x1106, "VIA" }, { 0x1274, "Ensoniq" }, { 0x125D, "ESS" }, { 0x1013, "Cirrus Logic" },
    { 0x1234, "Bochs/QEMU" }, { 0x1AF4, "Red Hat (virtio)" }, { 0x1B36, "Red Hat (QEMU)" },
    { 0x15AD, "VMware" }, { 0x80EE, "VirtualBox" }, { 0x104C, "Texas Instruments" }, { 0, 0 } };

static int pci_word(int bus, int devfn, int reg, u16 *v)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0xB109; r.bx = (u16)((bus << 8) | devfn); r.di = (u16)reg;
    r.ds = r.es = app_seg();
    if (intr(0x1A, &r) || (r.ax & 0xFF00)) return 0;
    *v = r.cx;
    return 1;
}
static const char *vendor_name(u16 id)
{
    int i;
    for (i = 0; vendors[i].id; i++) if (vendors[i].id == id) return vendors[i].name;
    return 0;
}
static int kind_of(u16 cls)
{
    switch (cls >> 8) {
    case 0x01: return K_STORAGE;
    case 0x02: return K_NET;
    case 0x03: return K_DISPLAY;
    case 0x04: return K_SOUND;
    case 0x06: return K_SYSTEM;
    case 0x0C: return cls == 0x0C03 ? K_USB : K_SYSTEM;
    case 0x09: return (cls & 0xFF) == 2 ? K_MOUSE : K_KEYB;
    }
    return K_OTHER;
}
static const char *class_text(u16 cls, u8 progif)
{
    switch (cls) {
    case 0x0101: return "IDE controller";
    case 0x0106: return "SATA controller";
    case 0x0180: return "Storage controller";
    case 0x0200: return "Ethernet controller";
    case 0x0300: return "VGA-compatible display";
    case 0x0401: return "Audio device";
    case 0x0403: return "High Definition Audio";
    case 0x0600: return "Host bridge";
    case 0x0601: return "ISA bridge";
    case 0x0604: return "PCI bridge";
    case 0x0680: return "Bridge";
    case 0x0C03: return progif == 0 ? "USB controller (UHCI)" : progif == 0x10 ? "USB controller (OHCI)" :
                        progif == 0x20 ? "USB controller (EHCI)" : "USB controller (xHCI)";
    case 0x0C05: return "SMBus controller";
    }
    return "Device";
}
static struct device *new_dev(int kind, const char *name, const char *where)
{
    struct device *d;
    if (ndev >= MAX_DEV) return 0;
    d = &devs[ndev++];
    mem_set(d, 0, sizeof *d);
    d->kind = (u8)kind;
    str_ncopy(d->name, name, sizeof d->name);
    str_ncopy(d->where, where, sizeof d->where);
    return d;
}
static void hex4(char *out, u16 v) { fmt_hex4(out, v); }
static void read_active(void)
{
    char buf[512], *p;
    int f = dos_open("\\DRIVERS\\ACTIVE.CFG", 0), n;
    active_audio[0] = active_video[0] = 0;
    if (f < 0) return;
    n = dos_read(f, buf, sizeof buf - 1);
    dos_close(f);
    if (n <= 0) return;
    buf[n] = 0;
    for (p = buf; *p; p++) {
        if ((p == buf || p[-1] == '\n') && !str_nicmp(p, "AUDIO=", 6)) { int k = 0; p += 6; while (*p > ' ' && k < 15) active_audio[k++] = *p++; active_audio[k] = 0; }
        if ((p == buf || p[-1] == '\n') && !str_nicmp(p, "VIDEO=", 6)) { int k = 0; p += 6; while (*p > ' ' && k < 7) active_video[k++] = *p++; active_video[k] = 0; }
    }
}
static void scan_devices(void)
{
    struct regs r;
    char vendor[13], t[44], w[28];
    int family, bus, dev, fn, last_bus = 0, i;
    ndev = 0;
    read_active();
    family = cpu_vendor(vendor);
    str_copy(t, vendor[0] ? vendor : "80386 compatible");
    if (vendor[0]) { str_cat(t, " family "); fmt_u32(t + str_len(t), (u32)family); }
    new_dev(K_PROC, t, "CPU");
    new_dev(K_SYSTEM, "System timer (8254)", "I/O 0040-0043, IRQ 0");
    new_dev(K_SYSTEM, "Programmable interrupt controllers (8259)", "I/O 0020, 00A0");
    new_dev(K_SYSTEM, "Real-time clock (CMOS)", "I/O 0070-0071, IRQ 8");
    new_dev(K_SYSTEM, "DMA controllers (8237)", "I/O 0000, 00C0");
    new_dev(K_KEYB, "Standard PS/2 keyboard (i8042)", "I/O 0060, 0064, IRQ 1");
    mem_set(&r, 0, sizeof r); r.ds = r.es = app_seg();
    intr(0x11, &r);
    if (peek16(0, 0x33 * 4 + 2)) new_dev(K_MOUSE, "PS/2 mouse (INT 33h driver)", "IRQ 12");
    /* Disks: the BIOS drives. */
    {
        int floppies = (r.ax & 1) ? ((r.ax >> 6) & 3) + 1 : 0;
        for (i = 0; i < floppies && i < 2; i++) { str_copy(t, "Floppy disk drive "); t[18] = (char)('A' + i); t[19] = ':'; t[20] = 0; new_dev(K_DISK, t, "BIOS drive 00h"); devs[ndev - 1].where[12] = (char)('0' + i); }
    }
    for (i = 0; i < 4; i++) {
        mem_set(&r, 0, sizeof r);
        r.ax = 0x0800; r.dx = (u16)(0x80 + i); r.ds = r.es = app_seg();
        if (intr(0x13, &r) || (r.ax & 0xFF00)) break;
        {
            u32 cyl = ((r.cx >> 8) | ((u32)(r.cx & 0xC0) << 2)) + 1, heads = (r.dx >> 8) + 1, spt = r.cx & 63;
            u32 mb = cyl * heads * spt / 2048;
            str_copy(t, "Hard disk "); fmt_u32(t + str_len(t), (u32)i); str_cat(t, " (");
            fmt_u32(t + str_len(t), mb); str_cat(t, " MB)");
            str_copy(w, "BIOS drive 8"); w[12] = (char)('0' + i); w[13] = 'h'; w[14] = 0;
            new_dev(K_DISK, t, w);
        }
        if (i + 1 >= (r.dx & 0xFF)) break;
    }
    for (i = 0; i < 4; i++) {
        u16 port = peek16(0x40, (u16)(i * 2));
        if (!port) continue;
        str_copy(t, "Communications port (COM"); t[24] = (char)('1' + i); t[25] = ')'; t[26] = 0;
        str_copy(w, "I/O "); hex4(w + 4, port);
        new_dev(K_PORTS, t, w);
    }
    for (i = 0; i < 3; i++) {
        u16 port = peek16(0x40, (u16)(8 + i * 2));
        if (!port) continue;
        str_copy(t, "Printer port (LPT"); t[17] = (char)('1' + i); t[18] = ')'; t[19] = 0;
        str_copy(w, "I/O "); hex4(w + 4, port);
        new_dev(K_PORTS, t, w);
    }
    /* PCI: every function the BIOS knows. */
    mem_set(&r, 0, sizeof r);
    r.ax = 0xB101; r.ds = r.es = app_seg();
    if (intr(0x1A, &r) || (r.ax & 0xFF00)) return;
    last_bus = r.cx & 0xFF;
    for (bus = 0; bus <= last_bus && bus < 8; bus++)
        for (dev = 0; dev < 32; dev++)
            for (fn = 0; fn < 8; fn++) {
                u16 vid, did, cls, pi, bar, irq, head;
                int devfn = (dev << 3) | fn, k;
                struct device *d;
                const char *v;
                if (!pci_word(bus, devfn, 0, &vid) || vid == 0xFFFF) { if (!fn) break; continue; }
                pci_word(bus, devfn, 2, &did);
                pci_word(bus, devfn, 0x0A, &cls);
                pci_word(bus, devfn, 0x08, &pi);
                pci_word(bus, devfn, 0x3C, &irq);
                pci_word(bus, devfn, 0x10, &bar);
                t[0] = 0;
                for (k = 0; known[k].vendor; k++) if (known[k].vendor == vid && known[k].device == did) str_copy(t, known[k].name);
                if (!t[0]) {
                    v = vendor_name(vid);
                    str_copy(t, v ? v : "PCI");
                    str_cat(t, " ");
                    str_cat(t, class_text(cls, (u8)(pi >> 8)));
                }
                str_copy(w, "PCI bus "); fmt_u32(w + str_len(w), (u32)bus); str_cat(w, ", device ");
                fmt_u32(w + str_len(w), (u32)dev); str_cat(w, ", function "); fmt_u32(w + str_len(w), (u32)fn);
                d = new_dev(kind_of(cls), t, w);
                if (!d) return;
                d->pci = 1; d->bus = (u8)bus; d->devfn = (u8)devfn;
                d->vendor = vid; d->device = did; d->cls = cls;
                d->io = (bar & 1) ? bar & 0xFFFC : 0;
                d->irq = irq & 0xFF;
                if (!fn) {
                    pci_word(bus, devfn, 0x0E, &head);
                    if (!(head & 0x80)) break;
                }
            }
    for (i = 0; i < K_COUNT; i++) open_class[i] = 1;
}

/* ------------------------------------------------------------------ */
/* Drivers                                                             */
#define MAX_DRV 24
struct driver { u8 enabled; char cls[9], name[9]; char cmd[64]; char status[12]; };
static struct driver drvs[MAX_DRV];
static int ndrv;
static const char cfg_path[] = "\\DRIVERS\\DRIVERS.CFG";
static char filebuf[2048];
static void next_token(const char **p, char *out, int n)
{
    int k = 0;
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p > ' ' && k < n - 1) out[k++] = *(*p)++;
    out[k] = 0;
    while (**p > ' ') (*p)++;
}
static int read_file(const char *path, char *buf, int n)
{
    int f = dos_open(path, 0), got;
    if (f < 0) { buf[0] = 0; return f; }
    got = dos_read(f, buf, n - 1);
    dos_close(f);
    if (got < 0) got = 0;
    buf[got] = 0;
    return got;
}
static void load_drivers(void)
{
    char log[512];
    const char *p;
    int i;
    ndrv = 0;
    read_file(cfg_path, filebuf, sizeof filebuf);
    read_file("\\DRIVERS\\LOADDRV.LOG", log, sizeof log);
    for (p = filebuf; *p && ndrv < MAX_DRV;) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) p++;
        if (*line == '0' || *line == '1') {
            struct driver *d = &drvs[ndrv];
            const char *q = line + 1;
            int k = 0;
            d->enabled = *line == '1';
            next_token(&q, d->cls, sizeof d->cls);
            next_token(&q, d->name, sizeof d->name);
            while (*q == ' ') q++;
            while (*q && *q != '\r' && *q != '\n' && k < 63) d->cmd[k++] = *q++;
            d->cmd[k] = 0;
            if (!d->name[0]) continue;
            str_copy(d->status, d->enabled ? "Not loaded" : "Disabled");
            for (i = 0; log[i]; i++) {
                int n = str_len(d->name);
                if ((i == 0 || log[i - 1] == '\n') && !str_nicmp(log + i, d->name, n) && log[i + n] == ' ') {
                    str_copy(d->status, !str_nicmp(log + i + n + 1, "OK", 2) ? "Loaded" :
                             !str_nicmp(log + i + n + 1, "SKIP", 4) ? "No match" : "Error");
                    if (!d->enabled) str_copy(d->status, "Disabled");
                }
            }
            ndrv++;
        }
    }
}
static int save_drivers(void)
{
    int f = dos_create(cfg_path), i;
    static const char head[] =
        "; CiukiOS installed drivers (Control Panel > Drivers).\r\n"
        "; LOADDRV.COM runs the enabled ones at startup.\r\n"
        "; E CLASS NAME [@PCI|@PCI=VVVV:DDDD,...|@APM] COMMAND...\r\n"
        "; @PCI matches Hardware= in DRIVER.INF; %PKT%, %IRQ%, %IO% use the match.\r\n";
    if (f < 0) return f;
    dos_write(f, head, sizeof head - 1);
    for (i = 0; i < ndrv; i++) {
        char line[96];
        line[0] = drvs[i].enabled ? '1' : '0'; line[1] = ' '; line[2] = 0;
        str_cat(line, drvs[i].cls); str_cat(line, " ");
        str_cat(line, drvs[i].name); str_cat(line, " ");
        str_cat(line, drvs[i].cmd); str_cat(line, "\r\n");
        dos_write(f, line, str_len(line));
    }
    dos_close(f);
    return 0;
}
/* key=value from an INI text ("" when absent). */
static void ini_get(const char *text, const char *key, char *out, int n)
{
    int kl = str_len(key), k;
    const char *p = text;
    out[0] = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!str_nicmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            for (k = 0; k < n - 1 && *p && *p != '\r' && *p != '\n'; k++) out[k] = *p++;
            while (k > 0 && out[k - 1] == ' ') k--;
            out[k] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}
static void driver_dir(const struct driver *d, char *out)
{
    str_copy(out, "C:\\DRIVERS\\");
    str_cat(out, d->cls);
    str_cat(out, "\\");
    str_cat(out, d->name);
}
/* The installed driver for a PCI device, from its DRIVER.INF Hardware. */
static int driver_for(const struct device *dv)
{
    char p[SYS_PATH], hw[80], id[10];
    int i;
    if (!dv->pci) return -1;
    hex4(id, dv->vendor); id[4] = ':'; hex4(id + 5, dv->device);
    for (i = 0; i < ndrv; i++) {
        driver_dir(&drvs[i], p);
        str_cat(p, "\\DRIVER.INF");
        if (read_file(p, filebuf, sizeof filebuf) <= 0) continue;
        ini_get(filebuf, "Hardware", hw, sizeof hw);
        {
            const char *h = hw;
            while (*h) {
                if (!str_nicmp(h, id, 9)) return i;
                while (*h && *h != ',') h++;
                while (*h == ',' || *h == ' ') h++;
            }
        }
    }
    return -1;
}
static const char *builtin_driver(const struct device *d)
{
    switch (d->kind) {
    case K_DISPLAY: return active_video[0] ? (active_video[1] == 'B' ? "CiukiOS VBE display (built in)" : "CiukiOS VGA display (built in)") : "CiukiOS display (built in)";
    case K_SOUND:
        if (!str_icmp(active_audio, "ICH_AC97") && d->vendor == 0x8086) return "CiukiOS AC'97 player (built in)";
        if (!str_icmp(active_audio, "VSBHDA")) return "VSBHDA (application launchers)";
        return 0;
    case K_KEYB: return "CiukiOS i8042 keyboard (built in)";
    case K_MOUSE: return "CiukiOS PS/2 mouse (built in)";
    case K_DISK: return "BIOS disk services";
    case K_PORTS: return "BIOS serial and printer services";
    case K_PROC: case K_SYSTEM: return "System (built in)";
    case K_STORAGE: return "BIOS disk services";
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rows of the device tree                                             */
#define MAX_ROWS (MAX_DEV + K_COUNT)
static int rows[MAX_ROWS];                  /* -1 - class, or a device */
static int nrows;
static void build_rows(void)
{
    int k, i;
    nrows = 0;
    for (k = 0; k < K_COUNT; k++) {
        int any = 0;
        for (i = 0; i < ndev; i++) if (devs[i].kind == k) any = 1;
        if (!any) continue;
        rows[nrows++] = -1 - k;
        if (!open_class[k]) continue;
        for (i = 0; i < ndev; i++) if (devs[i].kind == k) rows[nrows++] = i;
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
static int tab, sel_row, top_row, sel_drv, top_drv;
static unsigned last_click;
static int last_i = -1;
static char status[80], msg[200];
static int X0, Y0, W, H, tab_y, lx, ly, lw, lh, btn_y;
#define ROW_H 18
static void layout(void)
{
    X0 = HOST.x + 3; Y0 = HOST.y + TITLE_H; W = HOST.w - 6; H = HOST.h - TITLE_H - 4;
    tab_y = Y0 + MENUBAR_H + 6;
    lx = X0 + 8; ly = tab_y + 28; lw = W - 16 - 16; lh = Y0 + H - 22 - 40 - ly;
    btn_y = ly + lh + 8;
}
static int visible_rows(void) { int r = (lh - (tab ? 24 : 4)) / ROW_H; return r < 1 ? 1 : r; }

enum { C_PROPS = 1, C_SCAN, C_HWDETECT, C_ADD, C_ENABLE, C_DISABLE, C_REMOVE, C_CLOSE, C_TAB_DEV, C_TAB_DRV, C_ABOUT };
static struct menu_item m_action[] = {
    { "P&roperties", "Alt+Enter", C_PROPS, 0 }, { "", 0, 0, MI_SEP },
    { "&Scan for Hardware Changes", "F5", C_SCAN, 0 }, { "Run &Hardware Detection", 0, C_HWDETECT, 0 },
    { "", 0, 0, MI_SEP }, { "&Close", "Alt+F4", C_CLOSE, 0 } };
static struct menu_item m_driver[] = {
    { "&Add New Driver...", 0, C_ADD, 0 }, { "", 0, 0, MI_SEP }, { "&Enable", 0, C_ENABLE, 0 },
    { "&Disable", 0, C_DISABLE, 0 }, { "&Remove", "Del", C_REMOVE, 0 } };
static struct menu_item m_view[] = { { "&Devices by Type", 0, C_TAB_DEV, 0 }, { "Installed D&rivers", 0, C_TAB_DRV, 0 } };
static struct menu_item m_help[] = { { "&About Devices", 0, C_ABOUT, 0 } };
static struct menu menus[] = { { "&Action", m_action, 6 }, { "&Driver", m_driver, 5 }, { "&View", m_view, 2 }, { "&Help", m_help, 1 } };
static struct menubar bar = { menus, 4, 0, 0, 0, -1, 0 };
static struct popup ctx;
static struct menu_item m_ctx_dev[] = { { "P&roperties", 0, C_PROPS, 0 }, { "", 0, 0, MI_SEP }, { "&Scan for Hardware Changes", 0, C_SCAN, 0 } };
static struct menu_item m_ctx_drv[] = { { "&Enable", 0, C_ENABLE, 0 }, { "&Disable", 0, C_DISABLE, 0 }, { "Re&move", 0, C_REMOVE, 0 },
                                        { "", 0, 0, MI_SEP }, { "P&roperties", 0, C_PROPS, 0 }, { "", 0, 0, MI_SEP }, { "&Add New Driver...", 0, C_ADD, 0 } };
static int hot_btn = -1;
static void flag(struct menu_item *it, int f, int on) { if (on) it->flags |= f; else it->flags &= ~f; }
static void update_menus(void)
{
    int d = tab == 1 && sel_drv >= 0 && sel_drv < ndrv;
    flag(&m_driver[2], MI_DISABLED, !d || drvs[sel_drv].enabled); flag(&m_driver[3], MI_DISABLED, !d || !drvs[sel_drv].enabled);
    flag(&m_driver[4], MI_DISABLED, !d);
    flag(&m_ctx_drv[0], MI_DISABLED, !d || drvs[sel_drv].enabled); flag(&m_ctx_drv[1], MI_DISABLED, !d || !drvs[sel_drv].enabled);
    flag(&m_ctx_drv[2], MI_DISABLED, !d); flag(&m_ctx_drv[4], MI_DISABLED, !d);
    flag(&m_view[0], MI_CHECKED, tab == 0); flag(&m_view[1], MI_CHECKED, tab == 1);
}

/* Dialogs: a modal one (messages, Add New Driver), Properties modeless. */
static struct dialog dlg, pdlg;
static struct dctl dc[12], pdc[14];
static char pl[10][72], ptitle[48];
static int dlg_kind, prop_icon;
enum { D_NONE, D_MSG, D_ADD, D_CONFIRM_ADD, D_REMOVE };
static struct field add_field;
static char add_buf[SYS_PATH], add_dir[SYS_PATH];
static void ctl(struct dctl *c, int type, int x, int y, int w, int h, const char *text, int id)
{
    mem_set(c, 0, sizeof *c);
    c->type = type; c->x = x; c->y = y; c->w = w; c->h = h; c->text = text; c->id = id;
}
static void message(const char *title, const char *text)
{
    msgbox(&dlg, title, text, "OK");
    dlg_kind = D_MSG;
    app_log("[DEVICES] message", text);
}
static void device_properties(int i)
{
    struct device *d = &devs[i];
    int k = 0, y = 0, drv = driver_for(d);
    const char *b = builtin_driver(d);
    str_copy(pl[0], d->name);
    str_copy(pl[1], "Type: "); str_cat(pl[1], class_names[d->kind]);
    str_copy(pl[2], "Location: "); str_cat(pl[2], d->where);
    pl[3][0] = pl[4][0] = 0;
    if (d->pci) {
        str_copy(pl[3], "Hardware ID: PCI\\VEN_"); hex4(pl[3] + str_len(pl[3]), d->vendor);
        str_cat(pl[3], "&DEV_"); hex4(pl[3] + str_len(pl[3]), d->device);
        str_cat(pl[3], "&CC_"); hex4(pl[3] + str_len(pl[3]), d->cls);
        str_copy(pl[4], "Resources: ");
        if (d->io) { str_cat(pl[4], "I/O "); hex4(pl[4] + str_len(pl[4]), d->io); str_cat(pl[4], "  "); }
        if (d->irq && d->irq < 16) { str_cat(pl[4], "IRQ "); fmt_u32(pl[4] + str_len(pl[4]), d->irq); }
        if (!d->io && !(d->irq && d->irq < 16)) str_cat(pl[4], "memory mapped");
    }
    str_copy(pl[5], "Driver: ");
    if (drv >= 0) { str_cat(pl[5], drvs[drv].name); str_cat(pl[5], drvs[drv].enabled ? " (installed, enabled)" : " (installed, disabled)"); }
    else if (b) str_cat(pl[5], b);
    else str_cat(pl[5], "none installed");
    str_copy(pl[6], drv >= 0 || b ? "This device is working properly." : "No driver is installed for this device.");
    prop_icon = class_icons[d->kind];
    ctl(&pdc[k++], DC_LABEL, 52, 8, 0, 16, pl[0], 0); y = 44;
    ctl(&pdc[k++], DC_LABEL, 0, y, 0, 16, pl[1], 0); y += 22;
    ctl(&pdc[k++], DC_LABEL, 0, y, 0, 16, pl[2], 0); y += 22;
    if (pl[3][0]) { ctl(&pdc[k++], DC_LABEL, 0, y, 0, 16, pl[3], 0); y += 22; }
    if (pl[4][0]) { ctl(&pdc[k++], DC_LABEL, 0, y, 0, 16, pl[4], 0); y += 22; }
    ctl(&pdc[k++], DC_LABEL, 0, y, 0, 16, pl[5], 0); y += 30;
    ctl(&pdc[k++], DC_GROUP, 0, y, 440, 46, "Device status", 0);
    ctl(&pdc[k++], DC_LABEL, 12, y + 20, 0, 16, pl[6], 0); y += 58;
    if (drv < 0 && !b) { ctl(&pdc[k++], DC_BUTTON, 0, y, 150, 26, "&Add New Driver...", 10); }
    ctl(&pdc[k++], DC_BUTTON, 356, y, 84, 26, "OK", 1);
    str_ncopy(ptitle, d->name, 30);
    str_cat(ptitle, " Properties");
    if (pdlg.open) { pdlg.open = 0; dialog_sync(&pdlg); }
    dialog_show(&pdlg, ptitle, pdc, k, 460, y + 32, 1, 1);
    pdlg.modeless = 1;
    dialog_sync(&pdlg);
    app_log("[DEVICES] properties", d->name);
}
static void driver_properties(int i)
{
    char p[SYS_PATH], v[64];
    struct driver *d = &drvs[i];
    int k = 0, y = 0, n;
    driver_dir(d, p);
    str_cat(p, "\\DRIVER.INF");
    read_file(p, filebuf, sizeof filebuf);
    ini_get(filebuf, "Description", v, sizeof v);
    str_copy(pl[0], d->name);
    str_copy(pl[1], v[0] ? v : "(no description)");
    str_copy(pl[2], "Class: "); str_cat(pl[2], d->cls);
    ini_get(filebuf, "Version", v, sizeof v);
    str_copy(pl[3], "Version: "); str_cat(pl[3], v[0] ? v : "-");
    ini_get(filebuf, "Hardware", v, sizeof v);
    str_copy(pl[4], "Hardware: "); str_cat(pl[4], v[0] ? v : "any");
    str_copy(pl[5], "Command: "); str_ncopy(pl[5] + 9, d->cmd, 60);
    driver_dir(d, p);
    str_copy(pl[6], "Folder: "); str_cat(pl[6], p);
    str_copy(pl[7], "Status: "); str_cat(pl[7], d->status);
    str_cat(pl[7], d->enabled ? " (loaded at startup)" : "");
    prop_icon = ICON_PACKAGE;
    ctl(&pdc[k++], DC_LABEL, 52, 8, 0, 16, pl[0], 0); y = 44;
    for (n = 1; n < 8; n++) { ctl(&pdc[k++], DC_LABEL, 0, y, 0, 16, pl[n], 0); y += 22; }
    y += 8;
    ctl(&pdc[k++], DC_BUTTON, 356, y, 84, 26, "OK", 1);
    str_ncopy(ptitle, d->name, 30);
    str_cat(ptitle, " Properties");
    if (pdlg.open) { pdlg.open = 0; dialog_sync(&pdlg); }
    dialog_show(&pdlg, ptitle, pdc, k, 460, y + 32, 1, 1);
    pdlg.modeless = 1;
    dialog_sync(&pdlg);
    app_log("[DEVICES] driver properties", d->name);
}
static void add_dialog(const char *dir)
{
    ctl(&dc[0], DC_LABEL, 52, 4, 0, 16, "Insert the driver's disk, or type the folder", 0);
    ctl(&dc[1], DC_LABEL, 52, 24, 0, 16, "that holds its DRIVER.INF file.", 0);
    ctl(&dc[2], DC_LABEL, 0, 58, 0, 16, "&Folder:", 0);
    field_set(&add_field, add_buf, SYS_PATH, dir ? dir : "A:\\");
    add_field.sel = 1;
    ctl(&dc[3], DC_FIELD, 60, 56, 330, 22, 0, 0); dc[3].field = &add_field;
    ctl(&dc[4], DC_BUTTON, 214, 94, 84, 26, "&Next >", 1);
    ctl(&dc[5], DC_BUTTON, 306, 94, 84, 26, "Cancel", 2);
    dialog_show(&dlg, "Add New Driver", dc, 6, 410, 126, 1, 2);
    dlg.focus = 3;
    dlg_kind = D_ADD;
    app_log("[DEVICES] dialog", "Add New Driver");
}
/* Checks the package in add_dir; asks to install it. */
static char inf_name[16], inf_class[16], inf_version[16], inf_files[96], inf_load[64], inf_desc[64], inf_hardware[80];
static int valid_word(const char *s)
{
    int n = 0;
    while (*s) {
        char c = to_upper(*s++);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        n++;
    }
    return n > 0 && n <= 8;
}
static void add_check(void)
{
    char p[SYS_PATH], f[16];
    const char *q;
    int n = str_len(add_field.text), i;
    str_ncopy(add_dir, add_field.text, SYS_PATH);
    for (i = 0; add_dir[i]; i++) add_dir[i] = to_upper(add_dir[i]);
    while (n > 3 && add_dir[n - 1] == '\\') add_dir[--n] = 0;
    str_copy(p, add_dir);
    if (p[str_len(p) - 1] != '\\') str_cat(p, "\\");
    str_cat(p, "DRIVER.INF");
    if (read_file(p, filebuf, sizeof filebuf) < 0) { message("Add New Driver", "No DRIVER.INF was found in this folder."); return; }
    ini_get(filebuf, "Name", inf_name, sizeof inf_name);
    ini_get(filebuf, "Class", inf_class, sizeof inf_class);
    ini_get(filebuf, "Version", inf_version, sizeof inf_version);
    ini_get(filebuf, "Files", inf_files, sizeof inf_files);
    ini_get(filebuf, "Load", inf_load, sizeof inf_load);
    ini_get(filebuf, "Hardware", inf_hardware, sizeof inf_hardware);
    ini_get(filebuf, "Description", inf_desc, sizeof inf_desc);
    for (i = 0; inf_name[i]; i++) inf_name[i] = to_upper(inf_name[i]);
    for (i = 0; inf_class[i]; i++) inf_class[i] = to_upper(inf_class[i]);
    if (!valid_word(inf_name) || !valid_word(inf_class) || !inf_files[0] || !inf_load[0]) {
        message("Add New Driver", "DRIVER.INF needs Name and Class (up to eight\nletters or digits), Files and Load.");
        return;
    }
    /* Every file must be there; Load must start with one of them. */
    for (q = inf_files; *q;) {
        int k = 0;
        while (*q == ' ' || *q == ',') q++;
        while (*q && *q != ',' && *q != ' ' && k < 13) f[k++] = *q++;
        f[k] = 0;
        if (!k) break;
        str_copy(p, add_dir); if (p[str_len(p) - 1] != '\\') str_cat(p, "\\"); str_cat(p, f);
        if (dos_get_attr(p) < 0) { str_copy(msg, "The package is missing the file "); str_cat(msg, f); str_cat(msg, "."); message("Add New Driver", msg); return; }
    }
    str_copy(msg, "Install this driver?\n\n");
    str_cat(msg, inf_name); str_cat(msg, " ("); str_cat(msg, inf_class);
    if (inf_version[0]) { str_cat(msg, ", version "); str_cat(msg, inf_version); }
    str_cat(msg, ")\n");
    str_cat(msg, inf_desc[0] ? inf_desc : "No description.");
    str_cat(msg, "\nIt will be loaded when CiukiOS starts.");
    msgbox(&dlg, "Add New Driver", msg, "Yes|No");
    dlg_kind = D_CONFIRM_ADD;
    app_log("[DEVICES] confirm", inf_name);
}
static void add_install(void)
{
    char dst[SYS_PATH], src[SYS_PATH], f[16], cmd[64];
    const char *q;
    int r = 0, i;
    struct driver *d;
    load_drivers();
    for (i = 0; i < ndrv && str_icmp(drvs[i].name, inf_name); i++) ;
    if (i == ndrv && ndrv >= MAX_DRV) {
        message("Add New Driver", "The installed driver list is full."); return;
    }
    str_copy(dst, "C:\\DRIVERS\\"); str_cat(dst, inf_class); str_cat(dst, "\\"); str_cat(dst, inf_name);
    r = make_dirs(dst);
    if (r < 0) { message("Add New Driver", dos_error_text(r)); return; }
    for (q = inf_files; *q && r >= 0;) {
        int k = 0;
        char s2[SYS_PATH];
        while (*q == ' ' || *q == ',') q++;
        while (*q && *q != ',' && *q != ' ' && k < 13) f[k++] = to_upper(*q++);
        f[k] = 0;
        if (!k) break;
        str_copy(src, add_dir); if (src[str_len(src) - 1] != '\\') str_cat(src, "\\"); str_cat(src, f);
        str_copy(s2, dst); str_cat(s2, "\\"); str_cat(s2, f);
        r = tree_copy(src, s2, msg, sizeof msg);
    }
    if (r >= 0) {
        str_copy(src, add_dir); if (src[str_len(src) - 1] != '\\') str_cat(src, "\\"); str_cat(src, "DRIVER.INF");
        {
            char s2[SYS_PATH];
            str_copy(s2, dst); str_cat(s2, "\\DRIVER.INF");
            r = tree_copy(src, s2, msg, sizeof msg);
        }
    }
    if (r < 0) { message("Add New Driver", dos_error_text(r)); return; }
    str_copy(cmd, inf_hardware[0] ? "@PCI " : "");
    if (str_len(cmd) + str_len(dst + 2) + str_len(inf_load) + 2 >= sizeof cmd) {
        message("Add New Driver", "The driver's load command is too long."); return;
    }
    str_cat(cmd, dst + 2);                    /* [@PCI] \DRIVERS\CLASS\NAME\LOAD... */
    str_cat(cmd, "\\");
    str_ncopy(cmd + str_len(cmd), inf_load, 64 - str_len(cmd));
    load_drivers();
    for (i = 0; i < ndrv && str_icmp(drvs[i].name, inf_name); i++) ;
    if (i == ndrv) ndrv++;
    d = &drvs[i];
    d->enabled = 1;
    str_ncopy(d->cls, inf_class, 9);
    str_ncopy(d->name, inf_name, 9);
    str_ncopy(d->cmd, cmd, 64);
    save_drivers();
    load_drivers();
    app_log("[DEVICES] driver installed", inf_name);
    tab = 1;
    for (i = 0; i < ndrv; i++) if (!str_icmp(drvs[i].name, inf_name)) sel_drv = i;
    str_copy(msg, "The driver "); str_cat(msg, inf_name);
    str_cat(msg, " was installed.\nIt will be loaded the next time CiukiOS starts.");
    message("Add New Driver", msg);
}
static void command(int id)
{
    status[0] = 0;
    switch (id) {
    case C_PROPS:
        if (tab == 0 && sel_row >= 0 && sel_row < nrows && rows[sel_row] >= 0) device_properties(rows[sel_row]);
        else if (tab == 0 && sel_row >= 0 && sel_row < nrows) { int k = -1 - rows[sel_row]; open_class[k] = !open_class[k]; build_rows(); }
        else if (tab == 1 && sel_drv >= 0 && sel_drv < ndrv) driver_properties(sel_drv);
        break;
    case C_SCAN:
        scan_devices(); build_rows(); load_drivers();
        str_copy(status, "Hardware scan complete.");
        app_log("[DEVICES] scan", 0);
        break;
    case C_HWDETECT: app_command("\\DRIVERS\\HWDETECT.COM /SAVE"); break;
    case C_ADD: add_dialog(0); break;
    case C_ENABLE: case C_DISABLE:
        if (sel_drv < 0 || sel_drv >= ndrv) break;
        drvs[sel_drv].enabled = id == C_ENABLE;
        save_drivers(); load_drivers();
        app_log(id == C_ENABLE ? "[DEVICES] driver enabled" : "[DEVICES] driver disabled", drvs[sel_drv].name);
        str_copy(status, "The change takes effect the next time CiukiOS starts.");
        break;
    case C_REMOVE:
        if (sel_drv < 0 || sel_drv >= ndrv) break;
        str_copy(msg, "Remove the driver "); str_cat(msg, drvs[sel_drv].name);
        str_cat(msg, " and delete its files?");
        msgbox(&dlg, "Remove Driver", msg, "Yes|No");
        dlg_kind = D_REMOVE;
        break;
    case C_CLOSE: app_close(); break;
    case C_TAB_DEV: tab = 0; break;
    case C_TAB_DRV: tab = 1; load_drivers(); break;
    case C_ABOUT: message("About Devices", "CiukiOS Device Manager and drivers.\nDrivers are DOS programs in a package\nwith DRIVER.INF, loaded at startup."); break;
    }
}
static void dialog_result(int r)
{
    int kind = dlg_kind;
    if (r < 0) return;
    dlg_kind = D_NONE;
    if (kind == D_ADD && r == 1) add_check();
    if (kind == D_CONFIRM_ADD && r == MB_YES) add_install();
    if (kind == D_REMOVE && r == MB_YES && sel_drv >= 0 && sel_drv < ndrv) {
        char p[SYS_PATH];
        int i;
        driver_dir(&drvs[sel_drv], p);
        tree_delete(p);
        app_log("[DEVICES] driver removed", drvs[sel_drv].name);
        for (i = sel_drv; i + 1 < ndrv; i++) mem_copy(&drvs[i], &drvs[i + 1], sizeof drvs[0]);
        ndrv--;
        save_drivers();
        load_drivers();
        if (sel_drv >= ndrv) sel_drv = ndrv - 1;
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
static const char *tab_names[2] = { "Devices by Type", "Installed Drivers" };
static int tab_x(int i, int *w)
{
    int x = X0 + 8, k;
    for (k = 0; k < i; k++) x += ui_measure(tab_names[k]) + 38;
    *w = ui_measure(tab_names[i]) + 34;                 /* room for the bold label */
    return x;
}
struct btn { const char *label; int w, cmd; };
static const struct btn dev_btns[] = { { "P&roperties", 100, C_PROPS }, { "&Scan for Changes", 140, C_SCAN } };
static const struct btn drv_btns[] = { { "&Add New Driver...", 140, C_ADD }, { "&Enable", 80, C_ENABLE },
                                       { "&Disable", 80, C_DISABLE }, { "&Remove", 80, C_REMOVE }, { "P&roperties", 100, C_PROPS } };
static int btn_enabled(int cmd)
{
    int d = sel_drv >= 0 && sel_drv < ndrv;
    if (tab == 0) return cmd != C_PROPS || (sel_row >= 0 && sel_row < nrows);
    if (cmd == C_ENABLE) return d && !drvs[sel_drv].enabled;
    if (cmd == C_DISABLE) return d && drvs[sel_drv].enabled;
    if (cmd == C_REMOVE || cmd == C_PROPS) return d;
    return 1;
}
static int btn_at(int sx, int sy)
{
    int x = lx, i, n = tab ? 5 : 2;
    const struct btn *b = tab ? drv_btns : dev_btns;
    if (sy < btn_y || sy >= btn_y + 26) return -1;
    for (i = 0; i < n; i++) { if (sx >= x && sx < x + b[i].w) return i; x += b[i].w + 6; }
    return -1;
}
static void paint(void)
{
    int i, w, x, y;
    char t[80];
    layout();
    update_menus();
    bar.x = X0; bar.y = Y0; bar.w = W;
    for (i = 0; i < 2; i++) {
        x = tab_x(i, &w);
        if (i == tab) { ui_bevel(x, tab_y, w, 26, C_FACE); ui_rect(x + 2, tab_y + 23, w - 4, 4, C_FACE); }
        else ui_bevel(x, tab_y + 3, w, 23, C_FACE);
        ui_text(x + 12, tab_y + (i == tab ? 4 : 6), tab_names[i], i == tab ? C_INK | BOLD : C_INK);
    }
    ui_inset(lx, ly, lw + 16, lh);
    ui_rect(lx + 2, ly + 2, lw + 12, lh - 4, C_PAPER);
    if (tab == 0) {
        int vr = visible_rows();
        if (sel_row >= nrows) sel_row = nrows - 1;
        if (sel_row >= 0 && sel_row < top_row) top_row = sel_row;
        if (sel_row >= top_row + vr) top_row = sel_row - vr + 1;
        for (i = top_row; i < nrows && i < top_row + vr; i++) {
            int fg = C_INK, r = rows[i];
            y = ly + 2 + (i - top_row) * ROW_H;
            if (r < 0) {
                int k = -1 - r;
                x = lx + 6;
                ui_bevel(x, y + 4, 11, 11, C_PAPER);
                ui_rect(x + 3, y + 9, 5, 1, C_INK);
                if (!open_class[k]) ui_rect(x + 5, y + 7, 1, 5, C_INK);
                str_copy(t, class_names[k]);
                if (i == sel_row) { ui_rect(x + 16, y, ui_measure(t) + 8, ROW_H, C_TITLE); fg = C_PAPER; }
                ui_text(x + 20, y + 1, t, fg | BOLD);
            } else {
                x = lx + 30;
                ui_rect(x, y + 5, 12, 9, C_SHADOW);
                ui_rect(x + 1, y + 6, 10, 5, C_LIGHT);
                if (i == sel_row) { ui_rect(x + 16, y, ui_measure(devs[r].name) + 8, ROW_H, C_TITLE); fg = C_PAPER; }
                ui_text(x + 20, y + 1, devs[r].name, fg);
            }
        }
        draw_scroll(lx + lw, ly + 2, lh - 4, top_row, nrows, vr);
    } else {
        int vr = visible_rows(), c1 = lx + 2, c2 = lx + (int)((long)lw * 30 / 100), c3 = lx + (int)((long)lw * 46 / 100),
            c4 = lx + (int)((long)lw * 64 / 100), c5 = lx + lw;
        static const char *heads[] = { "Name", "Class", "Status", "Command" };
        int cx[5];
        cx[0] = c1; cx[1] = c2; cx[2] = c3; cx[3] = c4; cx[4] = c5;
        for (i = 0; i < 4; i++) { ui_bevel(cx[i], ly + 2, cx[i + 1] - cx[i], 20, C_FACE); ui_text(cx[i] + 6, ly + 4, heads[i], C_INK); }
        if (sel_drv >= ndrv) sel_drv = ndrv - 1;
        if (sel_drv >= 0 && sel_drv < top_drv) top_drv = sel_drv;
        if (sel_drv >= top_drv + vr) top_drv = sel_drv - vr + 1;
        for (i = top_drv; i < ndrv && i < top_drv + vr; i++) {
            int fg = C_INK;
            y = ly + 24 + (i - top_drv) * ROW_H;
            if (i == sel_drv) { ui_rect(c1 + 2, y, c5 - c1 - 4, ROW_H, C_TITLE); fg = C_PAPER; }
            ui_text(c1 + 8, y + 1, drvs[i].name, fg);
            ui_text(c2 + 6, y + 1, drvs[i].cls, fg);
            ui_text(c3 + 6, y + 1, drvs[i].status, fg);
            draw_frame_text(c4 + 6, y + 1, c5 - c4 - 10, drvs[i].cmd, fg);
        }
        if (!ndrv) ui_text(lx + 16, ly + 32, "No drivers are installed. Add New Driver installs a driver package.", C_SHADOW);
        draw_scroll(lx + lw, ly + 2, lh - 4, top_drv, ndrv, vr);
    }
    x = lx;
    {
        int n = tab ? 5 : 2;
        const struct btn *b = tab ? drv_btns : dev_btns;
        for (i = 0; i < n; i++) {
            int on = btn_enabled(b[i].cmd);
            char plain[32];
            int k = 0;
            const char *s = b[i].label;
            while (*s) { if (*s != '&') plain[k++] = *s; s++; }
            plain[k] = 0;
            ui_bevel(x, btn_y, b[i].w, 26, i == hot_btn && on ? C_LIGHT : C_FACE);
            ui_text(x + (b[i].w - ui_measure(plain)) / 2, btn_y + 5, plain, on ? C_INK : C_SHADOW);
            x += b[i].w + 6;
        }
    }
    y = Y0 + H - 20;
    ui_rect(X0, y - 1, W, 21, C_FACE);
    ui_inset(X0 + 2, y, W - 4, 19);
    if (status[0]) str_copy(t, status);
    else if (tab == 0) { fmt_u32(t, (u32)ndev); str_cat(t, " device(s)"); }
    else { fmt_u32(t, (u32)ndrv); str_cat(t, " installed driver(s)"); }
    ui_text(X0 + 8, y + 1, t, C_INK);
    menubar_draw(&bar);
    if (ctx.open) popup_draw(&ctx);
}
static void paint_dialog(struct dialog *d, int icon)
{
    dialog_draw(d);
    ui_icon(d->x + 6, d->y + DIALOG_TITLE_H + 2, icon);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
static int on_mouse(int kind, int sx, int sy)
{
    int r, i;
    layout();
    if (dlg.open) { r = dialog_mouse(&dlg, kind, sx, sy); if (r >= 0) dialog_result(r); return 1; }
    if (ctx.open) { r = popup_mouse(&ctx, kind, sx, sy); if (r >= 0) command(r); return 1; }
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (kind == MOUSE_DOWN && sy >= tab_y && sy < tab_y + 26) {
        int w;
        for (i = 0; i < 2; i++) { int x = tab_x(i, &w); if (sx >= x && sx < x + w) { command(i ? C_TAB_DRV : C_TAB_DEV); return 1; } }
    }
    if (kind == MOUSE_DOWN) {
        i = btn_at(sx, sy);
        if (i >= 0) {
            int cmd = tab ? drv_btns[i].cmd : dev_btns[i].cmd;
            if (btn_enabled(cmd)) command(cmd);
            return 1;
        }
    }
    if (sx >= lx + lw && sx < lx + lw + 16 && sy >= ly && sy < ly + lh && kind == MOUSE_DOWN) {
        int total = tab ? ndrv : nrows, *top = tab ? &top_drv : &top_row;
        long p = scroll_hit(lx + lw, ly + 2, lh - 4, sy, total, visible_rows());
        if (p == -1 && *top > 0) (*top)--;
        else if (p == -2 && *top + visible_rows() < total) (*top)++;
        else if (p >= 16) *top = (int)(p - 16);
        return 1;
    }
    if (sx >= lx && sx < lx + lw && sy >= ly && sy < ly + lh && (kind == MOUSE_DOWN || kind == MOUSE_RIGHT)) {
        int row = tab ? top_drv + (sy - ly - 24) / ROW_H : top_row + (sy - ly - 2) / ROW_H;
        int total = tab ? ndrv : nrows;
        if (tab && sy < ly + 24) return 1;
        if (row >= total) row = -1;
        status[0] = 0;
        if (kind == MOUSE_DOWN) {
            int dbl = row >= 0 && row == last_i && (unsigned)(HOST.ticks - last_click) < (unsigned)HOST.dblclick;
            last_i = row; last_click = HOST.ticks;
            if (tab) sel_drv = row; else sel_row = row;
            if (!tab && row >= 0 && rows[row] < 0 && sx < lx + 20) { command(C_PROPS); return 1; }
            if (dbl) { last_i = -1; command(C_PROPS); }
            return 1;
        }
        if (tab) sel_drv = row; else sel_row = row;
        update_menus();
        if (tab) popup_open(&ctx, m_ctx_drv, 7, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        else popup_open(&ctx, m_ctx_dev, 3, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        return 1;
    }
    return kind == MOUSE_DOWN;
}
static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), r;
    layout();
    if (dlg.open) { r = dialog_key(&dlg, key, HOST.shift); if (r >= 0) dialog_result(r); return 1; }
    if (ctx.open) { r = popup_key(&ctx, key); if (r >= 0) command(r); return 1; }
    r = menubar_key(&bar, key, HOST.shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (s == 0x0F) { tab = !tab; if (tab) load_drivers(); return 1; }
    if (s == 0xA6 || (s == 0x1C && (HOST.shift & SH_ALT)) || ch == 13) { command(C_PROPS); return 1; }
    if (!ch || ch == 0xE0) {
        int *sel = tab ? &sel_drv : &sel_row, total = tab ? ndrv : nrows;
        switch (s) {
        case K_UP: if (*sel > 0) (*sel)--; return 1;
        case K_DOWN: if (*sel + 1 < total) (*sel)++; return 1;
        case K_HOME: *sel = 0; return 1;
        case K_END: *sel = total - 1; return 1;
        case K_F5: command(C_SCAN); return 1;
        case K_DEL: if (tab) command(C_REMOVE); return 1;
        case K_LEFT: case K_RIGHT:
            if (!tab && *sel >= 0 && *sel < nrows && rows[*sel] < 0) {
                open_class[-1 - rows[*sel]] = s == K_RIGHT;
                build_rows();
            }
            return 1;
        }
    }
    if (key == 0x5D00) {
        update_menus();
        if (tab) popup_open(&ctx, m_ctx_drv, 7, lx + 40, ly + 40, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        else popup_open(&ctx, m_ctx_dev, 3, lx + 40, ly + 40, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        return 1;
    }
    return 0;
}
static int props_event(int ev, int a, int b, int c)
{
    int r = -1;
    if (ev == EV_PAINT) { paint_dialog(&pdlg, prop_icon); return 0; }
    if (ev == EV_CLOSE) { pdlg.win = 0; pdlg.open = 0; return 0; }
    if (ev == EV_KEY) r = dialog_key(&pdlg, a, HOST.shift);
    if (ev == EV_MOUSE) {
        if (a == MOUSE_HOVER) { ui_dirty = 0; dialog_hover(&pdlg, HOST.x + b, HOST.y + TITLE_H + c); return ui_dirty; }
        r = dialog_mouse(&pdlg, a, HOST.x + b, HOST.y + TITLE_H + c);
    }
    if (r == 10) add_dialog(0);
    dialog_sync(&pdlg);
    dialog_sync(&dlg);
    return 1;
}
static char pending_install[SYS_PATH];       /* after the main window shows */
static int dev_event(int ev, int a, int b, int c)
{
    switch (ev) {
    case EV_OPEN: {
        char arg[APP_ARG_BYTES];
        str_copy(app_title, "Device Manager");
        str_ncopy(arg, APP_ARG, APP_ARG_BYTES);
        APP_ARG[0] = 0;
        if (!HDR_WIDTH) {
            HDR_WIDTH = HOST.screen_w - 80 > 620 ? 620 : HOST.screen_w - 80;
            HDR_HEIGHT = HOST.screen_h - 120 > 460 ? 460 : HOST.screen_h - 120;
            scan_devices();
            build_rows();
            load_drivers();
            sel_row = 0; sel_drv = 0;
            app_log("[DEVICES] devices", 0);
        }
        if (!str_icmp(arg, "network")) {
            int row;
            scan_devices(); build_rows();
            tab = 0; sel_row = 0; top_row = 0;
            for (row = 0; row < nrows; row++) {
                if (rows[row] >= 0 && devs[rows[row]].kind == K_NET) {
                    sel_row = row; top_row = row > 2 ? row - 2 : 0; break;
                }
            }
        }
        if (!str_icmp(arg, "drivers")) {
            int i;
            tab = 1; load_drivers();
            for (i = 0; i < ndrv; i++) if (!str_icmp(drvs[i].cls, "NET")) {
                sel_drv = i; top_drv = i > 2 ? i - 2 : 0; break;
            }
        }
        if (!str_nicmp(arg, "install:", 8)) { tab = 1; str_ncopy(pending_install, arg + 8, SYS_PATH); }
        {
            char t[8];
            fmt_u32(t, (u32)ndev);
            app_log(tab ? "[DEVICES] tab drivers" : "[DEVICES] tab devices", t);
        }
        return 1;
    }
    case EV_PAINT: paint(); return 0;
    case EV_KEY: return on_key(a);
    case EV_MOUSE: return on_mouse(a, HOST.x + b, HOST.y + TITLE_H + c);
    case EV_POLL:
        if (pending_install[0] && !dlg.open) {
            add_dialog(pending_install);
            pending_install[0] = 0;
            return 1;
        }
        return 0;
    }
    return 0;
}
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_OPEN && a == 2) { dlg.win = 0; pdlg.win = 0; pdlg.open = 0; dialog_sync(&dlg); return 1; }
    if (pdlg.win && HOST.window == pdlg.win) return props_event(ev, a, b, c);
    r = dialog_pre(&dlg, WIN_DEVICES, &ev, &a);
    if (r >= 0) return r;
    if (dialog_mine(&dlg) && ev == EV_PAINT) { paint_dialog(&dlg, dlg_kind == D_ADD || dlg_kind == D_CONFIRM_ADD ? ICON_PACKAGE : ICON_DEVICES); return 0; }
    if (ev == EV_MOUSE && a == MOUSE_HOVER) {
        int sx = HOST.x + b, sy = HOST.y + TITLE_H + c, i;
        layout();
        ui_dirty = 0;
        if (dialog_mine(&dlg)) dialog_hover(&dlg, sx, sy);
        else if (dlg.open) return 0;
        else if (ctx.open) popup_mouse(&ctx, MOUSE_HOVER, sx, sy);
        else if (menubar_open(&bar)) menubar_mouse(&bar, MOUSE_HOVER, sx, sy);
        else { i = btn_at(sx, sy); if (i != hot_btn) { hot_btn = i; ui_dirty = 1; } }
        return ui_dirty;
    }
    r = dev_event(ev, a, b, c);
    r = dialog_post(&dlg, WIN_DEVICES, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
