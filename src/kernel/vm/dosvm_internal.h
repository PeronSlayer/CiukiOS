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
#define DOSVM_PORTS 16u
/* f3-02 presenter: fixed 640x480 XRGB window surface, integer scale only.
 * One frame per 33 host ticks; the last three ticks are vertical retrace. */
#define DOSVM_VIDEO_WIDTH 640u
#define DOSVM_VIDEO_HEIGHT 480u
#define DOSVM_FRAME_TICKS 33u
#define DOSVM_RETRACE_TICKS 3u
#define DOSVM_CURSOR_TICKS 250u
#define DOSVM_BLINK_TICKS 500u
#include "dosvm_memory.h"
struct dosvm {
    struct dosvm_info info;
    struct aspace as;
    uint32_t frames[DOSVM_PAGES];
    struct task *task;
    struct v86 monitor;
    struct v86_frame frame;
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
    char overlay_path[48];
    struct { uint32_t lba, offset; } sectors[DOSVM_OVERLAY_SECTORS];
    unsigned sector_count;
    uint8_t sector[512];
    uint16_t text[80 * 25], keyboard[32];
    unsigned cursor, key_head, key_count;
    /* f3-02 virtual VGA and window presenter (dosvm_vga.c, dosvm_client.c). */
    cvga_state vga;
    cvbios video_bios;
    cvx_bus video_bus;
    bool video_bus_fault, video_presented;
    uint8_t video_hsync;
    uint64_t video_tick;
    uint32_t video_display, video_phase, owner_pid;
    uint32_t video_faults, video_frames;
    cvga_geometry video_geometry;
    uint8_t video_taken[4][CVGA_DIRTY_BYTES];
    uint8_t video_row[DOSVM_VIDEO_WIDTH];
    uint32_t video_pixels[DOSVM_VIDEO_WIDTH];
    char name[32];
    char line[81];
    unsigned line_length;
    bool line_bad;
    struct {
        uint16_t first, last;
        dosvm_port_handler handler;
        void *arg;
    } ports[DOSVM_PORTS];
};
struct dosvm *dosvm_lookup(struct dosvm_key);
int dosvm_vga_init(struct dosvm *);
int dosvm_vga_bios(struct dosvm *, struct v86_frame *);
int dosvm_vga_fault(struct dosvm *, struct v86_frame *);
/* Writes changed rows of the 640x480 frame through row(arg, y, pixels, bytes).
 * Returns 1 with *damage set when rows changed, 0 when nothing changed or the
 * frame period has not elapsed, or the negative row-writer error. */
struct ciuki_rect;
int dosvm_vga_present(struct dosvm *, int (*row)(void *, unsigned, const void *, unsigned),
                      void *, uint64_t tick, struct ciuki_rect *damage);
uint8_t dosvm_vga_status(struct dosvm *, uint64_t tick);
int dosvm_disk_open(struct dosvm *);
int dosvm_disk_close(struct dosvm *, bool forced);
int dosvm_bios_init(struct dosvm *);
int dosvm_io(void *, uint16_t, unsigned, bool, uint32_t *);
void dosvm_putchar(struct dosvm *, uint8_t, uint8_t);
int dosvm_wait(struct dosvm *, bool keyboard, uint64_t deadline);
void dosvm_input_poll(struct dosvm *);
int dosvm_keyboard_refresh(struct dosvm *);
static inline const char *dosvm_probe(const struct dosvm *v)
{ return v->probe ? v->probe : "vm-boot"; }
#endif
