/* Private implementation shared by the three F3-00 components.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_INTERNAL_H
#define CIUKI_DOSVM_INTERNAL_H
#include <ciuki/dosvm.h>
#include <ciuki/abi.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/sync.h>
#include "virtual_vga_bios.h"
#define DOSVM_PAGES 272u
#define DOSVM_SECTORS 262144u
#define DOSVM_OVERLAY_SECTORS 1024u
#define DOSVM_PORTS 16u
#include "dosvm_memory.h"
struct dosvm {
    struct dosvm_info info;
    struct aspace as;
    uint32_t frames[DOSVM_PAGES];
    struct task *task;
    struct v86 monitor;
    struct v86_frame frame;
    struct vm_memory memory;
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
    uint8_t midnight, disk_status, kbc_command, kbc_output, kbc_reply;
    bool kbc_pending;
    const char *probe;
    struct vfs_table disk_table;
    int base, overlay;
    char overlay_path[48];
    struct { uint32_t lba, offset; } sectors[DOSVM_OVERLAY_SECTORS];
    unsigned sector_count;
    uint8_t sector[512];
    uint16_t text[80 * 25], keyboard[32];
    unsigned cursor, key_head, key_count;
    cvga_state vga;
    cvbios video_bios;
    cvx_bus video_bus;
    bool video_bus_fault, video_presented, focused;
    uint64_t video_tick;
    uint32_t video_display, video_phase, owner_pid;
    cvga_geometry video_geometry;
    uint8_t video_row[640];
    uint32_t video_pixels[640];
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
int dosvm_vga_present(struct dosvm *, int (*row)(void *, unsigned, const void *, unsigned),
                      void *, uint64_t, struct ciuki_rect *);
int dosvm_disk_open(struct dosvm *);
int dosvm_disk_close(struct dosvm *, bool forced);
int dosvm_bios_init(struct dosvm *);
int dosvm_io(void *, uint16_t, unsigned, bool, uint32_t *);
void dosvm_putchar(struct dosvm *, uint8_t, uint8_t);
int dosvm_wait(struct dosvm *, bool keyboard, uint64_t deadline);
void dosvm_input_poll(struct dosvm *);
static inline const char *dosvm_probe(const struct dosvm *v)
{ return v->probe ? v->probe : "vm-boot"; }
#endif
