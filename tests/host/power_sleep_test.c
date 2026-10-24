/* Production reversible device transaction; failure at every boundary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
static char order[32];
static unsigned used, missing;
static int fail, resume_fail;
static int step(char c, int stage) { order[used++]=c; order[used]=0; return fail==stage ? -EIO : 0; }
bool net_power_resume_ready(void) { return missing!=1; }
bool audio_power_resume_ready(void) { return missing!=2; }
bool usb_power_resume_ready(void) { return missing!=3; }
bool ata_power_resume_ready(void) { return missing!=4; }
int net_power_suspend(void) { return step('N',1); }
int audio_power_suspend(void) { return step('A',2); }
int usb_power_suspend(void) { return step('U',3); }
int ata_power_suspend(void) { return step('D',4); }
int net_power_resume(void) { step('n',0); return resume_fail==1 ? -EIO : 0; }
int audio_power_resume(void) { step('a',0); return resume_fail==2 ? -EIO : 0; }
int usb_power_resume(void) { step('u',0); return resume_fail==3 ? -EIO : 0; }
int ata_power_resume(void) { step('d',0); return resume_fail==4 ? -EIO : 0; }
int net_power_quiesce(void) { return 0; }
int audio_power_quiesce(void) { return 0; }
int usb_power_quiesce(void) { return 0; }
#include "../../src/kernel/core/power_quiesce.c"
int main(void)
{
    for (missing=1;missing<=4;missing++) {
        assert(!power_devices_resume_ready());
        assert(power_devices_suspend()==-ENOTSUP && !used);
    }
    missing=0;
    const char *failed[]={"", "N", "NAn", "NAUan", "NAUDuan"};
    for (fail=1;fail<=4;fail++) {
        used=0; assert(power_devices_suspend()==-EIO && !strcmp(order,failed[fail]));
        assert(power_devices_resume_ready());
    }
    fail=0; used=0;
    assert(!power_devices_suspend() && !strcmp(order,"NAUD"));
    assert(power_devices_suspend()==-EBUSY);
    assert(!power_devices_resume() && !strcmp(order,"NAUDduan"));
    used=0; assert(!power_devices_suspend()); resume_fail=3;
    assert(power_devices_resume()==-EIO && !strcmp(order,"NAUDduan"));
    assert(!power_devices_resume_ready() && power_devices_suspend()==-ENOTSUP);
    puts("PASS G11B device transaction: all owners required, reverse rollback at every boundary, no double suspend, failed resume disables sleep");
    return 0;
}
