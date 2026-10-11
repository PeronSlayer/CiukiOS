/* Private implementation shared by the three F3-00 components.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_INTERNAL_H
#define CIUKI_DOSVM_INTERNAL_H
#include <ciuki/dosvm.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/sync.h>
#include "dosvm_devices.h"
#define DOSVM_PAGES 272u
#define DOSVM_SECTORS 262144u
#define DOSVM_OVERLAY_SECTORS 1024u
#define DOSVM_PORTS 16u
#include "dosvm_memory.h"
#include "dpmi_internal.h"
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
    char line[81];
    char verdict_name[12];
    unsigned line_length;
    bool line_bad;
    struct {
        uint16_t first, last;
        dosvm_port_handler handler;
        void *arg;
    } ports[DOSVM_PORTS];
    struct dpmi dpmi;        /* f3-04 kernel DPMI host, idle until the first entry */
};
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
/* Low page protection: ROM/stub pages read-only, DPMI real-mode host stack
 * pages (C6000h-C7FFFh) writable; shared by creation and EMS teardown. */
static inline uint32_t dosvm_low_flags(unsigned page)
{
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
uint32_t dpmi_fault_linear(struct dosvm *, const struct v86_frame *);
int dpmi_rom_init(struct dosvm *);
void dpmi_info_fill(struct dosvm *, struct dpmi_info *);
#endif
