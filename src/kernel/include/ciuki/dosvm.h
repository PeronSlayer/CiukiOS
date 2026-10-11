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
struct dosvm_config { uint32_t commitment; }; /* zero selects 32 MiB */
struct dosvm_admission {
    uint64_t free_bytes, reserved_bytes, resident_bytes, outstanding_bytes;
    uint32_t requested_bytes, headroom_bytes;
    bool admitted;
};
struct dosvm_info {
    struct dosvm_key key;
    enum dosvm_state state;
    enum dosvm_reason reason;
    uint32_t commitment, resident, fault_vector, fault_eip, fault_address;
    uint32_t guest_records, dropped_records;
    uint16_t free_paragraphs;
    uint8_t dos_major, dos_minor, drive;
    bool dos_ready, dirty, data_loss;
    int cleanup_error;
};
struct dosvm_ledger { uint32_t vms, tasks, pages, handles, devices; uint64_t reserved, resident; };
/* Pure arithmetic uses free physical bytes AFTER firmware/kernel/driver,
 * framebuffer and eagerly backed native commitments. No overcommit. */
bool dosvm_admit(uint64_t free_bytes, uint64_t reserved, uint64_t resident,
                 uint32_t request, struct dosvm_admission *out);
int dosvm_create(const struct dosvm_config *, struct dosvm **out);
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
int dosvm_set_a20(struct dosvm *, bool enabled); /* base alias only; XMS is f3-03 */
typedef int (*dosvm_port_handler)(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
int dosvm_port_register(struct dosvm_key, uint16_t first, uint16_t last,
                        dosvm_port_handler, void *arg);
int dosvm_port_unregister(struct dosvm_key, uint16_t first);
bool dosvm_irq_raise(struct dosvm_key, unsigned irq);
bool dosvm_keyboard(struct dosvm_key, uint16_t bios_key);
void dosvm_tick(void); /* host timer hook; bounded, no allocations or I/O */
const uint16_t *dosvm_text(const struct dosvm *, unsigned *cursor);
/* Host-testable private-sector and BIOS boundaries. */
int dosvm_disk_read(struct dosvm *, uint32_t lba, void *sector);
int dosvm_disk_write(struct dosvm *, uint32_t lba, const void *sector);
int dosvm_bios(struct dosvm *, struct v86_frame *, uint8_t vector);
int dosvm_record_byte(struct dosvm *, uint8_t byte);
/* f3-02 event ingress. f3-01 installs its per-VM device consumer here.
 * No second physical input reader is permitted for a native window. */
struct ciuki_input_event;
typedef bool (*dosvm_input_handler)(struct dosvm_key, const struct ciuki_input_event *, bool focused);
void dosvm_input_register(dosvm_input_handler);
bool dosvm_input_event(struct dosvm_key, const struct ciuki_input_event *, bool focused);
/* Scheduler/arch ownership hooks, identity for every other task. */
bool dosvm_trap(struct trap_frame *);
uint32_t dosvm_task_cr3(const struct task *, uint32_t normal);
uint32_t dosvm_task_esp0(const struct task *, uint32_t normal);
#endif
