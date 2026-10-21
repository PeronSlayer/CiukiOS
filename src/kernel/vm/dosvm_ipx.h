/* Legacy DOS IPX API, copied frames and pinned ECB snapshots.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_IPX_H
#define CIUKI_DOSVM_IPX_H
#include <ciuki/kernel.h>
#define DOSVM_IPX_SOCKETS 32u
#define DOSVM_IPX_ECBS 64u /* aggregate per VM; hence also <=64 per socket */
#define DOSVM_IPX_FRAGMENTS 16u
#define DOSVM_IPX_ENTRY 0x1c00u
#define DOSVM_IPX_RETURN 0x1c04u
enum dosvm_ipx_completion {
    DOSVM_IPX_SUCCESS = 0, DOSVM_IPX_CANCELLED = 0xfc,
    DOSVM_IPX_MALFORMED = 0xfd, DOSVM_IPX_UNDELIVERABLE = 0xfe,
    DOSVM_IPX_HARDWARE = 0xff
};
struct dosvm_ipx_regs { uint16_t ax, bx, cx, dx, si, di, es; };
struct dosvm_ipx_upcall {
    uint16_t cs, ip, es, si;
    uint8_t al; /* FF IPX; 00 asynchronous event scheduler. RETF, virtual IF=0. */
    uint32_t generation;
};
struct dosvm_ipx_ops {
    void *arg;
    /* pin resolves and retains the entire range before returning, including
     * across any sleeping copy; nonzero token on success. No borrowed guest
     * pointer is stored. read/write return an error rather than panic. */
    int (*pin)(void *, uint32_t linear, unsigned, bool write, uint32_t *token);
    void (*unpin)(void *, uint32_t token);
    int (*read)(void *, uint32_t linear, void *, unsigned);
    int (*write)(void *, uint32_t linear, const void *, unsigned);
    int (*send)(void *, const uint8_t *, unsigned);
    void (*relinquish)(void *);
    /* Enqueue/enter guest far call at the existing VM callback boundary.
     * 1 accepted, 0 unsafe now, negative invalid target. Never call guest code
     * as host code. Owner validates VM generation and guest executable range. */
    int (*upcall)(void *, const struct dosvm_ipx_upcall *);
};
struct dosvm_ipx_fragment { uint32_t linear, pin; uint16_t length; };
struct dosvm_ipx_ecb {
    uint32_t linear, pin, esr_pin;
    uint16_t es, si, esr_cs, esr_ip, socket, total;
    uint64_t order, due;
    uint8_t state, fragments, event_al;
    uint8_t immediate[6];
    struct dosvm_ipx_fragment fragment[DOSVM_IPX_FRAGMENTS];
};
struct dosvm_ipx_socket { uint16_t number; uint8_t lifetime; bool open; };
struct dosvm_ipx {
    struct dosvm_ipx_ops ops;
    struct dosvm_ipx_socket socket[DOSVM_IPX_SOCKETS];
    struct dosvm_ipx_ecb ecb[DOSVM_IPX_ECBS];
    uint8_t mac[6], frame[1514];
    uint32_t generation, rejected, memory_errors, rx_dropped, callbacks_dropped;
    uint64_t ticks, sequence;
    uint16_t next_dynamic;
    bool enabled, servicing;
};
int dosvm_ipx_init(struct dosvm_ipx *, uint32_t generation, const uint8_t mac[6], const struct dosvm_ipx_ops *);
/* INT2F AX=7A00 -> AL=FF ES:DI; INT7A -> BX function. Returns 1 handled. */
int dosvm_ipx_interrupt(struct dosvm_ipx *, unsigned vector, struct dosvm_ipx_regs *);
/* ticks are monotonic 18.2065 Hz BIOS-interval units, independent of guest
 * writes to the BIOS wall clock or PIT divisor. Work is <=64 entries/pass. */
void dosvm_ipx_service(struct dosvm_ipx *, uint64_t ticks);
int dosvm_ipx_receive(struct dosvm_ipx *, const uint8_t *, unsigned);
int dosvm_ipx_dispatch(struct dosvm_ipx *);
void dosvm_ipx_disconnect(struct dosvm_ipx *);
void dosvm_ipx_destroy(struct dosvm_ipx *);
#endif
