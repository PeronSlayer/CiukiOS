/* Production launch, virtual COM/LPT and printer-file service; synthetic VFS.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/lpt.h>
#include <ciuki/game_ports_settings.h>
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t read_eflags(void) { return 0x200; }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/files.h>
#include "../../src/kernel/vm/dosvm_internal.h"
volatile uint64_t g_ticks;
static struct dosvm vm;
static uint8_t memory[0x1000];
static char ini[2][GAME_PORTS_TEXT_MAX],printed[64];
static size_t ini_size[2],printed_size;
static bool opened[3],missing_com,missing_lpt,lpt_owned,file_missing,short_write,commit_failed;
static unsigned commits,closes;
void klog(const char *f,...) { (void)f; }
void kpreempt_point(void) {}
void task_sleep_ms(uint32_t n) { g_ticks+=n; }
uint64_t deadline_after_ms(uint32_t n) { return g_ticks+n; }
bool deadline_passed(uint64_t d) { return g_ticks>=d; }
struct dosvm *dosvm_lookup(struct dosvm_key k) { return k.id==1 && k.generation==42 ? &vm : NULL; }
bool dosvm_valid(struct dosvm_key k) { return dosvm_lookup(k)!=NULL; }
bool dosvm_irq_raise(struct dosvm_key k,unsigned irq) { (void)k;(void)irq;return 0; }
int dosvm_write(struct dosvm *v,uint32_t a,const void *p,unsigned n)
{ assert(v==&vm && a+n<=sizeof(memory));memcpy(memory+a,p,n);return 0; }
int dosvm_read(struct dosvm *v,uint32_t a,void *p,unsigned n)
{ assert(v==&vm && a+n<=sizeof(memory));memcpy(p,memory+a,n);return 0; }
int dosvm_port_register(struct dosvm_key k,uint16_t a,uint16_t b,dosvm_port_handler h,void *p)
{ (void)k;(void)a;(void)b;(void)h;(void)p;return 0; }
#include "dosvm_uart_seams.inc"
bool com_snapshot(unsigned i,struct ciuki_com_status *s,const char **why)
{ (void)why;if (i || missing_com) return false;*s=(struct ciuki_com_status){.number=1,.base=0x3f8,.flags=CIUKI_COM_READY};return true; }
bool lpt_snapshot(unsigned i,struct ciuki_lpt_status *s,const char **why)
{ (void)why;if (i || missing_lpt) return false;*s=(struct ciuki_lpt_status){.base=0x378,.flags=CIUKI_LPT_READY,.status=0xd8};return true; }
int lpt_open(unsigned i,uint32_t id,uint32_t gen,bool raw,uint32_t *g)
{ (void)i;(void)id;(void)gen;(void)raw;if (lpt_owned) return -EBUSY;lpt_owned=true;*g=7;return 0; }
void lpt_close(unsigned i,uint32_t g) { (void)i;assert(g==7);lpt_owned=false; }
int lpt_raw_io(unsigned i,uint32_t g,unsigned offset,bool write,uint8_t *b)
{ (void)i;(void)g;(void)offset;(void)write;*b=0x55;return 0; }
int lpt_transfer(unsigned i,uint32_t g,void *b,unsigned n,bool w,bool nb,bool (*cancel)(void *),void *ctx)
{ (void)i;(void)g;(void)b;(void)w;(void)nb;assert(!cancel(ctx));return (int)n; }
int lpt_control(unsigned i,uint32_t g,uint32_t op,void *p,unsigned n,bool w)
{ (void)i;(void)g;(void)n;(void)w;if (op==LPT_GET_STATUS) ((struct ciuki_lpt_status *)p)->status=0xd8;return 0; }
int px_error(int e) { return e ? -EIO : 0; }
int vfs_open(struct vfs_table *t,const char *p,unsigned f,enum vfs_share share,uint8_t attr)
{
    (void)t;(void)share;(void)attr;
    int fd=strstr(p,"ports.0.ini") ? 0 : strstr(p,"ports.1.ini") ? 1 : 2;
    if (fd<2 && !ini_size[fd]) return -FS_ENOENT;
    if (fd==2) { assert(!strcmp(p,"C:\\Documents\\print.prn") && f==(VFS_WRITE|VFS_CREATE));if (file_missing) return -FS_ENOENT; }
    assert(!opened[fd]);opened[fd]=true;return fd;
}
int vfs_read(struct vfs_table *t,int fd,void *p,size_t n,size_t *done)
{ (void)t;assert(fd<2 && opened[fd]);assert(ini_size[fd]<=n);memcpy(p,ini[fd],ini_size[fd]);*done=ini_size[fd];return 0; }
int vfs_write(struct vfs_table *t,int fd,const void *p,size_t n,size_t *done)
{ (void)t;assert(fd==2 && opened[fd] && n==1 && printed_size<sizeof(printed));*done=short_write ? 0 : n;if (*done) printed[printed_size++]=*(const char *)p;return 0; }
int vfs_seek(struct vfs_table *t,int fd,int64_t offset,enum vfs_whence w,uint64_t *position)
{ (void)t;assert(fd==2 && opened[fd] && !offset && w==VFS_SEEK_END);*position=printed_size;return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{ (void)t;assert(fd==2 && opened[fd]);commits++;return commit_failed ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t,int fd)
{ (void)t;assert(opened[fd]);opened[fd]=false;closes++;return 0; }
#include "../../src/kernel/vm/uart16550.c"
#include "../../src/kernel/vm/dosvm_uart.c"
#include "../../src/kernel/vm/dosvm_lpt.c"
#include "../../src/kernel/vm/dosvm_game_ports.c"
static void reset(void)
{
    assert(!opened[0] && !opened[1] && !opened[2]);memset(&vm,0,sizeof(vm));memset(memory,0,sizeof(memory));
    vm.info.key=(struct dosvm_key){1,42};vm.game=1;vm.table_ready=true;
    uint16_t equipment=0x22;memcpy(memory+0x410,&equipment,2);
    assert(!dosvm_uart_init(&vm));assert(!dosvm_lpt_init(&vm));
}
static uint16_t word(unsigned at) { uint16_t x;memcpy(&x,memory+at,2);return x; }
static void save(struct game_ports_settings *p,unsigned seq)
{ p->sequence=seq;ini_size[seq&1]=game_ports_encode(p,ini[seq&1]); }
static void close_vm(void) { dosvm_uart_close(&vm);dosvm_lpt_stop(&vm);assert(!dosvm_game_ports_close(&vm)); }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    reset();assert(word(0x410)==0x4822);assert(!dosvm_game_ports_apply(&vm));assert(word(0x410)==0x822);close_vm();
    struct game_ports_settings p;game_ports_defaults(&p);p.com[0]=0;p.com[1]=0x3f8;p.lpt_mode=GAME_LPT_RAW;save(&p,1);
    reset();assert(!dosvm_game_ports_apply(&vm));assert(!vm.uart[0].bound && vm.uart[1].native==0 && vm.lpt[0].raw);
    assert(word(0x400)==0 && word(0x402)==0x2f8 && word(0x410)==0x4622);
    uint32_t value=0;assert(!dosvm_uart_port(&vm,0x3f8,2,false,&value,NULL) && value==0xffff);
    assert(!dosvm_uart_port(&vm,0x3f8,4,false,&value,NULL) && value==UINT32_MAX);close_vm();
    /* Busy acquisition is refused, then ordinary cleanup releases prior leases. */
    reset();host_com_owner[0]=CIUKI_COM_OWNER_NATIVE;assert(dosvm_game_ports_apply(&vm)==-EBUSY);host_com_owner[0]=0;close_vm();
    reset();lpt_owned=true;assert(dosvm_game_ports_apply(&vm)==-EBUSY);lpt_owned=false;close_vm();assert(!host_com_owner[0]);
    missing_com=missing_lpt=true;reset();assert(!dosvm_game_ports_apply(&vm));
    assert(vm.ports_changed==CIUKI_DOSVM_PORT_FALLBACK && vm.uart[1].native==-1 && word(0x408)==0);close_vm();missing_com=missing_lpt=false;
    game_ports_defaults(&p);p.lpt_mode=GAME_LPT_FILE;strcpy(p.path,"/Documents/print.prn");save(&p,2);
    reset();assert(!dosvm_game_ports_apply(&vm) && vm.printer_open && word(0x410)==0x4822);
    struct v86_frame f={0};f.tf.eax=0xaabb0041;f.tf.eflags=0x247;
    assert(!dosvm_lpt_bios(&vm,&f) && f.tf.eax==0xaabb9041 && f.tf.eflags==0x247 && printed_size==1 && printed[0]=='A');
    short_write=true;f.tf.eax=0x0042;assert(!dosvm_lpt_bios(&vm,&f) && (f.tf.eax&0x100));short_write=false;
    f.tf.eax=0x0100;assert(!dosvm_lpt_bios(&vm,&f) && printed_size==1);
    f.tf.eax=0x0200;assert(!dosvm_lpt_bios(&vm,&f) && f.tf.eax==0x9000);
    vm.info.state=DOSVM_STOPPING;f.tf.eax=0x0043;assert(!dosvm_lpt_bios(&vm,&f) && (f.tf.eax&0x100));
    assert(opened[2] && !commits);close_vm();assert(commits==1);
    reset();assert(!dosvm_game_ports_apply(&vm));f.tf.eax=0x0042;assert(!dosvm_lpt_bios(&vm,&f));close_vm();assert(printed_size==2 && printed[1]=='B');
    reset();assert(!dosvm_game_ports_apply(&vm));commit_failed=true;assert(dosvm_game_ports_close(&vm)==-EIO && !opened[2]);commit_failed=false;close_vm();
    file_missing=true;reset();assert(!dosvm_game_ports_apply(&vm) && !vm.printer_open && (vm.ports_changed&2));close_vm();file_missing=false;
    ini[0][0]^=1;reset();assert(!dosvm_game_ports_apply(&vm) && (vm.ports_changed&4) && vm.lpt[0].raw);close_vm();
    ini[1][0]^=1;reset();assert(!dosvm_game_ports_apply(&vm) && (vm.ports_changed&4) && word(0x410)==0x822);close_vm();
    assert(!opened[0] && !opened[1] && !opened[2] && !lpt_owned && !host_com_owner[0] && closes);
    puts("PASS game ports: persisted launch snapshot, BDA COM+LPT counts, none/null, missing fallbacks, busy leases, raw/printer isolation, INT17 append/error/stop/commit and corrupt slots");
}
