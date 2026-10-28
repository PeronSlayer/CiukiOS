/* SPDX-License-Identifier: MIT
 * iconx: the bounded icon extraction core shared by the native shell
 * (libdeskui fileicon.c) and the Win32 USER/SHELL facade (sdk/win32/user32).
 *
 * Freestanding: no libc, no heap, integer arithmetic only, 32-bit
 * intermediates. Every byte of the subject file is read through the
 * caller's positional reader, through a small read cache, within a read
 * budget; every table walk has a fixed entry limit. A malformed or hostile
 * file fails with a negative code and the caller shows the type's default
 * icon: nothing here loops on file data or trusts an offset.
 *
 * Formats (sources in docs/design/file-icons.md):
 *  - ICO/CUR: ICONDIR + ICONDIRENTRY (J. Hornick, "Icons", MSDN 1995);
 *  - PE: .rsrc tree (Microsoft PE format, section 6.9), RT_GROUP_ICON (14)
 *    GRPICONDIR + GRPICONDIRENTRY.nID -> RT_ICON (3) images;
 *  - NE: resource table (MS-DOS Encyclopedia, Appendix K), types 0x800E/0x8003;
 *  - ANI: RIFF 'ACON', 'anih' (AF_ICON), first 'icon' chunk of LIST 'fram';
 *  - BMP: BITMAPFILEHEADER + DIB, only as an explicit icon location;
 *  - images: BITMAPINFOHEADER/BITMAPCOREHEADER DIBs with 1/4/8/16/24/32 bpp,
 *    XOR image then AND mask; 32 bpp alpha when any alpha byte is set (the
 *    Windows XP rule), the AND mask otherwise. PNG images (Windows Vista)
 *    are skipped, as Windows 98 cannot show them.
 *
 * Group choice (ExtractIcon/ExtractIconEx semantics): index >= 0 counts
 * RT_GROUP_ICON resources in resource order (PE: named entries first, then
 * IDs ascending; NE: table order), index < 0 names the group whose resource
 * ID is -index. Image choice (Hornick, "Choosing an Icon"; Wine
 * CURSORICON_FindBestIcon): the image closest in size, a larger one on a tie,
 * then the greatest colour depth (CiukiOS draws in 32 bpp), then directory
 * order; an image that does not decode yields to the next candidate. The
 * chosen image is resampled to the requested square size with the integer
 * area filter of libdeskui icons.c (alpha-weighted, rounded half up).
 *
 * Output pixels are straight-alpha AARRGGBB, zero where transparent. An AND
 * mask pixel over a non-black XOR colour (screen inversion, cursors) is
 * shown opaque as the inverse colour, its look over a white background. */
#ifndef CIUKI_ICONX_H
#define CIUKI_ICONX_H
#include <stdint.h>

#define ICONX_SIZE_MAX 256u        /* largest source and destination edge */
#define ICONX_READ_BUDGET (4u << 20) /* bytes one iconx object may read */
#define ICONX_GROUP_MAX 512u       /* images in one icon directory */
#define ICONX_DIR_MAX 4096u        /* entries in one PE resource directory */
#define ICONX_TRIES 8u             /* candidate images tried per extraction */
#define ICONX_CACHE 3u
#define ICONX_CACHE_BYTES 2048u

enum iconx_kind { ICONX_UNKNOWN, ICONX_ICO, ICONX_CUR, ICONX_ANI, ICONX_BMP, ICONX_PE, ICONX_NE, ICONX_DOS };
enum {
    ICONX_OK = 0,
    ICONX_EIO = -1,        /* the reader failed, or the read budget is spent */
    ICONX_EFORMAT = -2,    /* malformed or unsupported data */
    ICONX_ENONE = -3,      /* a valid file without icons */
    ICONX_EINDEX = -4,     /* no group at that index or resource ID */
    ICONX_ELIMIT = -5,     /* a table exceeds a fixed limit */
    ICONX_EARG = -6
};

/* Reads exactly `bytes` at `offset` (0) or fails (nonzero). */
typedef int (*iconx_read_fn)(void *ctx, uint32_t offset, void *buffer, uint32_t bytes);

struct iconx_image { unsigned width, height, depth, index; uint32_t id; };

struct iconx {
    iconx_read_fn read; void *ctx;
    uint32_t size;                  /* bytes of the whole source */
    uint32_t base, limit;           /* the window [base, base+limit) being parsed */
    uint32_t budget, loaded, calls; /* read accounting */
    struct { uint32_t at, len, age; uint8_t bytes[ICONX_CACHE_BYTES]; } cache[ICONX_CACHE];
    uint32_t clock;
    int kind;
    uint32_t group_id;              /* resource ID of the extracted group, 0 when named or a file */
    struct iconx_image chosen;      /* the image the last extraction used */
    uint32_t palette[256];
    uint32_t row[ICONX_SIZE_MAX];   /* one decoded source row */
    uint8_t raw[ICONX_SIZE_MAX * 4];
    uint8_t mask[ICONX_SIZE_MAX / 8];
    uint32_t sums[ICONX_SIZE_MAX][4];
};

void iconx_init(struct iconx *, iconx_read_fn, void *ctx, uint32_t size);
/* Memory source helper: ctx is a struct iconx_memory. */
struct iconx_memory { const uint8_t *bytes; uint32_t size; };
int iconx_memory_read(void *ctx, uint32_t offset, void *buffer, uint32_t bytes);

int iconx_kind(struct iconx *);
/* ExtractIcon(-1): icon groups of a PE or NE file, 1 for ICO/CUR/ANI/BMP,
 * 0 for anything else (including a DOS program); negative on a damaged file. */
int iconx_count(struct iconx *);
/* One icon at size x size (1..ICONX_SIZE_MAX) into argb. */
int iconx_extract(struct iconx *, int index, unsigned size, uint32_t *argb);
/* One RT_ICON/ICO image (a DIB at [offset, offset+bytes) of the source). */
int iconx_decode(struct iconx *, uint32_t offset, uint32_t bytes, unsigned size, uint32_t *argb);
/* Rank the images of an in-memory icon directory (GRPICONDIR when entry ==
 * 14, ICONDIR when entry == 16) for a size: order[] receives up to max
 * entry indices, best first. Returns the count, or a negative error. */
int iconx_rank(const uint8_t *dir, uint32_t bytes, unsigned entry, unsigned size, unsigned *order, unsigned max);

/* ---- icon locations ----------------------------------------------------
 * Pure parsers over bytes the caller read; every output string is
 * NUL-terminated and bounded by its capacity. They never touch files. */
#define ICONX_PATH 260u
struct iconx_link {
    char target[ICONX_PATH], icon[ICONX_PATH], arguments[ICONX_PATH];
    int32_t icon_index;
    int ciuki;                      /* the CiukiOS CIUKLNK3 format */
};
/* A CiukiOS CIUKLNK3 or MS-SHLLINK .lnk: target (LocalBasePath[+suffix] or
 * the relative path), ICON_LOCATION or the IconEnvironmentDataBlock and the
 * header's IconIndex. 0 on success. */
int iconx_link_parse(const uint8_t *bytes, uint32_t n, struct iconx_link *out);
/* A Windows 95/98 PIF: the "WINDOWS VMM 4.0" section's icon file and
 * number. 1 when the PIF names an icon other than PIFMGR.DLL's (the MS-DOS
 * default), 0 when it uses the default or has no such section, <0 when the
 * PIF is malformed. program receives the basic section's program (may be
 * empty). */
int iconx_pif_parse(const uint8_t *bytes, uint32_t n, char *icon, unsigned cap, int32_t *index,
                    char *program, unsigned program_cap);
/* An INI value (desktop.ini, autorun.inf): case-insensitive section and key,
 * ANSI/UTF-8 or UTF-16LE with a BOM (converted to UTF-8), ';' comments,
 * surrounding blanks and one pair of double quotes removed. 1 if found. */
int iconx_ini_get(const uint8_t *bytes, uint32_t n, const char *section, const char *key, char *out, unsigned cap);
/* "file[,index]" (autorun.inf icon=, desktop.ini IconResource=): the
 * trailing ",<integer>" is split off. */
void iconx_location_split(char *location, int32_t *index);
/* A folder's desktop.ini icon: IconFile + IconIndex, else IconResource. */
int iconx_desktop_ini(const uint8_t *bytes, uint32_t n, char *icon, unsigned cap, int32_t *index);
/* An AutoRun medium's [autorun] icon=. */
int iconx_autorun_inf(const uint8_t *bytes, uint32_t n, char *icon, unsigned cap, int32_t *index);
#endif
