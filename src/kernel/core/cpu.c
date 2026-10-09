/* CPUID feature detection.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>

bool g_cpu_fxsr, g_cpu_sse, g_cpu_pse, g_cpu_pge, g_cpu_tsc;
char g_cpu_vendor[13];
uint32_t g_cpu_signature;

void cpu_detect(void)
{
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(g_cpu_vendor, &b, 4);
    memcpy(g_cpu_vendor + 4, &d, 4);
    memcpy(g_cpu_vendor + 8, &c, 4);
    g_cpu_vendor[12] = 0;
    if (a < 1)
        return;
    cpuid(1, &a, &b, &c, &d);
    g_cpu_signature = a;
    g_cpu_tsc = d & (1u << 4);
    g_cpu_pse = d & (1u << 3);
    g_cpu_pge = d & (1u << 13);
    g_cpu_fxsr = d & (1u << 24);
    g_cpu_sse = g_cpu_fxsr && (d & (1u << 25));
}
