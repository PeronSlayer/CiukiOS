/* Host-only ctypes shim for scripts/test_vga_x86.py. Plain RAM bus with an
 * access log; it is NOT a VGA device and is never linked into a guest module. */
#include "vga_x86.h"

typedef struct shim_bus {
    uint8_t *memory;
    uint32_t size;
    uint32_t *log;
    uint32_t capacity, count;
} shim_bus;

static int log_access(shim_bus *b, uint32_t linear, uint32_t kind)
{
    if (linear >= b->size) return 1;
    if (b->count < b->capacity) {
        b->log[b->count * 2] = linear;
        b->log[b->count * 2 + 1] = kind;
    }
    ++b->count;
    return 0;
}

static int shim_read(void *context, uint32_t linear, uint8_t *value)
{
    shim_bus *b = (shim_bus *)context;
    if (log_access(b, linear, 0)) return 1;
    *value = b->memory[linear];
    return 0;
}

static int shim_write(void *context, uint32_t linear, uint8_t value)
{
    shim_bus *b = (shim_bus *)context;
    if (log_access(b, linear, 1)) return 1;
    b->memory[linear] = value;
    return 0;
}

static int shim_fetch(void *context, uint32_t linear, uint8_t *value)
{
    shim_bus *b = (shim_bus *)context;
    if (log_access(b, linear, 2)) return 1;
    *value = b->memory[linear];
    return 0;
}

int cvx_shim_execute(cvx_cpu *cpu, uint8_t *memory, uint32_t size, uint32_t budget,
                     uint32_t *log, uint32_t capacity, uint32_t *count, cvx_result *result)
{
    shim_bus b;
    cvx_bus bus;
    int status;
    b.memory = memory;
    b.size = size;
    b.log = log;
    b.capacity = capacity;
    b.count = 0;
    bus.read = shim_read;
    bus.write = shim_write;
    bus.fetch = shim_fetch;
    bus.context = &b;
    status = cvx_execute(cpu, &bus, budget, result);
    *count = b.count;
    return status;
}

unsigned cvx_shim_cpu_size(void) { return (unsigned)sizeof(cvx_cpu); }
unsigned cvx_shim_result_size(void) { return (unsigned)sizeof(cvx_result); }
