/* H05: literal production disk-policy marshaller and actual power gate. */
#define main sec_k04_power_runtime_unused
#include "../power_runtime_test.c"
#undef main
/* sec-k08b K8-7: fixture admission with production role/session predicates. */
#define irq_save security_fixture_irq_save
#define irq_restore security_fixture_irq_restore
#include "subject_fixture.c"
#undef irq_save
#undef irq_restore
static unsigned policy_writes;
static uint32_t requested_seconds;
static unsigned char record_memory[128];
int copy_from_user(void *out,uint32_t address,uint32_t bytes)
{ if(address!=4096 || bytes>sizeof(record_memory))return -EFAULT;memcpy(out,record_memory,bytes);return 0; }
int copy_to_user(uint32_t address,const void *in,uint32_t bytes)
{ if(address!=4096 || bytes>sizeof(record_memory))return -EFAULT;memcpy(record_memory,in,bytes);return 0; }
static int policy_queue(struct ciuki_power_policy *p)
{ int e=power_policy_apply(p);if(!e){policy_writes++;requested_seconds=p->disk_seconds;}return e; }
#include "power_record.inc"
static void disk_timer(uint32_t seconds, int expected)
{
    struct ciuki_power_policy policy={.version=CIUKI_POWER_POLICY_VERSION,.size=sizeof(policy),.disk_seconds=seconds};
    memcpy(record_memory,&policy,sizeof(policy));
    unsigned before=policy_writes;
    unsigned calls=g11_calls;
    assert(power_record_call(CIUKI_POWER_POLICY,4096,sizeof(policy))==expected);
    assert(policy_writes==before+(expected==0));
    assert(g11_calls==calls+(expected==0));
    if(!expected) {
        assert(requested_seconds==seconds && g11_seconds==seconds && disk_seconds==seconds);
        struct ciuki_power_policy result;
        memcpy(&result,record_memory,sizeof(result));
        assert(result.disks==1 && result.applied_seconds==seconds && !result.error);
    } else assert(!memcmp(record_memory,&policy,sizeof(policy)));
}
static void session_desktop(uint32_t uid, uint32_t session)
{
    desktop.pid=42;desktop.state=PROC_LIVE;
    security_fixture_identity(&desktop,uid,session,false,CIUKI_SECURITY_ROLE_DESKTOP);
    struct security_session *s=&security_sessions[0];
    *s=(struct security_session){.credential=desktop.subject->credential,.state=CIUKI_SECURITY_SESSION_ACTIVE,.refs=1};
    desktop.subject->session=s;
    role_generations[CIUKI_SECURITY_ROLE_LOGON]=1; /* disable interim PID fallback */
    assert(security_device_desktop(&desktop));
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    /* Slot zero is the supervisor; production desktop lookup starts at one. */
    static struct process reserved;
    security_fixture_identity(&reserved,0,0,true,CIUKI_SECURITY_ROLE_OWNER);
    reset();
    g11_fixed=1;
    disk_timer(300,0); /* interim Guest desktop remains supported */
    session_desktop(1001,11);disk_timer(600,0);
    session_desktop(501,12);disk_timer(1200,0);
    const uint32_t users[]={1001,501};
    for(unsigned i=0;i<ARRAY_SIZE(users);i++) {
        session_desktop(users[i],11+i);
        desktop.subject->role=0;disk_timer(900,-EPERM); /* client, even at desktop PID */
        desktop.subject->role=CIUKI_SECURITY_ROLE_DESKTOP;
        desktop.subject->live=false;disk_timer(900,-EPERM);
        desktop.subject->live=true;
        security_sessions[0].state=CIUKI_SECURITY_SESSION_CLOSING;disk_timer(900,-EPERM);
        security_sessions[0].state=CIUKI_SECURITY_SESSION_DEAD;disk_timer(900,-EPERM);
        security_sessions[0].state=CIUKI_SECURITY_SESSION_ACTIVE;
        desktop.subject->credential.session_id=0;disk_timer(900,-EPERM);
        desktop.subject=0;disk_timer(900,-EPERM);
    }
    security_fixture_identity(&desktop,500,1,true,0);desktop.pid=7;
    disk_timer(1800,0); /* administrator path remains available without desktop role */
    desktop.subject->live=false;disk_timer(900,-EPERM);
    desktop.subject->live=true;desktop.subject->session=&security_sessions[0];
    security_sessions[0].state=CIUKI_SECURITY_SESSION_CLOSING;disk_timer(900,-EPERM);
    desktop.subject=0;disk_timer(900,-EPERM);
    for(uint32_t role=CIUKI_SECURITY_ROLE_LOGON;role<=CIUKI_SECURITY_ROLE_LOGON_UI;role++) {
        security_fixture_identity(&desktop,0,0,true,role);
        disk_timer(300,0); /* SYSTEM remains admitted by the administrator predicate */
    }
    reset();desktop.pid=7;
    security_fixture_identity(&desktop,0,0,true,CIUKI_SECURITY_ROLE_LOGON_UI);
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-EPERM && !stages);
    assert(power_syscall(CIUKI_POWER_STANDBY)==-EPERM && !stages);
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQP"));
    puts("H05 power policy: Standard/Guest live desktop timers applied; non-desktop, revoked, missing identity and non-live session denied unchanged; administrator retained; logon UI coordinated Off only PASS");
    return 0;
}
