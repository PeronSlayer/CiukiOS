/* The interactive fault observer must never return to the evidence overlay.
 * Production observer; only scheduler/bootstrap/record endpoints are adapted.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../../src/kernel/probes/f5_probes_observers.c"

struct ciuki_boot_info g_boot;
static jmp_buf slept;
static unsigned bootstraps, sleeps, ready, refused;
int probes_w32_core_fixture(unsigned fixture) { (void)fixture; return 0; }
void supervisor_bootstrap(void) { bootstraps++; }
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    (void)fmt;
    assert(!strcmp(probe,"q3-lease-eio"));
    if (!strcmp(event,"READY")) ready++;
    else { assert(!strcmp(event,"FAIL")); refused++; }
}
void task_sleep_ms(uint32_t ms)
{
    assert(ms && ms<=60000 && bootstraps==1 && ready==1);
    if (++sleeps==3) longjmp(slept,1);
}
int main(void)
{
    assert(probe_q3_lease_eio()==1 && refused==1 && !bootstraps && !sleeps);
    strcpy(g_boot.test_request,"f5:q3-lease-eio run=1234abcd");
    g_boot.test_request_len=strlen(g_boot.test_request);
    g_boot.flags=CBI_F_SMBIOS_QEMU|CBI_F_TEST_REQUEST;
    if (!setjmp(slept)) {
        (void)probe_q3_lease_eio();
        assert(!"interactive observer returned to the evidence slideshow");
    }
    assert(sleeps==3 && bootstraps==1 && ready==1);
    puts("PASS fx-q3hang interactive observer: QEMU-only admission, bootstrap once, READY once, sleeping continuation");
    return 0;
}
