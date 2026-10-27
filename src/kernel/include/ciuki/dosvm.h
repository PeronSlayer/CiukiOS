/* Kernel-internal DOS VM services. Physical VGA belongs only to the single
 * full-screen owner; native clients never receive physical addresses/ports.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_H
#define CIUKI_DOSVM_H
#include <ciuki/v86.h>
#define DOSVM_LIMIT 4u
#define DOSVM_LINEAR_END 0x0010FFF0u
#define DOSVM_LOW_CAP (8u * 1024u * 1024u)
#define DOSVM_RESIDENT_CAP (48u * 1024u * 1024u)
#define DOSVM_HEADROOM (32u * 1024u * 1024u)
#define DOSVM_RECORD_PORT 0xE9u
#define DOSVM_CONTROL_PORT 0xEAu
#define DOSVM_FAULT_PORT 0xFFFFu
enum dosvm_state { DOSVM_CREATING, DOSVM_BOOTING, DOSVM_RUNNABLE,
                   DOSVM_BLOCKED, DOSVM_STOPPING, DOSVM_DEAD };
enum dosvm_reason { DOSVM_EXIT, DOSVM_TERMINATE, DOSVM_FAULT, DOSVM_RESET, DOSVM_TIMEOUT };
struct dosvm;
struct task;
struct dos_identity;
struct dosvm_key { uint32_t id, generation; };
struct dosvm_config {
    uint32_t commitment; /* zero selects DOSVM_RESIDENT_CAP */
    uint16_t ems_frame; /* zero/E000h default, D000h alternative */
    bool disable_umb;
    const char *probe; /* record owner for admission/stop/guest rows; NULL = "vm-boot" */
    bool dpmi;         /* f3-04 staged testing: answer INT 2Fh/1687h before DOSVM_DPMI_PUBLIC */
    /* f3-06: read-only base image (VFS path, the private FAT16 geometry:
     * DOSVM_SECTORS sectors, or fewer when its trailing zero sectors were
     * cut, which then read as zeros); NULL selects C:\SYSTEM\DOS\UNIFIED.IMG.
     * sec-k05: the writable overlay and per-title data live in the owner's
     * profile (dos_identity.h); C:\SYSTEM\DOS only for internal VMs and the
     * interim bootstrap session (any base volume). */
    const char *base;
    bool disable_file_bridge; /* unified disables the retired v1 bridge */
    uint8_t game, kernel; /* game 1..3; kernel 2=unified; 0 only for explicit host-fixture seams */
    /* f3-46 per-game compatibility (Windows 98 PIF "XMS/EMS memory locked",
     * Resource Kit table 25.2 xml/eml): the pool is backed eagerly at
     * creation and stays resident. Default: demand-zero, pageable pool. */
    bool memory_locked;
    void (*record_observer)(struct dosvm *,const char *,unsigned,void *);
    void *record_arg; /* selected controller only, lifetime through destroy */
    uint8_t serial[4]; /* trusted launch: 0=null, 1..8=native COM1..8 */
    /* fx-4: true once the creating owner is stopping; polled between the
     * blocks of the cold base identity hash, which then returns EINTR.
     * Called only during dosvm_create, on the creating thread. */
    bool (*cancelled)(void *arg);
    void *cancel_arg;
    /* sec-k05: the creator's captured identity (dos_identity.h), fixed before
     * guest boot; the VM takes its own subject reference. NULL only for a
     * kernel-internal VM (probes, conformance), which runs as SYSTEM and is
     * never reachable from a user process. */
    const struct dos_identity *identity;
};
/* f3-02 window adapter: desktop KEY is public set 1; MOTION is surface-local
 * absolute coordinates; BUTTON is 1/2/3 and down/up. TEXT is ignored after
 * a window claims focus. Kernel trusted transport supplies the VM key. */
struct ciuki_input_event;
bool dosvm_input_focus(struct dosvm_key, bool focused);
bool dosvm_input_event(struct dosvm_key, const struct ciuki_input_event *);
struct dosvm_key dosvm_focused_key(void);
/* Internal result: 0 entered, DISPLAY_FULLSCREEN_WINDOWED refused safely,
 * negative on failure. The owner syscall translates windowed to success. */
int dosvm_fullscreen_enter(struct dosvm_key);
int dosvm_fullscreen_leave(struct dosvm_key, bool background);
bool dosvm_fullscreen_active(struct dosvm_key);
void dosvm_fullscreen_poll(void);
void dosvm_fullscreen_checkpoint(struct dosvm *);
void dosvm_fullscreen_stopping(struct dosvm *);
void dosvm_fullscreen_switch(const struct task *);
void dosvm_fullscreen_progress(struct dosvm *, uint32_t cs, uint32_t ip);
void dosvm_fullscreen_model_begin(struct dosvm *);
void dosvm_fullscreen_model_end(struct dosvm *);
int dosvm_fullscreen_port(uint16_t, unsigned, bool, uint32_t *);
/* IRQ-safe merged-input filter: no firmware, waits or allocation. */
bool dosvm_fullscreen_input(uint16_t type, uint16_t code, int32_t value);
bool dosvm_fullscreen_desktop_event(struct ciuki_input_event *);
struct dosvm_device_info {
    uint64_t host_ticks, pit_clocks, pit_edges, pit_dropped_ticks, key_overflows;
    uint32_t queued_bytes, held_keys, buttons;
    uint8_t irr[2], isr[2], imr[2];
    uint32_t delivered[16], last_vector, pic_base[2];
};
bool dosvm_device_info(struct dosvm_key, struct dosvm_device_info *);
struct dosvm_admission {
    uint64_t free_bytes, reserved_bytes, resident_bytes, outstanding_bytes;
    uint32_t requested_bytes, headroom_bytes;
    bool admitted;
};
struct dosvm_info {
    struct dosvm_key key;
    enum dosvm_state state;
    enum dosvm_reason reason;
    uint32_t commitment, resident, fault_vector, fault_eip, fault_address, fault_error;
    uint32_t guest_records, dropped_records;
    uint16_t free_paragraphs;
    uint8_t dos_major, dos_minor, drive;
    bool dos_ready, dirty, data_loss;
    int cleanup_error;
    uint32_t pool_quota, pool_used;
    /* f3-46 split: address quota above; residency, paged-out and pinned
     * pool bytes here (commitment is the whole quota, RAM or pagefile). */
    uint32_t pool_resident, pool_swapped, pool_pinned;
    bool pool_locked;
    bool memory_done, memory_pass;
    uint32_t xms_calls, ems_calls;
    uint32_t video_stage, video_flags; /* f3-02 VMVIDEO.COM "VMVIDEO s f" latch */
    /* f3-04: verdict of the payload named by dosvm_expect(): "<NAME> PASS"
     * or "<NAME> FAIL <detail>" on the record port (data, not evidence). */
    bool verdict_done, verdict_pass;
    char verdict_detail[20];
};
/* pages: physical pages owned now. reserved/resident: the VM-level RAM
 * admission promise and residency (bytes). f3-46: pool commitment, paged-out
 * and pinned pool pages, reclaimable read-cache pages and overlay index rows
 * (the copy-on-write index is disk semantics, never cache debt). */
struct dosvm_ledger { uint32_t vms, tasks, pages, handles, devices; uint64_t reserved, resident;
                      uint32_t committed, swapped, pinned, cache, overlay_rows; };
/* Pure arithmetic uses free physical bytes AFTER firmware/kernel/driver,
 * framebuffer and eagerly backed native commitments. No overcommit. */
bool dosvm_admit(uint64_t free_bytes, uint64_t reserved, uint64_t resident,
                 uint32_t request, struct dosvm_admission *out);
int dosvm_create(const struct dosvm_config *, struct dosvm **out);
/* f3-06: the owner's local workload disk (scripts/test/dos_fixtures.py):
 * "X:\CIUKIGAM.IMG" of exactly the private geometry on the first drive after
 * C: that has it. 0 with path filled, -ENOENT without one, else the error.
 * The workload probes use only this disk (their pins are the owner's data). */
int dosvm_workload_base(char path[24]);
/* Window VMs always use C:\SYSTEM\DOS\UNIFIED.IMG. No local fixture or
 * retired kernel is substituted by this lookup. */
int dosvm_window_base(char path[32]);
int dosvm_boot(struct dosvm *);
struct dosvm_key dosvm_key(const struct dosvm *);
bool dosvm_valid(struct dosvm_key);
void dosvm_info(const struct dosvm *, struct dosvm_info *);
void dosvm_snapshot(struct dosvm_ledger *);
/* Stop revokes producers immediately. Destroy waits for the worker to leave
 * guest execution, then follows the single idempotent release path. */
int dosvm_stop(struct dosvm_key, enum dosvm_reason);
int dosvm_destroy(struct dosvm_key, bool forced);
bool dosvm_range(const struct dosvm *, uint32_t linear, unsigned bytes, bool write);
int dosvm_read(struct dosvm *, uint32_t linear, void *, unsigned bytes);
/* f3-46: resident frames only (virtual DMA in the clock work): never pages
 * in or allocates; -EAGAIN when a pool page is not resident. */
int dosvm_read_resident(struct dosvm *, uint32_t linear, void *, unsigned bytes);
int dosvm_write(struct dosvm *, uint32_t linear, const void *, unsigned bytes);
int dosvm_set_a20(struct dosvm *, bool enabled);
typedef int (*dosvm_port_handler)(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
int dosvm_port_register(struct dosvm_key, uint16_t first, uint16_t last,
                        dosvm_port_handler, void *arg);
int dosvm_port_unregister(struct dosvm_key, uint16_t first);
/* Trusted launcher only. slot=0..3, native_index=-1 for the null sink.
 * Bind before boot; running VMs cannot change physical ownership. */
/* native_index -2=none, -1=null, 0..7=physical. */
int dosvm_uart_bind(struct dosvm_key, unsigned slot, int native_index, const char **reason);
bool dosvm_irq_raise(struct dosvm_key, unsigned irq);
bool dosvm_keyboard(struct dosvm_key, uint16_t bios_key);
/* PIT top half (fx-9): O(DOSVM_LIMIT) sampling and an O(1) wake of the
 * vm-clock task; no allocations, I/O or VM state advance. */
void dosvm_tick(void);
const uint16_t *dosvm_text(const struct dosvm *, unsigned *cursor);
/* f3-02: SHA-256 of the complete 640x480 XRGB presenter frame at the given
 * blink/cursor clock, without a window surface; mode is BDA 0049h. */
struct dosvm_frame { uint8_t digest[32]; uint32_t width, height, scale, mode, text; };
int dosvm_frame_digest(struct dosvm *, uint64_t tick, struct dosvm_frame *);
/* f3-06: VGA activity of a VM for workload probes (diagnostic snapshot). */
struct dosvm_video_info {
    uint32_t mode;            /* BDA 0049h */
    uint32_t unchained;       /* SR04 chain-4 clear, odd/even disabled: Mode X/Y planes */
    uint32_t width, height;   /* model scanout geometry, 0 when unsupported */
    uint32_t faults, pm_faults; /* aperture faults emulated or retried; of which from DPMI clients */
    uint32_t starts;          /* CRTC start-address-high writes (page flips) */
    uint32_t maps, flushes;   /* direct plane windows opened; window pages copied back */
    uint32_t frames;          /* presenter frames produced */
    uint32_t burst;           /* instructions emulated after a fault without trapping */
    uint32_t linear_maps, linear_harvests; /* chain-4 windows opened; dirty pages harvested */
};
bool dosvm_video_info(struct dosvm_key, struct dosvm_video_info *);
/* Host-testable private-sector and BIOS boundaries. */
int dosvm_disk_read(struct dosvm *, uint32_t lba, void *sector);
int dosvm_disk_write(struct dosvm *, uint32_t lba, const void *sector);
int dosvm_bios(struct dosvm *, struct v86_frame *, uint8_t vector);
int dosvm_record_byte(struct dosvm *, uint8_t byte);
/* f3-02 native window owner boundary (dosvm_client.c): syscall 70 and the
 * supervisor's owner-death collection. No physical input or display grant. */
int32_t dosvm_client_syscall(struct trap_frame *);
void dosvm_client_poll(void);
/* sec-k05 teardown hook for sec-k07: terminate every window VM created in
 * this security session (forced, like an owner death). Sleeps; call it from
 * the SYSTEM teardown context after the session left ACTIVE. Returns the
 * number of VMs still retained for a later retry. */
unsigned dosvm_session_revoke(uint32_t session);
struct dosvm *dosvm_client_first(uint32_t *owner_pid);
void dosvm_expect(struct dosvm *, const char *name);   /* resets the verdict latch */
/* f3-06: the 80x25 text cells latched when the expected verdict line
 * arrived (NULL before it); a child's output survives the shell's redraw. */
const uint16_t *dosvm_verdict_text(const struct dosvm *);
/* Scheduler/arch ownership hooks, identity for every other task. */
bool dosvm_trap(struct trap_frame *);
bool dosvm_client(const struct task *);       /* f3-04: worker running a DPMI client frame */
void dosvm_task_switch(const struct task *);  /* f3-04: per-VM LDT and watchpoints */
uint32_t dosvm_task_cr3(const struct task *, uint32_t normal);
uint32_t dosvm_task_esp0(const struct task *, uint32_t normal);
#endif
