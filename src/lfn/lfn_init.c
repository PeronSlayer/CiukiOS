/* LFN.COM, install part (segment ITEXT, dropped when resident): the
 * volume's layout from its boot sector, read through the kernel. */
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;
struct kio { u32 lba; u16 off, seg, write; };
int kio(struct kio *q);
extern u16 g_spc, g_shift, g_fat, g_fatsz, g_nfats, g_rootn, g_maxcl;
extern u32 g_root, g_data, g_serial;
extern u8 dbuf[512];
static u16 ds_(void);
#pragma aux ds_ = "mov ax,ds" value [ax];

static u16 w(u16 o) { return dbuf[o] | ((u16)dbuf[o + 1] << 8); }

/* 0: a FAT16 volume the extension can serve. */
int lfn_init(void)
{
    struct kio q;
    u32 total, clusters;
    u16 res;
    q.lba = 0; q.off = (u16)dbuf; q.seg = ds_(); q.write = 0;
    if (kio(&q)) return 1;
    if (w(11) != 512) return 2;
    g_spc = dbuf[13];
    for (g_shift = 0; g_shift < 8 && (1u << g_shift) != g_spc; g_shift++) ;
    if (g_shift == 8) return 3;
    res = w(14);
    g_nfats = dbuf[16];
    g_rootn = w(17);
    g_fatsz = w(22);
    total = w(19) ? w(19) : ((u32)w(32) | ((u32)w(34) << 16));
    if (!res || !g_nfats || g_nfats > 2 || !g_fatsz || !g_rootn || (g_rootn & 15)) return 4;
    g_fat = res;
    g_root = (u32)res + (u32)g_nfats * g_fatsz;
    g_data = g_root + (g_rootn >> 4);
    if (total <= g_data) return 5;
    clusters = (total - g_data) >> g_shift;
    if (clusters < 4085 || clusters > 65524L) return 6;
    if (clusters > (u32)g_fatsz * 256 - 2) clusters = (u32)g_fatsz * 256 - 2;
    g_maxcl = (u16)clusters + 1;
    g_serial = dbuf[38] == 0x29 ? ((u32)w(39) | ((u32)w(41) << 16)) : 0;
    return 0;
}
