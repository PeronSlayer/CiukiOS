/* Boot guard research: Clang's global guard option avoids TLS/GS, which the
 * kernel does not provide:
 * https://clang.llvm.org/docs/ClangCommandLineReference.html#mstack-protector-guard
 * Seed once in registry_init, before sched_init/task creation. All early
 * callees have returned; kmain never returns. No suspended frame can retain
 * the old guard. The initializer itself must have no saved guard.
 * Prefer the initialized entropy DRBG at this safe point. If collection
 * failed, the original TSC/PIT guard fallback is NOT a cryptographic seed.
 * There is no later guard change, RDRAND or floating-point code here.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/init.h>
#include <ciuki/entropy.h>
#include <ciuki/entropy_pool.h>

uintptr_t __stack_chk_guard;
static bool seeded;

__attribute__((no_stack_protector)) void stackprot_init(void)
{
    if (seeded) return;
    entropy_boot();
    uint32_t secret;
    if (!entropy_get(&secret, sizeof(secret))) {
        __stack_chk_guard = secret ? secret : 0xC1A04B37u;
        entropy_erase(&secret, sizeof(secret));
        seeded = true;
        return;
    }
    uint64_t cycles = ktime_cycles(), ticks = g_ticks;
    uint32_t value = (uint32_t)cycles ^ (uint32_t)(cycles >> 32) ^
                     (uint32_t)ticks ^ (uint32_t)(ticks >> 32) ^ 0xC1A04B37u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    __stack_chk_guard = value ? value : 0xC1A04B37u;
    seeded = true;
}

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void)
{
    panic("stack protector: task=%s", g_current ? g_current->name : "boot");
}
