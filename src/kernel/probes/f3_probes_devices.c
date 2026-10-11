/* DOS device controller, independent host time/heartbeat and witness VM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/dosvm.h>
#include <ciuki/abi.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/storage.h>

struct devices_mail { uint16_t magic, stage, command, ticks, sti_mark, sti_seen,
    ss_seen, keys, last_key, isr0, isr1, mouse_release, callbacks, last_ip; };
static int devices_read(struct dosvm *v, struct devices_mail *m) { return dosvm_read(v, 0x600, m, sizeof(*m)); }
static int devices_command(struct dosvm *v, uint16_t n) { return dosvm_write(v, 0x604, &n, 2); }
static int devices_stage(struct dosvm *v, unsigned stage, struct devices_mail *m)
{
    uint64_t deadline = deadline_after_ms(10000);
    for (;;) {
        struct dosvm_info info; dosvm_info(v,&info);
        if (devices_read(v,m)) return -EFAULT;
        if (m->magic == 0xD301 && m->stage == stage) return 0;
        if (info.state >= DOSVM_STOPPING) return -EIO;
        if (deadline_passed(deadline)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static int devices_boot(struct dosvm *v)
{
    int e = dosvm_boot(v); if (e) return e;
    uint64_t deadline = deadline_after_ms(30000);
    for (;;) {
        struct dosvm_info info; dosvm_info(v,&info);
        const uint16_t *text = dosvm_text(v,0); bool prompt = false;
        for (unsigned i=0;i<1997;i++) if ((uint8_t)text[i]=='C' && (uint8_t)text[i+1]==':' && (uint8_t)text[i+2]=='\\')
            for(unsigned j=i+3;j<2000 && j<i+80;j++) if ((uint8_t)text[j]=='>') prompt=true;
        if (info.dos_ready && prompt) return 0;
        if (info.state >= DOSVM_STOPPING) return -EIO;
        if (deadline_passed(deadline)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static struct task *devices_heartbeat(uint32_t *data)
{
    struct task *t=task_create_user("devices-heartbeat",P_NORMAL,0x400000,0x800000,0,0,0);
    if (!t) return 0;
    const uint8_t code[]={0xFF,0x05,0,0,0x50,0,0xEB,0xF8};
    for (unsigned i=0;i<3;i++) {
        uint32_t p=pmm_alloc(); if (!p) goto fail;
        memset(P2V(p),0,PAGE_SIZE);
        if (as_map(&t->as,i==0?0x400000:i==1?0x500000:0x7FF000,p,PTE_U|(i?PTE_W:0))) { pmm_free(p); goto fail; }
        if (!i) memcpy(P2V(p),code,sizeof(code));
        if (i==1) *data=p;
    }
    task_start(t); return t;
fail:
    task_kill(t,0); task_reap(t); return 0;
}
static bool devices_key(struct dosvm_key k, unsigned code, int value)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=(int32_t)code,.value=value};
    return dosvm_input_event(k,&e);
}
static int probe_f3_devices(void)
{
    rec_emit("vm-devices","BEGIN","version=1");
    struct dosvm *v=0,*w=0; struct task *native=0; uint32_t heartbeat=0;
    struct devices_mail m={0}; struct dosvm_device_info before={0},after={0};
    struct dosvm_ledger baseline, final; dosvm_snapshot(&baseline);
    const struct dosvm_config config={.commitment=DOSVM_LOW_CAP,.probe="vm-devices"};
    int error=storage_enable_write(storage_get(),2);
    if (error) goto cleanup;
    native=devices_heartbeat(&heartbeat); if (!native) { error=-ENOMEM; goto cleanup; }
    if ((error=dosvm_create(&config,&w)) || (error=devices_boot(w)) ||
        (error=dosvm_create(&config,&v)) || (error=devices_boot(v))) goto cleanup;
    uint32_t sentinel=0xA17E5AFE, observed=0;
    if ((error=dosvm_write(w,0x180,&sentinel,4))) goto cleanup;
    struct dosvm_key key=dosvm_key(v);
    const char *command="\\VMDEV.COM\r"; /* the shell starts in C:\APPS (f3-03 finding) */
    while (*command) if (!dosvm_keyboard(key,(uint8_t)*command++)) { error=-ENOSPC; goto cleanup; }
    if ((error=devices_stage(v,1,&m))) goto cleanup;
    uint64_t cli_start=g_ticks; uint32_t native_before=*(volatile uint32_t *)P2V(heartbeat);
    dosvm_device_info(key,&before); task_sleep_ms(100); dosvm_device_info(key,&after);
    devices_read(v,&m);
    bool cli=m.ticks==0 && after.pit_edges>before.pit_edges && *(volatile uint32_t *)P2V(heartbeat)!=native_before;
    rec_emit("vm-devices","DATA","case=cli preemptible=%u host_ticks=%llu pit_edges=%llu pending=%u guest_ticks=%u",
        cli,g_ticks-cli_start,after.pit_edges-before.pit_edges,!!(after.irr[0]&1),m.ticks);
    if (!cli) { error=-EFAULT; goto cleanup; }
    uint64_t start=g_ticks;
    if ((error=devices_command(v,1)) || (error=devices_stage(v,3,&m))) goto cleanup;
    uint64_t elapsed=g_ticks-start;
    bool timer=m.ticks>=64 && elapsed>=40 && elapsed<=250 && m.sti_seen==1 && m.isr0==1;
    rec_emit("vm-devices","DATA","case=timer ticks=%u host_ticks=%llu sti_shadow=%u isr0=%u cap=64",m.ticks,elapsed,m.sti_seen==1,m.isr0);
    if (!timer) { error=-EFAULT; goto cleanup; }
    uint16_t stopped_ticks=m.ticks;
    dosvm_irq_raise(key,0);
    if ((error=devices_command(v,3)) || (error=devices_stage(v,4,&m))) goto cleanup;
    dosvm_device_info(key,&after);
    bool ss=m.ss_seen==stopped_ticks && after.imr[0]==0xFF && (after.irr[0]&1);
    rec_emit("vm-devices","DATA","case=shadow mov_ss=%u masked_pending=%u ticks=%u ss_seen=%u last_irq_ip=%04x",ss,!!(after.irr[0]&1),m.ticks,m.ss_seen,m.last_ip);
    if (!ss || !dosvm_input_focus(key,true) || !devices_key(key,0x1E,1) || !devices_key(key,0x1E,2)) { error=-EFAULT; goto cleanup; }
    if ((error=devices_command(v,4)) || (error=devices_stage(v,6,&m))) goto cleanup;
    bool keyboard=m.keys>=2 && m.last_key==0x1E && m.isr1==2;
    struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=10,.value2=10};
    struct ciuki_input_event button={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
    if (!dosvm_input_event(key,&motion)) { error=-EFAULT; goto cleanup; }
    motion.value=27; motion.value2=18;
    if (!dosvm_input_event(key,&motion) || !dosvm_input_event(key,&button)) { error=-EFAULT; goto cleanup; }
    task_sleep_ms(20);
    dosvm_device_info(key,&after);
    bool held=after.held_keys==1 && after.buttons==1;
    if (!dosvm_input_focus(key,false)) { error=-EFAULT; goto cleanup; }
    dosvm_device_info(key,&after);
    if ((error=devices_stage(v,7,&m))) goto cleanup;
    bool input=keyboard && held && !after.held_keys && !after.buttons && m.last_key==0x9E && m.mouse_release==1 && m.callbacks>=1;
    rec_emit("vm-devices","DATA","case=input keyboard=%u hlt_wake=%u focus_release=%u callbacks=%u keys=%u held=%u buttons=%u",
        keyboard,keyboard,input,m.callbacks,m.keys,after.held_keys,after.buttons);
    uint64_t until=deadline_after_ms(5000); struct dosvm_info info;
    do { dosvm_info(v,&info); if (info.state>=DOSVM_STOPPING) break; task_sleep_ms(1); } while (!deadline_passed(until));
    struct dosvm_info witness; dosvm_info(w,&witness);
    bool locality=info.state==DOSVM_STOPPING && info.reason==DOSVM_FAULT && info.fault_vector==13 &&
        witness.state<DOSVM_STOPPING && !dosvm_read(w,0x180,&observed,4) && observed==sentinel;
    rec_emit("vm-devices","DATA","case=locality malformed_io=%u witness=%u heartbeat=%u",locality,witness.state<DOSVM_STOPPING,*(volatile uint32_t *)P2V(heartbeat)!=native_before);
    if (!input || !locality) error=-EFAULT;
cleanup:
    if (v) { int e=dosvm_destroy(dosvm_key(v),true); if (!error) error=e; }
    if (w) { int e=dosvm_destroy(dosvm_key(w),true); if (!error) error=e; }
    if (native) { task_kill(native,0); task_reap(native); }
    dosvm_snapshot(&final);
    bool restored=!memcmp(&baseline,&final,sizeof(final));
    rec_emit("vm-devices","DATA","case=cleanup restored=%u vms=%u tasks=%u pages=%u handles=%u devices=%u",restored,final.vms,final.tasks,final.pages,final.handles,final.devices);
    if (!restored && !error) error=-ENOMEM;
    rec_emit("vm-devices","END","status=%s error=%d",error?"FAIL":"PASS",error);
    return error;
}
CIUKI_F3_PROBE("vm-devices",probe_f3_devices);
