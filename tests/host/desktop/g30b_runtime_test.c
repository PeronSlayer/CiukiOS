/* G30B: production boot-policy parser and scheduler/network sources.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/systools/systools.h"
#include <ciuki/system_settings.h>
#include <ciuki/files.h>
#include "../../../apps/systools/sysreq.h"
#include <ciuki/kernel.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct ciuki_boot_info g_boot;
static struct vfs settings_volume;
static const char *settings_perf, *settings_hw;
static int settings_fd, settings_tables, settings_fail, settings_legacy;
void klog(const char *format, ...) { (void)format; }
#include "../../../src/kernel/core/loader_progress.inc"
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ assert(v == &settings_volume && drive == 2 && !settings_tables); memset(t,0,sizeof(*t)); settings_tables++; return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; assert(settings_tables == 1 && !settings_fd); settings_tables--; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{
    (void)t; (void)share; (void)attr; assert(flags == VFS_READ && !settings_fd);
    if (!strcmp(path, "C:\\SYSTEM\\SETTINGS\\performance.cfg")) { if (!settings_perf) return -FS_ENOENT; settings_fd = 3; }
    else if (!strcmp(path, "C:\\SYSTEM\\HWPROF.CFG")) { if (!settings_hw || settings_legacy) return -FS_ENOENT; settings_fd = 4; }
    else { assert(!strcmp(path, "C:\\SYSTEM\\SETTINGS\\hwprofiles.cfg")); if (!settings_hw) return -FS_ENOENT; settings_fd = 4; }
    return settings_fd;
}
int vfs_read(struct vfs_table *t, int fd, void *out, size_t cap, size_t *n)
{
    (void)t; assert(fd == settings_fd); if (settings_fail) return -FS_EIO;
    const char *source = fd == 3 ? settings_perf : settings_hw;
    *n = strlen(source); if (*n > cap) *n = cap; memcpy(out, source, *n); return 0;
}
int vfs_close(struct vfs_table *t, int fd) { (void)t; assert(fd == settings_fd); settings_fd = 0; return 0; }
static struct hp_store profiles;
static char text[24576];
static void boot_settings(void)
{
    hp_init(&profiles);
    assert(!hp_copy(&profiles, 0, "Docked"));
    struct ciuki_device_record nic = {.id = 0x10058, .class_id = CIUKI_DEVICE_NETWORK, .state = CIUKI_DEVICE_READY};
    struct dc_result r;
    assert(!hp_apply(&profiles, 1, &nic, DC_DISABLE, NULL, &r));
    int n = hp_save(&profiles, text, sizeof(text)); assert(n > 0);
    system_hw_parse(text, (size_t)n, 0); assert(system_device_allowed(nic.id, "e100"));
    system_hw_parse(text, (size_t)n, 1); assert(!system_device_allowed(nic.id, "e100"));
    assert(!hp_apply(&profiles, 1, &nic, DC_ENABLE, NULL, &r));
    assert(!hp_apply(&profiles, 1, &nic, DC_UPDATE_DRIVER, "rtl8139", &r));
    n = hp_save(&profiles, text, sizeof(text)); system_hw_parse(text, (size_t)n, 1);
    assert(system_device_allowed(nic.id, "rtl8139") && system_device_allowed(nic.id, "RTL8139") && !system_device_allowed(nic.id, "e100"));
    assert(!hp_apply(&profiles, 1, &nic, DC_REMOVE, NULL, &r));
    n = hp_save(&profiles, text, sizeof(text)); system_hw_parse(text, (size_t)n, 1);
    assert(system_device_allowed(nic.id, "rtl8139")); /* Remove redetects; override survives. */
    const char bad_id[] = "1:device=fffffffff disabled=1 removed=0 driver=-\n";
    system_hw_parse(bad_id, sizeof(bad_id) - 1, 1);
    assert(system_device_allowed(nic.id, "e100"));
    system_profile_select(7); system_profile_select(8); assert(system_profile_index() == 7);
    struct system_perf p = system_perf_default(), old = p;
    const char valid[] = "version=1\nrole=1\nread_ahead=0\ngraphics=0\nvm=1\nmin_mb=32\nmax_mb=128\n";
    assert(system_perf_parse(&p, valid, sizeof(valid) - 1));
    assert(p.role == 1 && !p.read_ahead && !p.graphics && p.vm == 1 && p.min_mb == 32 && p.max_mb == 128);
    const char *bad[] = {"version=1\nmin_mb=33\n", "version=1\nvm=1\nmin_mb=128\nmax_mb=32\n", "version=1\nmax_mb=42949672960\n",
        "version=1\nvm=-1\n", "version=1\nversion=1\n", "version=2\n", "role=1\n", "version=1\ngraphics=2\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) { p = old; assert(!system_perf_parse(&p, bad[i], strlen(bad[i])) && !memcmp(&p, &old, sizeof(p))); }
    puts("PASS G30B boot profile selection, disable/driver/remove and performance validation");
}
static void settings_io(void)
{
    settings_volume.volumes[2] = (void *)1;
    settings_perf = "version=1\nrole=2\nread_ahead=0\ngraphics=0\nvm=2\n";
    hp_init(&profiles); assert(!hp_copy(&profiles, 0, "Docked"));
    struct ciuki_device_record nic = {.id=0x10058, .class_id=CIUKI_DEVICE_NETWORK, .state=CIUKI_DEVICE_READY};
    struct dc_result r; assert(!hp_apply(&profiles, 1, &nic, DC_DISABLE, NULL, &r));
    assert(hp_save(&profiles, text, sizeof(text)) > 0); settings_hw = text;
    system_profile_select(1); system_settings_load(&settings_volume);
    assert(system_performance()->role == 2 && !system_performance()->read_ahead && system_performance()->vm == 2);
    assert(!system_device_allowed(nic.id, "e100") && !settings_fd && !settings_tables);
    settings_legacy = 1; system_hw_parse("", 0, 0); system_settings_load(&settings_volume);
    assert(!system_device_allowed(nic.id, "e100") && !settings_fd && !settings_tables);
    system_boot_hw_copy(text, strlen(text), 1); settings_fail = 1;
    system_settings_load(&settings_volume); /* read error retains the loader's exact boot policy */
    assert(!system_device_allowed(nic.id, "e100") && system_performance()->graphics == 1 && !settings_fd && !settings_tables);
    settings_fail = 0; settings_perf = "version=1\nvm=9\n"; system_settings_load(&settings_volume);
    assert(system_performance()->vm == 0 && !settings_fd && !settings_tables);
    system_settings_load(NULL); assert(!settings_fd && !settings_tables);
    puts("PASS G30B settings VFS: canonical/legacy paths, exact boot choice, invalid/read-error defaults and balanced cleanup");
}
static void early_handoff(void)
{
    static uint8_t loader[0x1e000];
    memset(loader, 0, sizeof(loader));
    uint8_t *trail = loader + 24544; memcpy(trail, "CLDRPRG1", 8); memcpy(trail + 24, "HP01", 4);
    trail[28] = 1; trail[29] = 2;
    size_t n = strlen(text); assert(n && n < 24576); trail[30]=(uint8_t)n; trail[31]=(uint8_t)(n>>8);
    memcpy(loader + 0x18000, text, n); g_boot.loader_start=0x20000; g_boot.loader_end=0x20000+sizeof(loader);
    system_hw_parse("",0,0); system_profile_select(0); loader_progress_report(trail,32);
    assert(system_profile_index()==1 && !system_device_allowed(0x10058,"e100"));
    memset(loader + 0x18000,0xcc,n); assert(!system_device_allowed(0x10058,"e100")); /* retained values, no pointer */
    system_hw_parse("",0,0); system_profile_select(0); g_boot.loader_end=g_boot.loader_start+0x18000+(uint32_t)n-1;
    loader_progress_report(trail,32); assert(system_profile_index()==0 && system_device_allowed(0x10058,"e100"));
    puts("PASS G30B early kernel handoff: extent bounds and policy copied before loader reclamation");
}
struct host { char file[SS_TEXT]; int64_t now; unsigned idle; int ac, result, done, launches, stops, save_error; };
static struct host f;
static struct ss_service svc;
static int64_t now(void *c) { return ((struct host *)c)->now; }
static int load(void *c, char *out, size_t cap, size_t *n) { struct host *h=c; *n=strlen(h->file); assert(*n<cap); memcpy(out,h->file,*n); return 0; }
static int save(void *c, const char *in, size_t n) { struct host *h=c; if (h->save_error) return -ENOSPC; memcpy(h->file,in,n); h->file[n]=0; return 0; }
static int launch(void *c, const char *p, const char *a) { struct host *h=c; assert(!strcmp(p,"calc") && !*a); h->launches++; return 17; }
static void conditions(void *c, struct st_conditions *v) { struct host *h=c; v->idle_minutes=h->idle; v->ac=h->ac; }
static int result(void *c, int pid, int *out) { struct host *h=c; assert(pid==17); *out=h->result; return h->done; }
static void stop(void *c, int pid) { assert(pid==17); ((struct host *)c)->stops++; }
static void scheduler(void)
{
    memset(&f,0,sizeof(f)); memset(&svc,0,sizeof(svc)); f.now=10000; f.ac=2;
    struct st_store s; st_init(&s);
    struct st_task task={.name="test",.command="calc",.kind=ST_AT_START,.created=1,.last_run=-1,
        .conditions=ST_IDLE|ST_AC_ONLY|ST_STOP_IDLE|ST_STOP_BATTERY,.idle_minutes=5};
    assert(!st_add(&s,&task)); assert(st_save(&s,f.file,sizeof(f.file))>0);
    struct ss_host h={&f,now,load,save,launch,conditions,result,stop,NULL};
    assert(!ss_poll(&svc,&h) && !f.launches); f.idle=5;
    assert(!ss_poll(&svc,&h) && !f.launches); f.ac=0;
    assert(!ss_poll(&svc,&h) && !f.launches); f.ac=1;
    assert(ss_poll(&svc,&h)==1 && svc.store.t[0].last_result==ST_RUNNING);
    assert(!ss_poll(&svc,&h) && f.launches==1);
    f.idle=0; assert(!ss_poll(&svc,&h) && f.stops==1);
    assert(!ss_poll(&svc,&h) && f.stops==1); /* one termination request */
    f.done=1; f.result=23; f.save_error=1;
    assert(ss_poll(&svc,&h)==-ENOSPC && svc.store.t[0].last_result==23);
    f.save_error=0; assert(!ss_poll(&svc,&h));
    struct st_store check; st_load(&check,f.file);
    assert(check.t[0].last_result==23 && check.t[0].conditions==task.conditions && check.t[0].idle_minutes==5);
    memset(&svc,0,sizeof(svc)); f.done=0; f.idle=5;
    assert(ss_poll(&svc,&h)==1); f.ac=0;
    assert(!ss_poll(&svc,&h) && f.stops==2);
    /* A service restart cannot claim a previous instance is still running. */
    memset(&svc,0,sizeof(svc)); assert(!ss_poll(&svc,&h));
    assert(svc.store.t[0].last_result==-EINTR);
    puts("PASS G30B idle/AC admission, deferred startup, running/exit results, stop conditions, save retry and restart recovery");
}
static int manual_request(void *ctx, char name[32]) { (void)ctx; strcpy(name, "test"); return 1; }
static void deadline_manual(void)
{
    memset(&f, 0, sizeof(f)); memset(&svc, 0, sizeof(svc)); f.now = 500; f.ac = 1;
    struct st_store s; st_init(&s);
    struct st_task t = {.name="test", .command="calc", .kind=ST_AT_START, .created=1, .last_run=-1,
        .conditions=ST_IDLE, .idle_minutes=5, .idle_deadline=10};
    assert(!st_add(&s, &t)); assert(st_save(&s, f.file, sizeof(f.file)) > 0);
    struct ss_host h={&f,now,load,save,launch,conditions,result,stop,NULL};
    assert(!ss_poll(&svc, &h)); f.now += 9; assert(!ss_poll(&svc, &h)); f.now++;
    assert(!ss_poll(&svc, &h) && !f.launches && svc.store.t[0].last_result == -ETIMEDOUT);
    h.manual = manual_request;
    assert(ss_poll(&svc, &h) == 1 && f.launches == 1 && svc.store.t[0].last_result == ST_RUNNING);
    f.done = 1; f.result = 27; h.manual = NULL;
    assert(!ss_poll(&svc, &h) && svc.store.t[0].last_result == 27);
    puts("PASS G30B idle deadline, explicit Run bypass and actual last result");
}
static void network(void)
{
    struct sm_source source={0}; struct sm_raw raw={0};
    struct ciuki_net_control net[2] = {{.size=sizeof(net[0]), .version=1, .generation=1}, {.size=sizeof(net[0]), .version=1, .generation=2}};
    net[0].rx_errors=10; net[1].tx_errors=20;
    sm_network_sample(&raw,&source,net,2);
    assert(raw.net_errors==0 && raw.have&(1u<<SM_NET_ERRORS));
    net[0].rx_errors+=3; net[1].tx_errors+=5; net[0].rx_packets+=100; net[1].rx_packets+=200;
    sm_network_sample(&raw,&source,net,2); assert(raw.net_errors==8 && raw.net_rx_packets==300);
    net[0].generation++; net[0].rx_errors=0; net[0].rx_packets=0;
    sm_network_sample(&raw,&source,net,1); assert(raw.net_errors==8 && raw.net_rx_packets==300);
    sm_network_sample(&raw,&source,NULL,0); assert(!(raw.have&(1u<<SM_NET_ERRORS)));
    sm_network_sample(&raw,&source,net,2); assert(raw.net_errors==8); /* reconnect primes */
    struct sm_monitor m; sm_init(&m); assert(!sm_add(&m,SM_NET_ERRORS,&raw)); raw.total_ticks=100;raw.tick_hz=100;
    assert(!sm_sample(&m,&raw,NULL,0,NULL)); raw.total_ticks+=200;raw.net_errors+=6;
    assert(!sm_sample(&m,&raw,NULL,0,NULL) && m.items[1].value==3);
    puts("PASS G30B multi-adapter counters, reset, unavailable/reconnect and error rates");
}
static void manual_message(void)
{
    struct ciuki_message m; struct desk_packet p;
    assert(!sysreq_task_message(&m, 1234, "conditional"));
    assert(desk_parse(&m, 1, NULL, &p) && p.opcode == DESK_LAUNCH && p.serial == 1234 && !strcmp(p.title, "schedtask") && !strcmp(p.argument, "run:conditional"));
    assert(sysreq_task_message(&m, 0, "conditional") == -EINVAL && sysreq_task_message(&m, 1, "bad\nname") == -EINVAL);
    puts("PASS G30B manual Run production message codec and parser");
}
int main(void) { manual_message(); setvbuf(stdout, NULL, _IONBF, 0); boot_settings(); settings_io(); early_handoff(); scheduler(); deadline_manual(); network(); return 0; }
