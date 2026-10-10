/* Production task stack fill/scan, with inaccessible pages at both ends.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
    unsigned page = (unsigned)sysconf(_SC_PAGESIZE);
    assert(page && !(KSTACK_SIZE % page));
    uint8_t *map = mmap(0, KSTACK_SIZE + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(map != MAP_FAILED);
    uint8_t *base = map + page;
    assert(!mprotect(base, KSTACK_SIZE, PROT_READ | PROT_WRITE));
    struct task t = { .kstack = base }, boot = { 0 };
    assert(!task_stack_high_water(0) && !task_stack_high_water(&boot));
    task_stack_init(base);
    assert(*(uint32_t *)base == KSTACK_CANARY && !task_stack_high_water(&t));
    for (unsigned i = sizeof(uint32_t); i < KSTACK_SIZE; i++) assert(base[i] == KSTACK_FILL);
    base[KSTACK_SIZE - 1] = 0;
    assert(task_stack_high_water(&t) == 1);
    base[KSTACK_SIZE - 513] = 0;
    assert(task_stack_high_water(&t) == 513 && task_stack_high_water(&t) == 513);
    base[KSTACK_SIZE - 17] = 0;
    assert(task_stack_high_water(&t) == 513); /* shallower activity cannot erase peak */
    base[sizeof(uint32_t)] = 0;
    assert(task_stack_high_water(&t) == KSTACK_SIZE - sizeof(uint32_t));
    *(uint32_t *)base = 0;
    assert(task_stack_high_water(&t) == KSTACK_SIZE);
    task_stack_init(base);
    assert(!task_stack_high_water(&t)); /* slot reuse resets the watermark */
    assert(!munmap(map, KSTACK_SIZE + 2 * page));
    puts("stack watermark: PASS (empty, byte depth, peak, canary, reuse, bounded scan)");
    return 0;
}
