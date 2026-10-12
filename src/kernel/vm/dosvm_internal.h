/* Private implementation shared by the three F3-00 components.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_INTERNAL_H
#define CIUKI_DOSVM_INTERNAL_H
#include <ciuki/dosvm.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/sync.h>
#include "dosvm_devices.h"
#include "virtual_vga_bios.h"
#include "dosvm_audio_state.h"
#define DOSVM_PAGES 272u
#define DOSVM_SECTORS 262144u
#define DOSVM_OVERLAY_SECTORS 1024u
/* A base image may stop before the geometry's end (owner decision
 * 2026-10-12, the bundled shareware games disk): trailing all-zero sectors
 * are cut by scripts/build_games_disk.py and read back as zeros. It must
 * still hold the boot sector, loader area, both FATs and the root directory
 * of the private FAT16 geometry (scripts/build_dos_image.py DATA_LBA). */
#define DOSVM_BASE_MIN_SECTORS 361u
#define DOSVM_PORTS 16u
/* f3-02 presenter: fixed 640x480 XRGB window surface, integer scale only.
 * One presented frame per 33 host ticks. Input status 1 follows the guest's
 * own VGA timing instead (dosvm_vga_status). */
#define DOSVM_VIDEO_WIDTH 640u
#define DOSVM_VIDEO_HEIGHT 480u
#define DOSVM_FRAME_TICKS 33u
#define DOSVM_CURSOR_TICKS 250u
#define DOSVM_BLINK_TICKS 500u
#include "dosvm_memory.h"
#include "dpmi_internal.h"
enum dosvm_trace_kind { DOSVM_TRACE_VECTOR, DOSVM_TRACE_PIT, DOSVM_TRACE_IRQ0,
    DOSVM_TRACE_DB_PM, DOSVM_TRACE_DB_RM, DOSVM_TRACE_FLAGS, DOSVM_TRACE_TIMER,
    DOSVM_TRACE_FAULT, DOSVM_TRACE_COUNT };
struct dosvm {
    struct dosvm_info info;
    struct aspace as;
    uint32_t frames[DOSVM_PAGES];
    struct task *task;
    struct v86 monitor;
    struct v86_frame frame;  /* working copy of the trapped frame (f3-04) */
    bool frame_pm;           /* frame is a ring-3 DPMI client trap_frame */
    bool dpmi_enabled;       /* INT 2Fh/1687h answered for this VM */
    struct vm_memory memory;
    struct dosvm_pit pit;
    struct dosvm_kbd kbd;
    struct dosvm_mouse mouse;
    struct dosvm_audio audio; /* f3-05: SB16 DSP/mixer, OPL3, 8237 and F4 queues */
    bool window_input;
    int32_t input_x, input_y;
    bool pointer_known;
    uint32_t continuation;
    struct kwait wake;
    /* Private-disk gate for the sector index and each VFS seek/transfer
     * pair. A kwait gate, not a kmutex: the VFS and ATA mutexes it nests
     * have lower addresses than this slot, which the address-ordered mutex
     * rule forbids; nobody takes this gate while holding those mutexes. */
    bool disk_busy; struct kwait disk_wait;
    uint64_t boot_deadline, input_since, ticks, timer_cycles, clock_ms, record_window;
    uint32_t bios_ticks, record_lines;
    bool started, a20, table_ready, base_open, overlay_open, overlay_created;
    uint8_t midnight, disk_status;
    const char *probe;
    struct vfs_table disk_table;
    int base, overlay;
    uint32_t base_sectors;   /* sectors the base image holds; zeros after them */
    char overlay_path[48];
    struct { uint32_t lba, offset; } sectors[DOSVM_OVERLAY_SECTORS];
    unsigned sector_count;
    uint8_t sector[512];
    uint16_t text[80 * 25], keyboard[32];
    unsigned cursor, key_head, key_count;
    /* f3-02 virtual VGA and window presenter (dosvm_vga.c, dosvm_client.c). */
    cvga_state vga __attribute__((aligned(4096))); /* planes page-aligned: f3-06 direct window */
    cvbios video_bios;
    cvx_bus video_bus;
    bool video_bus_fault, video_presented;
    uint8_t video_hsync;
    unsigned video_blank;      /* read index in the vertical blank, 0 outside */
    uint64_t video_vblank_us;  /* host time (us) the next vertical blank is due */
    uint64_t video_tick;
    uint32_t video_display, video_phase, owner_pid;
    uint32_t video_faults, video_frames;
    /* f3-06/07: direct window (0 none, 1..4 plane + 1, 5 linear). A0000h-
     * AFFFFh maps a model plane or the VM's own linear backing user-writable;
     * sync closes it before the VM's low-page restoration (dosvm_vga.c). */
    unsigned video_direct;
    uint32_t video_pm_faults, video_starts, video_maps, video_flushes, video_burst;
    uint32_t video_linear_maps, video_linear_harvests;
    cvga_geometry video_geometry;
    uint8_t video_taken[4][CVGA_DIRTY_BYTES];
    uint8_t video_row[DOSVM_VIDEO_WIDTH];
    uint32_t video_pixels[DOSVM_VIDEO_WIDTH];
    char name[32];
    char line[81];
    char verdict_name[12];
    /* f3-06: the text screen latched when the verdict line arrives, before
     * the shell runs again (the CiukiOS shell redraws its row-0 title bar
     * before its prompt, over a child's last output line). */
    uint16_t verdict_text[80 * 25];
    bool verdict_text_valid;
    unsigned line_length;
    bool line_bad;
    struct {
        uint16_t first, last;
        dosvm_port_handler handler;
        void *arg;
    } ports[DOSVM_PORTS];
    struct dpmi dpmi;        /* f3-04 kernel DPMI host, idle until the first entry */
    /* f3-09: fixed lifetime budgets, with at most four IRQ0 hook epochs. */
    struct {
        uint8_t used[DOSVM_TRACE_COUNT], irq_epochs;
        bool timer_programmed;
        uint64_t timer_tick;
    } trace;
};
void dosvm_trace(struct dosvm *, enum dosvm_trace_kind, const struct v86_frame *,
                 uint32_t a, uint32_t b, uint32_t c);
struct dosvm *dosvm_lookup(struct dosvm_key);
int dosvm_vga_init(struct dosvm *);
int dosvm_vga_bios(struct dosvm *, struct v86_frame *);
int dosvm_vga_fault(struct dosvm *, struct v86_frame *);
void dosvm_vga_burst(struct dosvm *, struct v86_frame *);
/* f3-06: a ring-3 DPMI client's access to the supervisor-only window. */
int dosvm_vga_fault_pm(struct dosvm *, struct v86_frame *);
/* f3-06: open, move or close the direct plane window for the current VGA
 * state; sync harvests stored pages for the presenter (and closes the
 * window, restoring the VM's own frames, if unmap). */
void dosvm_vga_direct(struct dosvm *);
void dosvm_vga_sync(struct dosvm *, bool unmap);
/* Close only the linear window before a consumer uses/modifies the planes. */
void dosvm_vga_model(struct dosvm *);
/* Eligible protection fault: open the linear window and retry the CPU cycle. */
bool dosvm_vga_retry(struct dosvm *, uint32_t address, uint32_t instruction, bool pm);
/* Writes changed rows of the 640x480 frame through row(arg, y, pixels, bytes).
 * Returns 1 with *damage set when rows changed, 0 when nothing changed or the
 * frame period has not elapsed, or the negative row-writer error. */
struct ciuki_rect;
int dosvm_vga_present(struct dosvm *, int (*row)(void *, unsigned, const void *, unsigned),
                      void *, uint64_t tick, struct ciuki_rect *damage);
uint8_t dosvm_vga_status(struct dosvm *, uint64_t tick);
int dosvm_disk_open(struct dosvm *, const char *base);
int dosvm_disk_close(struct dosvm *, bool forced);
int dosvm_bios_init(struct dosvm *);
int dosvm_io(void *, uint16_t, unsigned, bool, uint32_t *);
void dosvm_putchar(struct dosvm *, uint8_t, uint8_t);
int dosvm_wait(struct dosvm *, bool keyboard, uint64_t deadline);
void dosvm_input_poll(struct dosvm *);
int dosvm_keyboard_refresh(struct dosvm *);
static inline const char *dosvm_probe(const struct dosvm *v)
{ return v->probe ? v->probe : "vm-boot"; }
/* Low page protection: the VGA window A0000h-BFFFFh is supervisor-only so
 * every guest access faults into the f3-02 model; ROM/stub pages read-only;
 * DPMI real-mode host stack pages (C6000h-C7FFFh) writable. Shared by
 * creation and EMS teardown. */
static inline uint32_t dosvm_low_flags(unsigned page)
{
    if (page >= 0xA0 && page < 0xC0) return PTE_W; /* guest #PF -> model */
    if (page >= DPMI_RSTACK_PAGE && page < DPMI_RSTACK_PAGE + DPMI_RSTACK_PAGES) return PTE_U | PTE_W;
    if ((page >= 0xC0 && page < 0xC9) || (page >= 0xF0 && page < 0x100)) return PTE_U;
    return PTE_U | PTE_W;
}
/* Shared DPMI helpers (one translation unit in the host harness). */
static inline unsigned dpmi_width(const struct dosvm *v) { return v->dpmi.bits32 ? 4 : 2; }
static inline uint8_t dpmi_id(const struct dosvm *v) { return (uint8_t)v->dpmi.clients; }
static inline void dpmi_gprs(struct v86_frame *dst, const struct v86_frame *src)
{
    dst->tf.eax = src->tf.eax; dst->tf.ebx = src->tf.ebx; dst->tf.ecx = src->tf.ecx;
    dst->tf.edx = src->tf.edx; dst->tf.esi = src->tf.esi; dst->tf.edi = src->tf.edi;
    dst->tf.ebp = src->tf.ebp;
}
/* dpmi.c entry points used by the monitor */
int dpmi_v86_int(struct dosvm *, struct v86_frame *, uint8_t vector);
int dpmi_v86_stub(struct dosvm *, struct v86_frame *);
int dpmi_pm_trap(struct dosvm *, struct v86_frame *, uint32_t cr2, uint32_t dr6);
int dpmi_deliver(struct dosvm *, struct v86_frame *);
void dpmi_close(struct dosvm *);
void dpmi_watch_hits(struct dosvm *, uint32_t dr6);
uint32_t dpmi_debug_status(const struct dosvm *, uint32_t dr6);
uint32_t dpmi_fault_linear(struct dosvm *, const struct v86_frame *);
int dpmi_rom_init(struct dosvm *);
void dpmi_info_fill(struct dosvm *, struct dpmi_info *);
#endif
