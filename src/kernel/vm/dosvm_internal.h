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
#include <ciuki/dos_video_modes.h>
int dosvm_game_remove_reserve(unsigned game);
void dosvm_game_remove_release(unsigned game);
int dosvm_game_program(unsigned game, bool remove, uint32_t *size);
#define DOSVM_PAGES 272u
#define DOSVM_SECTORS 262144u
#define DOSVM_OVERLAY_SECTORS 1024u
void dosvm_client_priority(struct dosvm *);
/* A base image may stop before the geometry's end (owner decision
 * 2026-10-12, the bundled shareware games disk): trailing all-zero sectors
 * are cut by scripts/build_games_disk.py and read back as zeros. It must
 * still hold the boot sector, loader area, both FATs and the root directory
 * of the private FAT16 geometry (scripts/build_dos_image.py DATA_LBA). */
#define DOSVM_BASE_MIN_SECTORS 361u
#define DOSVM_PORTS 16u
/* f3-02 presenter: fixed 640x480 XRGB window surface, integer scale only.
 * Graphics follows completion writes or the guest's programmed refresh;
 * unchanged pages do not trigger harvest. */
#define DOSVM_VIDEO_WIDTH 640u
#define DOSVM_VIDEO_HEIGHT 480u
#define DOSVM_VIDEO_BAND 8u
#define DOSVM_CURSOR_TICKS 250u
#define DOSVM_BLINK_TICKS 500u
#define DOSVM_HEARTBEAT_TICKS 2000u
#define DOSVM_HEARTBEAT_LIMIT 60u
#define DOSVM_PIC_STALL_TICKS 1000u
#define DOSVM_PIC_STALL_LIMIT 32u
#define DOSVM_IDLE_POLLS 8u
#define DOSVM_IDLE_WINDOW 10u
enum dosvm_idle_reason { DOSVM_IDLE_2F, DOSVM_IDLE_28, DOSVM_IDLE_16, DOSVM_IDLE_HLT, DOSVM_IDLE_REASONS };
/* f3-61: the end of a full-screen session. A hotkey (Alt+Enter/Alt+Tab) and a
 * guest mode change are ordinary; the others leave the VM running in its
 * window with a message, except STOPPING, where the VM itself stopped. */
enum dosvm_fullscreen_cause {
    DOSVM_FS_NONE, DOSVM_FS_HOTKEY, DOSVM_FS_BACKGROUND, DOSVM_FS_MODE_CHANGE, DOSVM_FS_PRESENT_ERROR,
    DOSVM_FS_STOPPING, DOSVM_FS_FIRST_FRAME_TIMEOUT, DOSVM_FS_PROGRESS_TIMEOUT, DOSVM_FS_OWNER_LOST,
    DOSVM_FS_REQUEST, DOSVM_FS_CAUSES
};
#include "dosvm_memory.h"
#include "dpmi_internal.h"
#include "dos_conformance_fixture.h"
void rec_sequence_ledger(const char *) __attribute__((weak));
void rec_dos_trace(const char *,const char *) __attribute__((weak));
void dosvm_file_trace_close(struct dosvm *,unsigned);
enum dosvm_trace_kind { DOSVM_TRACE_VECTOR, DOSVM_TRACE_PIT, DOSVM_TRACE_IRQ0,
    DOSVM_TRACE_DB_PM, DOSVM_TRACE_DB_RM, DOSVM_TRACE_FLAGS, DOSVM_TRACE_TIMER,
    DOSVM_TRACE_FAULT, DOSVM_TRACE_COUNT };
enum dosvm_perf_cause {
    DOSVM_COST_RM_INT, DOSVM_COST_RM_IRET, DOSVM_COST_RM_FLAGS,
    DOSVM_COST_PM_INT, DOSVM_COST_PM_FLAGS, DOSVM_COST_SWITCH,
    DOSVM_COST_VGA_IO, DOSVM_COST_VGA_PF, DOSVM_COST_PIT_PIC,
    DOSVM_COST_KBD, DOSVM_COST_IO, DOSVM_COST_DB, DOSVM_COST_TIMER,
    DOSVM_COST_OTHER, DOSVM_COST_RM_PUSHF, DOSVM_COST_RM_POPF,
    DOSVM_COST_RM_CLI, DOSVM_COST_RM_STI, DOSVM_COST_RM_HLT,
    DOSVM_COST_PM_HLT, DOSVM_COST_DPMI, DOSVM_COST_HOST_IRQ, DOSVM_COST_COUNT
};
#define DOSVM_PERF_INTERVAL 5000u
#define DOSVM_SERIAL_INTERVAL 2000u
#define DOSVM_PERF_TOP 5u
#define DOSVM_PERF_PORTS 256u
enum dosvm_if_path { DOSVM_IF_TRAMPOLINE, DOSVM_IF_STI, DOSVM_IF_IRET,
    DOSVM_IF_POPF, DOSVM_IF_API, DOSVM_IF_FORCED, DOSVM_IF_PATHS };
struct dosvm_isr_cost {
    uint64_t us, ms;
    uint32_t count, max_us, max_ms, bins[8];
    uint8_t vector;
};
struct dosvm_hot_cost {
    uint64_t cycles;
    uint32_t count;
    uint16_t id;
};
enum dosvm_vga_reg { DOSVM_VGA_SR_INDEX, DOSVM_VGA_SR_MASK, DOSVM_VGA_SR_OTHER,
    DOSVM_VGA_GC_INDEX, DOSVM_VGA_GC_READ, DOSVM_VGA_GC_MODE, DOSVM_VGA_GC_OTHER,
    DOSVM_VGA_CRTC, DOSVM_VGA_DAC, DOSVM_VGA_OTHER, DOSVM_VGA_REGS };
struct dosvm_perf_sample {
    uint32_t return_hits, return_segmented, return_fallbacks;
    uint32_t return_armed, return_expired, return_reacquired;
    uint32_t ms, frames, flips, harvests, pages, trapped, converted, rows, present_rate;
    uint32_t exits[6], irqs[16];
    uint64_t guest_us, monitor_us, present_us, harvest_us, log_us, edges, insn[V86_INSNS];
    uint32_t cost_count[DOSVM_COST_COUNT], batched;
    uint64_t cost_us[DOSVM_COST_COUNT];
    uint32_t idle_blocks, idle_reasons, idle_count[DOSVM_IDLE_REASONS], busy_permille;
    uint32_t port_remaps, burst_insn, latch_read_windows;
    uint64_t remap_us;
    uint32_t port_count[DOSVM_VGA_REGS], port_exits[DOSVM_VGA_REGS];
    uint64_t port_us[DOSVM_VGA_REGS], port_exit_us[DOSVM_VGA_REGS];
    struct dosvm_hot_cost top_port[DOSVM_PERF_TOP], top_vector[DOSVM_PERF_TOP];
    uint32_t port_overflow, ticks;
    uint64_t timer_us, audio_us;
};
/* Raw cumulative scalars captured with the interval tables detached. Only
 * the reporter reads this bank; producers use the other bank after IF restore. */
struct dosvm_perf_raw {
    uint32_t present_rate;
    uint32_t return_hits, return_segmented, return_fallbacks;
    uint32_t return_armed, return_expired, return_reacquired;
    uint32_t ms, frames, starts, harvests, direct_pages, trapped_bytes, converted_bytes, rows;
    uint32_t port_remaps, burst_insn, latch_read_windows, batched, port_overflow, ticks;
    uint32_t exits[6], irqs[16], cost_count[DOSVM_COST_COUNT];
    uint32_t port_count[DOSVM_VGA_REGS], port_exits[DOSVM_VGA_REGS];
    uint32_t idle_blocks, idle_count[DOSVM_IDLE_REASONS];
    uint64_t guest_cycles, monitor_cycles, present_cycles, harvest_cycles, log_cycles, edges;
    uint64_t remap_cycles, timer_cycles, audio_cycles, insn[V86_INSNS];
    uint64_t cost_cycles[DOSVM_COST_COUNT];
    uint64_t port_cycles[DOSVM_VGA_REGS], port_exit_cycles[DOSVM_VGA_REGS];
};
/* Interval-local disk counters; regions are exact over the private geometry. */
struct dosvm_disk_cost {
    uint64_t bytes;
    uint32_t reads, writes, hits, misses, regions, latency[5];
    uint8_t touched[256]; /* 128 MiB / 64 KiB / 8 */
};
struct dosvm_memory_cost {
    uint32_t queries, first_largest, first_total, largest, total;
    uint32_t allocations, requested, after_largest, after_total;
};
struct dosvm_perf_interval {
    struct dosvm_perf_raw raw;
    struct dosvm_disk_cost disk;
    struct dosvm_hot_cost ports[DOSVM_PERF_PORTS], vectors[3][256];
};
struct dosvm_vector_cost {
    uint64_t cycles, max_cycles;
    uint32_t count;
    uint16_t ax;
};
#define DOSVM_READ_SECTORS 128u
#define DOSVM_READ_BLOCKS 256u /* at most 16 MiB, sized to the base image */
#define DOSVM_READ_FALLBACK_BLOCKS 32u
#define DOSVM_READ_PAGES (DOSVM_READ_SECTORS * 512u / PAGE_SIZE)
struct dos_file;
struct dos_file_ops;
struct vm_cache_block;
struct dos_file_volume;
struct dos_unified;
struct dosvm {
    struct dosvm_info info;
    struct aspace as;
    uint32_t frames[DOSVM_PAGES];
    struct task *task;
    struct v86 monitor;
    struct v86_frame frame;  /* working copy of the trapped frame (f3-04) */
    struct dos_file *files; /* published only after f3-50's checked BIND */
    struct dos_file_volume *file_volume;
    struct v86_frame file_saved;
    uint32_t file_phys,file_bytes;
    struct dos_unified *unified; /* f3-56 DFV2 state: unified profile only */
    struct v86_frame unified_saved;
    uint32_t unified_phys,unified_bytes;
    bool unified_vif,unified_logged;
    bool frame_pm;           /* frame is a ring-3 DPMI client trap_frame */
    bool dpmi_enabled;       /* INT 2Fh/1687h answered for this VM */
    bool return_tracker_tf;  /* immutable platform choice, survives DPMI reset */
    struct vm_memory memory;
    struct dosvm_pit pit;
    struct dosvm_kbd kbd;
    struct dosvm_mouse mouse;
    struct dosvm_audio audio; /* f3-05: SB16 DSP/mixer, OPL3, 8237 and F4 queues */
    bool window_input;
    int32_t input_x, input_y;
    bool pointer_known;
    bool fullscreen, fullscreen_hold, fullscreen_parked, fullscreen_pause, fullscreen_model;
    bool fullscreen_suspended;
    uint32_t fullscreen_ip, fullscreen_cs;
    uint64_t fullscreen_progress;
    int fullscreen_error;
    /* f3-61: why the last full-screen session ended (enum dosvm_fullscreen_cause),
     * kept until the next entry for STATUS/probe evidence. */
    uint8_t fullscreen_cause;
    /* f3-61: per-second frame evidence of the current session: seconds
     * sampled and seconds in which the guest stored to the scanout (aperture
     * PTE dirty bits, or a VBE frame flush). Reset at each entry. */
    uint32_t fullscreen_seconds, fullscreen_frame_seconds;
    /* f3-61c: full-screen entries refused with the windowed continuation and
     * the last reason (negative errno), e.g. the QEMU std-VGA readback (f3-29). */
    uint32_t fullscreen_refusals;
    int fullscreen_refusal;
    uint32_t continuation;
    struct kwait wake;
    struct kwait present_wait; /* request dependency on the VM worker */
    uint64_t present_due_us, present_sample_due_us;
    uint32_t console_sequence, present_vbe_changes, present_samples;
    struct {
        bool sleeping, seen;
        uint64_t last_tick, poll_tick;
        uint32_t events, blocks, reasons, count[DOSVM_IDLE_REASONS], polls, busy_permille;
        /* Observe the real INT 16h return, including guest IVT hooks. Fixed
         * depth; excess nesting simply does not participate in detection. */
        struct { uint32_t cs, ip, ss, sp; bool pm; } poll[4];
        unsigned depth;
    } idle;
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
    bool disable_file_bridge;
    struct d48_media conformance_media; /* controller configures before boot */
    uint64_t file_trace_exported;
    bool file_trace_busy;
    uint8_t game, kernel;
    void (*record_observer)(struct dosvm *,const char *,unsigned,void *);
    void *record_arg;
    char base_sha256[65];
    bool base_cached; /* f3-51c: base_sha256 reused from the session digest cache */
    struct vfs_table disk_table;
    int base, overlay;
    uint32_t base_sectors;   /* sectors the base image holds; zeros after them */
    /* Private 64 KiB LRU blocks, merging base and authoritative overlay.
     * Buffers are PMM-owned MM_VM_PRIVATE pages inside the VM commitment.
     * f3-46: each block is a clean, reclaimable page of the f0-48 global
     * replacement (track); a reclaimed block has phys 0 and regrows on a
     * miss without pressure. Never guest-visible. */
    struct {
        uint32_t lba, count, phys;
        unsigned previous, next;
        struct vm_cache_block *track;
    } read_cache[DOSVM_READ_BLOCKS];
    uint32_t read_reclaimed, read_regrown;
    unsigned read_recent, read_oldest, read_capacity, read_pages;
    char overlay_path[128];
    struct { uint32_t lba, offset; } sectors[DOSVM_OVERLAY_SECTORS];
    unsigned sector_count;
    uint8_t sector[512];
    uint16_t text[80 * 25], keyboard[32];
    unsigned cursor, key_head, key_count;
    /* f3-02 virtual VGA and window presenter (dosvm_vga.c, dosvm_client.c). */
    cvga_state vga __attribute__((aligned(4096))); /* planes page-aligned: f3-06 direct window */
    cvbios video_bios;
    uint16_t vbe_mode;
    struct vm_block vbe_buffer;
    bool vbe_mapped[DPMI_CLIENTS];
    cvx_bus video_bus;
    bool video_bus_fault, video_presented;
    uint8_t video_hsync;
    unsigned video_blank;      /* read index in the vertical blank, 0 outside */
    uint64_t video_vblank_us;  /* host time (us) the next vertical blank is due */
    uint64_t video_tick;
    uint64_t video_due_us, video_period_us;
    uint64_t video_flip_us;
    unsigned video_flip_run, video_present_rate;
    bool video_flip_seen;
    uint32_t video_display, video_changes, video_phase, owner_pid;
    uint32_t video_faults, video_frames;
    /* f3-06/07: direct window (0 none, 1..4 plane + 1, 5 linear). A0000h-
     * AFFFFh maps a model plane or the VM's own linear backing user-writable;
     * sync closes it before the VM's low-page restoration (dosvm_vga.c). */
    unsigned video_direct;
    bool video_read_window, video_writable;
    uint16_t video_pending[4]; /* dirty plane pages deferred by PTE swaps */
    uint32_t video_pm_faults, video_starts, video_maps, video_flushes, video_burst;
    uint32_t video_linear_maps, video_linear_harvests;
    uint32_t video_seen_start, video_lut_display;
    bool video_lut_valid;
    uint32_t video_lut[256];
    uint64_t video_bands; /* destination 8-line bands written by the last present */
    uint32_t video_rows[15]; /* exact destination scanlines, PRESENT_BANDS opt-in */
    struct {
        uint32_t harvests, direct_pages, trapped_bytes, converted_bytes, rows;
        uint32_t port_remaps, burst_insn, latch_read_windows;
        uint64_t remap_cycles;
        unsigned vga_reg;
        uint32_t port_count[DOSVM_VGA_REGS], port_exits[DOSVM_VGA_REGS];
        uint64_t port_cycles[DOSVM_VGA_REGS], port_exit_cycles[DOSVM_VGA_REGS];
        uint32_t last_port_count[DOSVM_VGA_REGS], last_port_exits[DOSVM_VGA_REGS];
        uint64_t last_port_cycles[DOSVM_VGA_REGS], last_port_exit_cycles[DOSVM_VGA_REGS];
        uint32_t last_port_remaps, last_burst_insn, last_latch_read_windows;
        uint64_t last_remap;
        uint32_t exits[6]; /* #GP, #PF, #DB, host timer, other, protected-mode total */
        uint64_t guest_cycles, monitor_cycles, present_cycles, harvest_cycles, log_cycles;
        uint64_t stamp, report_tick;
        /* Independent UART window: leave every existing disk family at 5s. */
        struct {
            uint64_t tick, guest, monitor, cost_cycles[DOSVM_COST_COUNT];
            uint32_t exits[6], idle[DOSVM_IDLE_REASONS], cost_count[DOSVM_COST_COUNT];
            struct dosvm_hot_cost ports[2][DOSVM_PERF_PORTS];
            unsigned active;
            bool reporting;
        } serial;
        unsigned phase; /* 0 suspended, 1 guest, 2 monitor */
        unsigned resume_phase;
        bool trap_active;
        unsigned cause;
        uint64_t trap_cycles, cost_cycles[DOSVM_COST_COUNT], last_cost_cycles[DOSVM_COST_COUNT];
        bool io_valid, reporting;
        uint32_t io_dispatches, io_fast_cycles;
        uint16_t io_port;
        struct dosvm_perf_interval interval[2]; /* preallocated producer/reporter banks */
        unsigned active_interval;
        uint32_t port_overflow, ticks, last_ticks;
        uint64_t timer_cycles, audio_cycles, last_timer, last_audio;
        uint32_t cost_count[DOSVM_COST_COUNT], last_cost_count[DOSVM_COST_COUNT];
        uint32_t return_hits, return_segmented, return_fallbacks;
        uint32_t return_armed, return_expired, return_reacquired;
        uint32_t last_return_hits, last_return_segmented, last_return_fallbacks;
        uint32_t last_return_armed, last_return_expired, last_return_reacquired;
        uint32_t batched, last_batched;
        uint32_t last_frames, last_starts, last_harvests, last_pages, last_trapped, last_converted, last_rows;
        uint32_t last_exits[6], last_irqs[16];
        uint64_t last_guest, last_monitor, last_present, last_harvest, last_log, last_edges;
        uint64_t last_insn[V86_INSNS];
        uint32_t sample_count;
        uint32_t last_idle_blocks, last_idle_count[DOSVM_IDLE_REASONS];
        struct dosvm_perf_sample sample[16]; /* first four + latest twelve intervals */
        struct dosvm_vector_cost vector[3][256]; /* RM dispatch, synthetic BIOS, PM INT */
        uint64_t disk_bytes;
        uint32_t disk_reads, disk_max, disk_hits, disk_misses;
        struct dosvm_memory_cost memory;
        struct dosvm_isr_cost isr[16];
        uint64_t if_samples_ms, if_clear_ms, if_outside_ms;
        uint32_t if_enable[DOSVM_IF_PATHS], isr_max_depth;
        uint32_t isr_retired, isr_expired, isr_late_return;
        uint64_t isr_late_tick;
        bool isr_late_logged;
    } perf;
    struct {
        uint64_t tick;
        uint32_t number;
        bool started, busy;
        uint16_t text[80 * 25];
    } startup;
    /* Owner-armed command launch. The prompt never arms this gate. */
    bool launch_armed, launch_enter_consumed, launch_exec, launch_ready, launch_failed;
    uint16_t launch_cs, launch_ss, launch_sp;
    uint32_t launch_ip, launch_error;
    /* f3-61: the armed program returned to its shell (completed IRET of the
     * EXEC frame without CF), and V86 INT 21h/4B00h frames still running
     * (LIFO; an overflow is never treated as an idle shell). */
    bool launch_returned, exec_overflow;
    uint64_t launch_returned_tick;
    uint8_t exec_pending;
    struct { uint16_t cs, ss, sp; uint32_t ip; } exec_frame[4];
    uint16_t boot_log_count;
    char boot_log[2048], console_line[81];
    unsigned console_length;
    cvga_geometry video_geometry;
    bool video_modex, video_chain4;
    uint8_t video_dac_mask;
    uint16_t video_row_base[DOSVM_VIDEO_HEIGHT];
    uint8_t video_row_pan[DOSVM_VIDEO_HEIGHT];
    uint8_t video_taken[4][CVGA_DIRTY_BYTES];
    uint8_t video_row[DOSVM_VIDEO_WIDTH];
    /* Exact previous indexed scanout, independent of page dirt and flips. */
    uint8_t video_indexes[DOSVM_VIDEO_HEIGHT][DOSVM_VIDEO_WIDTH];
    uint8_t video_index_pan[DOSVM_VIDEO_HEIGHT];
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
    uint8_t port_hint[4]; /* checked slot hints: PIT, keyboard, VGA, other */
    struct dpmi dpmi;        /* f3-04 kernel DPMI host, idle until the first entry */
    /* f3-12: routine UART diagnostics wait for the outermost guest IRQ to
     * return. Fixed storage, independent of guest memory and PIC state. */
    struct {
        char line[8][240];
        uint32_t dropped;
        uint8_t count;
    } deferred_trace;
    /* f3-09: fixed lifetime budgets, with at most four IRQ0 hook epochs. */
    struct {
        uint8_t used[DOSVM_TRACE_COUNT], irq_epochs;
        bool timer_programmed;
        uint64_t timer_tick;
        bool heartbeat_started;
        uint8_t heartbeats;
        uint64_t heartbeat_tick;
        uint8_t pic_stalls;
        const char *boundary;
        uint16_t boundary_cs, injected_cs;
        uint32_t boundary_eip, injected_eip;
    } trace;
};
/* Weak only for existing standalone monitor fixtures, which deliberately do
 * not link storage. The kernel links the production adapter unconditionally. */
int dosvm_file_interrupt(struct dosvm *,struct v86_frame *,uint8_t,unsigned) __attribute__((weak));
int dosvm_file_reflect(struct dosvm *,struct v86_frame *,uint8_t,unsigned,uint16_t) __attribute__((weak));
void dosvm_file_return(struct dosvm *,const struct v86_frame *) __attribute__((weak));
void dosvm_file_reflected_return(struct dosvm *,const struct v86_frame *,bool) __attribute__((weak));
int dosvm_file_destroy(struct dosvm *) __attribute__((weak));
void dosvm_file_dump(struct dosvm *) __attribute__((weak));
void dosvm_file_deadline(struct dosvm *) __attribute__((weak));
int dosvm_file_disk(struct dosvm *,uint32_t,void *,bool) __attribute__((weak));
/* f3-61: kernel=unified binding evidence (vm-game probe); false without one. */
struct dosvm_unified_info {
    bool active; uint32_t owned, requests, opens, closes, reads, failed, critical, expired, refusals, held, start;
    uint64_t read_bytes;
    struct { uint16_t fn, error; char name[13]; } failure[8];   /* f3-61c: first min(failed, 8) */
};
bool dosvm_file_unified_info(const struct dosvm *,struct dosvm_unified_info *) __attribute__((weak));
void dosvm_disk_provider_init(struct dosvm *,struct blkdev *);
void dosvm_file_rom_init(struct dosvm *) __attribute__((weak));
void dosvm_file_ops_init(struct dosvm *,struct dos_file_ops *);
uint64_t dosvm_file_monitor_us(void *);
static inline void dosvm_boundary(struct dosvm *v, const struct v86_frame *f, const char *event)
{
    if (!(g_boot.flags & CBI_F_VERBOSE)) return;
    v->trace.boundary = event;
    v->trace.boundary_cs = (uint16_t)f->tf.cs;
    v->trace.boundary_eip = f->tf.eip;
}
void dosvm_trace(struct dosvm *, enum dosvm_trace_kind, const struct v86_frame *,
                 uint32_t a, uint32_t b, uint32_t c);
void dosvm_diag(struct dosvm *, const char *fmt, ...);
void dosvm_console_byte(struct dosvm *, uint8_t);
void dosvm_launch_video(struct dosvm *);
uint32_t dosvm_task_quantum(const struct task *, uint32_t normal);
const struct task *dosvm_task_focus(void);
const struct task *dosvm_task_irq(void);
int dosvm_io_burst(struct dosvm *, struct v86_frame *);
bool dosvm_io_fast(struct dosvm *, struct v86_frame *, const struct dpmi_seg *, int *);
uint64_t dosvm_perf_clock(void);
uint64_t dosvm_perf_task_clock(void);
void dosvm_vif_restore(struct dosvm *, bool, enum dosvm_if_path);
void dosvm_isr_begin(struct dosvm *, struct dpmi_record *, unsigned irq);
void dosvm_isr_end(struct dosvm *, const struct dpmi_record *);
void dosvm_perf_report(struct dosvm *);
void dosvm_perf_dump(struct dosvm *);
void dosvm_startup_report(struct dosvm *, bool force);
struct dosvm *dosvm_lookup(struct dosvm_key);
int dosvm_vga_init(struct dosvm *);
void cvbios_init_point(const cvbios *);
int dosvm_vga_port(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
int dosvm_vga_bios(struct dosvm *, struct v86_frame *);
struct ciuki_rect;
void dosvm_vbe_init(struct dosvm *);
int dosvm_vbe_bios(struct dosvm *, struct v86_frame *);
int dosvm_vbe_map(struct dosvm *, struct v86_frame *, unsigned);
bool dosvm_vbe_dirty(struct dosvm *, bool clear);
int dosvm_vbe_flush(struct dosvm *, bool force);
int dosvm_vbe_present(struct dosvm *, int (*)(void *, unsigned, const void *, unsigned),
                      void *, uint64_t, struct ciuki_rect *);
void dosvm_fullscreen_mode_changed(struct dosvm *);
bool dosvm_shell_idle(const struct dosvm *);
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
/* Row callback: negative error, zero written, positive already identical. */
int dosvm_vga_present(struct dosvm *, int (*row)(void *, unsigned, const void *, unsigned),
                      void *, uint64_t tick, struct ciuki_rect *damage);
/* Same complete scan as frame_digest, optionally retaining those exact rows. */
int dosvm_frame_capture(struct dosvm *, uint64_t, struct dosvm_frame *,
                        int (*row)(void *,unsigned,const void *,unsigned),void *);
uint8_t dosvm_vga_status(struct dosvm *, uint64_t tick);
int dosvm_disk_open(struct dosvm *, const char *base);
int dosvm_disk_close(struct dosvm *, bool forced);
int dosvm_bios_init(struct dosvm *);
int dosvm_io(void *, uint16_t, unsigned, bool, uint32_t *);
bool dosvm_io_hot(struct dosvm *,uint16_t,unsigned,bool,uint32_t *,int *);
void dosvm_putchar(struct dosvm *, uint8_t, uint8_t);
int dosvm_wait(struct dosvm *, bool keyboard, uint64_t deadline);
void dosvm_input_poll(struct dosvm *);
void dosvm_idle_wake(struct dosvm *);
void dosvm_present_notify(struct dosvm *);
void dosvm_present_checkpoint(struct dosvm *);
unsigned dosvm_vga_period_ms(struct dosvm *);
uint64_t dosvm_vga_period_us(struct dosvm *, uint64_t tick);
uint64_t dosvm_vga_next_due(uint64_t due_us, uint64_t period_us, uint64_t tick);
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
int dpmi_io_repeat(struct dosvm *, struct v86_frame *);
int dpmi_deliver(struct dosvm *, struct v86_frame *);
void dpmi_close(struct dosvm *);
void dpmi_watch_hits(struct dosvm *, uint32_t dr6);
uint32_t dpmi_debug_status(const struct dosvm *, uint32_t dr6);
uint32_t dpmi_fault_linear(struct dosvm *, const struct v86_frame *);
int dpmi_rom_init(struct dosvm *);
void dpmi_info_fill(struct dosvm *, struct dpmi_info *);
#endif
