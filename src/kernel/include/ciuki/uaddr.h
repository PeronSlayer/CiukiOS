/* F2 user mappings. All mutations run in the non-preemptible UP kernel.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_UADDR_H
#define CIUKI_UADDR_H
#include <ciuki/abi.h>
#include <ciuki/mm.h>

enum ua_kind { UA_ANON, UA_IMAGE, UA_HEAP, UA_STACK, UA_GUARD, UA_TLS };
struct ua_extent {
    uint32_t base, end, prot, maximum, owner;
    uint64_t generation;
    enum ua_kind kind;
    struct ua_extent *next;
};
struct ua_pin {
    uint32_t base, end;
    struct task *task;
    struct ua_pin *next;
};
struct uaddr {
    struct aspace as;
    struct ua_extent *head;
    struct ua_pin *pins;
    uint32_t heap_base, brk, extents, arena_extents, backing;
    void *identity;
};
int ua_init(struct uaddr *u, void *identity);
void ua_destroy(struct uaddr *u);
int ua_protection(uint32_t prot);
int ua_map_at(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot,
              uint32_t maximum, enum ua_kind kind, uint32_t owner);
int32_t ua_mmap(struct uaddr *u, const struct ciuki_mmap_args *args);
int ua_munmap(struct uaddr *u, uint32_t base, uint32_t bytes);
int ua_mprotect(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot);
int32_t ua_brk(struct uaddr *u, uint32_t end);
/* Internal release after a thread is stopped; never bypasses I/O pins. */
void ua_release_owner(struct uaddr *u, uint32_t owner);
struct ua_extent *ua_find(const struct uaddr *u, uint32_t address);
bool ua_range(const struct uaddr *u, uint32_t address, uint32_t bytes, uint32_t prot);
int ua_pin(struct uaddr *u, struct ua_pin *pin, uint32_t base, uint32_t bytes, bool write);
void ua_unpin(struct uaddr *u, struct ua_pin *pin);
void ua_cancel_pins(struct uaddr *u, const struct task *task);
/* Supervisor transfer via direct-map backing, including original RX loads. */
int ua_read(const struct uaddr *u, void *dst, uint32_t src, uint32_t bytes);
int ua_write(const struct uaddr *u, uint32_t dst, const void *src, uint32_t bytes);
int copy_from_user(void *dst, uint32_t src, uint32_t bytes);
int copy_to_user(uint32_t dst, const void *src, uint32_t bytes);
#endif
