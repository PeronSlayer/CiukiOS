/* CiukiOS memory-operand instruction emulator for trapped video apertures.
 *
 * A monitor executes ONE faulting guest instruction with this module when the
 * CPU reports a page fault on a guarded VGA page. Every memory byte goes
 * through the caller's bus callbacks, in the order the processor issues the
 * corresponding bus cycles (read-before-write for read-modify-write forms,
 * source-before-destination for MOVS, low byte first within an operand).
 * The bus decides which linear addresses belong to the virtual VGA model.
 *
 * Freestanding: no allocator, clock, BIOS, DOS or host port access.
 * Supported: 16-bit (V86/real) and 32-bit default sizes, segment overrides,
 * 66h/67h size overrides, LOCK (ignored), REP/REPE/REPNE strings, and memory
 * forms of MOV, MOVZX/MOVSX, MOV moffs, MOV Sreg store, ALU (ADD/OR/ADC/SBB/
 * AND/SUB/XOR/CMP), TEST, XCHG, INC/DEC, NOT/NEG, MUL/IMUL/DIV/IDIV (without
 * raising #DE), rotates/shifts, SETcc, XLAT, and MOVS/STOS/LODS/CMPS/SCAS.
 * Anything else, including FPU/MMX/SSE, BT*, stack forms, far-pointer loads,
 * control transfers through video memory and instruction fetch from the
 * aperture, returns CVX_UNSUPPORTED without changing registers or memory.
 * Architecturally undefined flags follow common Intel results, but callers
 * must not rely on them.
 */
#ifndef CIUKIOS_VGA_X86_H
#define CIUKIOS_VGA_X86_H

#include <stdint.h>
#include "virtual_vga.h"

enum { CVX_ES = 0, CVX_CS, CVX_SS, CVX_DS, CVX_FS, CVX_GS };
enum { CVX_EAX = 0, CVX_ECX, CVX_EDX, CVX_EBX, CVX_ESP, CVX_EBP, CVX_ESI, CVX_EDI };

/* Result codes. CVX_PARTIAL means a REP string instruction consumed its
 * budget: count/index registers were updated, EIP was NOT advanced, and the
 * instruction must restart exactly as an interrupted REP does on hardware. */
enum {
    CVX_DONE = 0, CVX_PARTIAL = 1, CVX_UNSUPPORTED = 2, CVX_BUS_FAULT = 3,
    CVX_DIVIDE_ERROR = 4, CVX_TOO_LONG = 5
};

typedef struct cvx_cpu {
    uint32_t gpr[8];
    uint32_t eip, eflags;
    uint32_t seg_base[6];
    uint16_t sreg[6];
    uint8_t code32;                  /* CS default size is 32 bits */
    uint8_t reserved[3];
} cvx_cpu;

/* Callbacks return 0 on success. A nonzero return aborts emulation with
 * CVX_BUS_FAULT; bytes already transferred remain transferred, so a monitor
 * must treat that as a fatal session error rather than retrying. */
typedef int (CVGA_CALL *cvx_read_fn)(void *context, uint32_t linear, uint8_t *value);
typedef int (CVGA_CALL *cvx_write_fn)(void *context, uint32_t linear, uint8_t value);
typedef struct cvx_bus {
    cvx_read_fn read;
    cvx_write_fn write;
    cvx_read_fn fetch;               /* instruction bytes; may equal read */
    void *context;
} cvx_bus;

typedef struct cvx_result {
    uint32_t length;                 /* decoded instruction length, 0 if unknown */
    uint32_t elements;               /* string elements (or 1) executed */
    uint8_t bytes[16];               /* fetched instruction bytes, for diagnostics */
} cvx_result;

/* budget bounds string elements per call (minimum 1). Returns CVX_*. */
int CVGA_CALL cvx_execute(cvx_cpu *cpu, const cvx_bus *bus, uint32_t budget,
                          cvx_result *result);

#endif
