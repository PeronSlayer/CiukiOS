/* H05: literal production machine-policy marshaller and actual power gate. */
#define main sec_k04_power_runtime_unused
#include "../power_runtime_test.c"
#undef main
static unsigned policy_writes;
static unsigned char record_memory[128];
int copy_from_user(void *out,uint32_t address,uint32_t bytes)
{ if(address!=4096 || bytes>sizeof(record_memory))return -EFAULT;memcpy(out,record_memory,bytes);return 0; }
int copy_to_user(uint32_t address,const void *in,uint32_t bytes)
{ if(address!=4096 || bytes>sizeof(record_memory))return -EFAULT;memcpy(record_memory,in,bytes);return 0; }
static int policy_queue(struct ciuki_power_policy *p)
{ int e=power_policy_check(p);if(!e)policy_writes++;return e; }
#include "power_record.inc"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);reset();
    struct ciuki_power_policy policy={.version=CIUKI_POWER_POLICY_VERSION,.size=sizeof(policy),.disk_seconds=300};
    memcpy(record_memory,&policy,sizeof(policy));
    assert(power_record_call(CIUKI_POWER_POLICY,4096,sizeof(policy))==-EPERM && !policy_writes); /* Guest desktop */
    security_fixture_identity(&desktop,1001,1,false,0);
    assert(power_record_call(CIUKI_POWER_POLICY,4096,sizeof(policy))==-EPERM && !policy_writes);
    security_fixture_identity(&desktop,500,1,true,0);desktop.pid=7;
    assert(!power_record_call(CIUKI_POWER_POLICY,4096,sizeof(policy)) && policy_writes==1);
    desktop.subject=0;
    assert(power_record_call(CIUKI_POWER_POLICY,4096,sizeof(policy))==-EPERM && policy_writes==1);
    reset();desktop.pid=7;
    security_fixture_identity(&desktop,0,0,true,CIUKI_SECURITY_ROLE_LOGON_UI);
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-EPERM && !stages);
    assert(power_syscall(CIUKI_POWER_STANDBY)==-EPERM && !stages);
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQP"));
    puts("H05 power policy: Guest/Standard desktop denied unchanged; administrator machine policy; missing identity denied; logon UI coordinated Off only PASS");
    return 0;
}
