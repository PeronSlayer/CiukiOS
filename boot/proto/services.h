#ifndef CIUKI_SERVICES_H
#define CIUKI_SERVICES_H

#include <stdint.h>

typedef enum ciuki_com_exit_reason {
    CIUKI_COM_EXIT_RETURN   = 0,
    CIUKI_COM_EXIT_INT20    = 1,
    CIUKI_COM_EXIT_INT21_4C = 2,
    CIUKI_COM_EXIT_API      = 3
} ciuki_com_exit_reason_t;

typedef struct ciuki_int21_regs {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t si;
    uint16_t di;
    uint16_t ds;
    uint16_t es;
    uint8_t carry;
    uint8_t reserved[3];
} ciuki_int21_regs_t;

typedef struct ciuki_dos_context {
    void *boot_info;
    void *handoff;

    uint16_t psp_segment;
    uint16_t reserved0;
    uint32_t reserved1;

    uint64_t psp_linear;
    uint64_t image_linear;
    uint32_t image_size;
    uint8_t command_tail_len;
    uint8_t exit_code;
    uint8_t exit_reason;
    uint8_t reserved2;
    char command_tail[128];
} ciuki_dos_context_t;

/*
 * ciuki_services_t — function table passed by stage2 to every loaded COM.
 * COM binaries must not call stage2 functions directly (no stable ABI);
 * they must only use these pointers.
 */
typedef struct ciuki_services {
    void     (*print)(const char *s);
    void     (*print_hex64)(unsigned long long v);
    void     (*cls)(void);
    void     (*int21)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
    void     (*int2f)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
    void     (*int20)(ciuki_dos_context_t *ctx);
    void     (*int21_4c)(ciuki_dos_context_t *ctx, uint8_t code);
    void     (*terminate)(ciuki_dos_context_t *ctx, uint8_t code);
} ciuki_services_t;

/* COM entry point convention */
typedef void (*com_entry_t)(ciuki_dos_context_t *ctx, ciuki_services_t *svc);

#endif
