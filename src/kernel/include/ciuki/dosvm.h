/* Kernel-internal DOS VM services. No native ABI or physical device grants.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_H
#define CIUKI_DOSVM_H
#include <ciuki/v86.h>
#define DOSVM_LIMIT 4u
#define DOSVM_LINEAR_END 0x0010FFF0u
#define DOSVM_LOW_CAP (8u * 1024u * 1024u)
#define DOSVM_RESIDENT_CAP (32u * 1024u * 1024u)
#define DOSVM_HEADROOM DOSVM_RESIDENT_CAP
#define DOSVM_RECORD_PORT 0xE9u
#define DOSVM_CONTROL_PORT 0xEAu
#define DOSVM_FAULT_PORT 0xFFFFu
enum dosvm_state { DOSVM_CREATING, DOSVM_BOOTING, DOSVM_RUNNABLE,
                   DOSVM_BLOCKED, DOSVM_STOPPING, DOSVM_DEAD };
enum dosvm_reason { DOSVM_EXIT, DOSVM_TERMINATE, DOSVM_FAULT, DOSVM_RESET, DOSVM_TIMEOUT };
struct dosvm;
struct task;
struct dosvm_key { uint32_t id, generation; };
struct dosvm_config {
    uint32_t commitment; /* zero selects 32 MiB */
    uint16_t ems_frame; /* zero/E000h default, D000h alternative */
    bool disable_umb;
    const char *probe; /* record owner for admission/stop/guest rows; NULL = "vm-boot" */
    bool dpmi;         /* f3-04 staged testing: answer INT 2Fh/1687h before DOSVM_DPMI_PUBLIC */
    /* f3-06: read-only base image (VFS path, the private FAT16 geometry:
     * DOSVM_SECTORS sectors, or fewer when its trailing zero sectors were
     * cut, which then read as zeros); NULL selects C:\SYSTEM\DOS\DOSBOOT.IMG.
     * The writable overlay always stays in C:\SYSTEM\DOS (any base volume). */
    const char *base;
};
/* f3-02 window adapter: desktop KEY is public set 1; MOTION is surface-local
 * absolute coordinates; BUTTON is 1/2/3 and down/up. TEXT is ignored after
 * a window claims focus. Kernel trusted transport supplies the VM key. */
struct ciuki_input_event;
bool dosvm_input_focus(struct dosvm_key, bool focused);
bool dosvm_input_event(struct dosvm_key, const struct ciuki_input_event *);
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
    bool memory_done, memory_pass;
    uint32_t xms_calls, ems_calls;
    uint32_t video_stage, video_flags; /* f3-02 VMVIDEO.COM "VMVIDEO s f" latch */
    /* f3-04: verdict of the payload named by dosvm_expect(): "<NAME> PASS"
     * or "<NAME> FAIL <detail>" on the record port (data, not evidence). */
    bool verdict_done, verdict_pass;
    char verdict_detail[20];
};
struct dosvm_ledger { uint32_t vms, tasks, pages, handles, devices; uint64_t reserved, resident; };
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
/* Window VMs (/Programs/dosbox): that local disk when attached, else the bundled
 * shareware games disk C:\GAMES\CIUKIGAM.IMG (owner decision 2026-10-12;
 * scripts/build_games_disk.py). Without a fixture, bundled lookup errors
 * (including ENOENT) are returned; an invalid bundled entry is EINVAL. */
int dosvm_window_base(char path[24]);
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
int dosvm_write(struct dosvm *, uint32_t linear, const void *, unsigned bytes);
int dosvm_set_a20(struct dosvm *, bool enabled);
typedef int (*dosvm_port_handler)(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
int dosvm_port_register(struct dosvm_key, uint16_t first, uint16_t last,
                        dosvm_port_handler, void *arg);
int dosvm_port_unregister(struct dosvm_key, uint16_t first);
bool dosvm_irq_raise(struct dosvm_key, unsigned irq);
bool dosvm_keyboard(struct dosvm_key, uint16_t bios_key);
void dosvm_tick(void); /* host timer hook; bounded, no allocations or I/O */
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
