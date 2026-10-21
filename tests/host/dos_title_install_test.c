/* Production installer controller and complete tree hashing with synthetic VFS.
 * No guest execution or device access. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
#include "../../src/kernel/probes/f3_probes_titles.c"
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
static struct dosvm vm;
static struct dos_file_volume volume;
static const struct { const char *path, *data; bool dir; } nodes[]={
    {"Z.TXT","last",false}, {"DIR","",true}, {"DIR/A.DAT","first",false}
};
static unsigned opens,closes,edges,records;
static bool short_read,read_error,close_error,timeout,bad_exit,refuse_key,extra_file;
static uint32_t position;
static char install_record[512];
int ksnprintf(char *buf,size_t n,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int r=vsnprintf(buf,n,fmt,ap);va_end(ap);return r; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
bool deadline_passed(uint64_t until) { return g_ticks>=until; }
void task_sleep_ms(uint32_t ms) { g_ticks+=ms; }
static int next_node(struct vfs_find *f,struct fat_entry *entry)
{
    while (f->cursor<ARRAY_SIZE(nodes)+(extra_file ? 1u : 0u)) {
        unsigned i=f->cursor++;
        const char *path=i==ARRAY_SIZE(nodes) ? "EXTRA.TXT" : nodes[i].path;
        const char *name=path;
        if (f->directory) {
            if (strncmp(path,"DIR/",4)) continue;
            name=path+4;
        } else if (strchr(path,'/')) continue;
        memset(entry,0,sizeof(*entry));
        snprintf(entry->name,sizeof(entry->name),"%s",name);
        entry->attr=(i<ARRAY_SIZE(nodes) && nodes[i].dir) ? FAT_ATTR_DIR : FAT_ATTR_HIDDEN;
        entry->size=i==ARRAY_SIZE(nodes) ? 1u : strlen(nodes[i].data);
        return 0;
    }
    return -FS_ENOENT;
}
int vfs_find_first(struct vfs_table *t,const char *path,uint8_t mask,struct vfs_find *f,struct fat_entry *entry)
{
    (void)t;assert(mask==0x37);
    assert(!strcmp(path,"C:\\SYNTH\\*") || !strcmp(path,"C:\\SYNTH\\DIR\\*"));
    memset(f,0,sizeof(*f));f->directory=strstr(path,"\\DIR\\")!=NULL;
    return next_node(f,entry);
}
int vfs_find_next(struct vfs_table *t,struct vfs_find *f,struct fat_entry *entry)
{ (void)t;return next_node(f,entry); }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    (void)t;assert(flags==VFS_READ && share==VFS_DENY_WRITE && !attr);
    char relative[101];snprintf(relative,sizeof(relative),"%s",path+9);
    for (unsigned j=0;relative[j];j++) if (relative[j]=='\\') relative[j]='/';
    for (unsigned i=0;i<ARRAY_SIZE(nodes);i++) if (!strcmp(relative,nodes[i].path)) {
        opens++;position=0;return (int)i+1;
    }
    if (!strcmp(relative,"EXTRA.TXT")) { opens++;position=0;return 4; }
    return -FS_ENOENT;
}
int vfs_read(struct vfs_table *t,int fd,void *buf,size_t n,size_t *done)
{
    (void)t;assert(fd>0 && fd<=4);
    if (read_error) return -EIO;
    const char *data=fd==4 ? "x" : nodes[fd-1].data;
    *done=short_read ? n-1 : n;memcpy(buf,data+position,*done);position+=*done;return 0;
}
int vfs_close(struct vfs_table *t,int fd)
{ (void)t;assert(fd>0);closes++;return close_error ? -EIO : 0; }
struct dosvm_key dosvm_key(const struct dosvm *v) { (void)v;return (struct dosvm_key){1,1}; }
bool dosvm_keyboard(struct dosvm_key key,uint16_t c) { (void)key;(void)c;return true; }
bool dosvm_valid(struct dosvm_key key) { (void)key;return true; }
void dosvm_expect(struct dosvm *v,const char *name) { (void)v;assert(!strcmp(name,"RUNREC")); }
void dosvm_info(const struct dosvm *v,struct dosvm_info *info)
{
    (void)v;memset(info,0,sizeof(*info));info->state=DOSVM_RUNNABLE;
    info->verdict_done=!timeout && g_ticks>=500;info->verdict_pass=!bad_exit;
}
bool dosvm_input_event(struct dosvm_key key,const struct ciuki_input_event *e)
{ (void)key;assert(e->code==28);edges++;return !refuse_key; }
const uint16_t *dosvm_text(const struct dosvm *v,uint32_t *sequence)
{ (void)v;(void)sequence;static const uint16_t cells[2000]={'C',':','\\','>'};return cells; }
void rec_emit(const char *probe,const char *event,const char *fmt,...)
{
    assert(!strcmp(probe,"title") && !strcmp(event,"DATA"));records++;
    va_list ap;va_start(ap,fmt);vsnprintf(install_record,sizeof(install_record),fmt,ap);va_end(ap);
}
static void reset(void)
{
    g_ticks=0;opens=closes=edges=records=0;
    short_read=read_error=close_error=timeout=bad_exit=refuse_key=extra_file=false;
    vm.file_volume=&volume;
}
int main(int argc,char **argv)
{
    assert(argc==2);reset();char digest[65];unsigned files;
    assert(!title_tree_hash(&vm,"SYNTH",digest,&files,1000));
    assert(files==2 && !strcmp(digest,argv[1]) && opens==closes && opens==2);
    extra_file=true;assert(!title_tree_hash(&vm,"SYNTH",digest,&files,1000));
    assert(files==3 && strcmp(digest,argv[1]));
    reset();short_read=true;assert(title_tree_hash(&vm,"SYNTH",digest,&files,1000)==-EIO && opens==closes);
    reset();read_error=true;assert(title_tree_hash(&vm,"SYNTH",digest,&files,1000)==-EIO && opens==closes);
    reset();close_error=true;assert(title_tree_hash(&vm,"SYNTH",digest,&files,1000)==-EIO && opens==closes);
    reset();assert(title_tree_hash(&vm,"SYNTH",digest,&files,0)==-V86_ETIMEDOUT);
    reset();vm.file_volume=0;assert(title_tree_hash(&vm,"SYNTH",digest,&files,1000)==-EIO);
    reset();memset(&title_plan,0,sizeof(title_plan));title_plan.stages=1;
    struct d53_stage *s=&title_plan.stage[0];s->timeout=1000;s->count=2;
    strcpy(s->command,"\\RUNREC.COM \\SYNTH SETUP.EXE");strcpy(s->target,"SYNTH");strcpy(s->tree,argv[1]);
    s->input[0].at=100;s->input[0].edge=28|256;s->input[1].at=180;s->input[1].edge=28;
    assert(!title_install(&vm,"synthetic") && records==1 && edges==2 && opens==closes);
    assert(strstr(install_record,"rc=0 files=2 tree_sha256=") && strstr(install_record,argv[1]));
    reset();timeout=true;assert(title_install(&vm,"synthetic")==-V86_ETIMEDOUT && records==1);
    reset();bad_exit=true;assert(title_install(&vm,"synthetic")==-EIO && !opens);
    reset();refuse_key=true;assert(title_install(&vm,"synthetic")==-EIO && !opens);
    reset();s->tree[0]='0';assert(title_install(&vm,"synthetic")==-EIO && opens==closes);
    puts("PASS: production installer keys/timeout/exit and complete VFS tree digest/error cleanup");return 0;
}
