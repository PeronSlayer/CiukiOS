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
static bool capture_mode, short_write, commit_error, incomplete_frame;
static unsigned capture_bytes[2], commits, tables_destroyed;
static char capture_paths[2][24];
static struct storage capture_storage;
struct storage *storage_get(void) { return &capture_storage; }
int vfs_table_init(struct vfs *fs,struct vfs_table *table,unsigned drive)
{ (void)fs;(void)table;assert(drive==2);return 0; }
void vfs_table_destroy(struct vfs_table *table) { (void)table;tables_destroyed++; }
int vfs_commit(struct vfs_table *table,int fd)
{ (void)table;(void)fd;commits++;return commit_error ? -EIO : 0; }
int vfs_write(struct vfs_table *table,int fd,const void *data,size_t n,size_t *done)
{
    (void)table;assert(capture_mode && fd>=1 && fd<=2);
    unsigned slot=fd-1;
    if (!slot && capture_bytes[slot]) {
        const uint8_t *rgb=data;
        assert(n==640*3 && rgb[0]==0x12 && rgb[1]==0x34 && rgb[2]==0x56);
    }
    *done=short_write ? n-1 : n;capture_bytes[slot]+=*done;return 0;
}
int dosvm_frame_capture(struct dosvm *v,uint64_t tick,struct dosvm_frame *out,
                        int (*row)(void *,unsigned,const void *,unsigned),void *arg)
{
    (void)v;assert(!tick);static uint32_t pixels[640];
    for (unsigned i=0;i<640;i++) pixels[i]=0x123456;
    for (unsigned y=0;y<480;y++) {
        int error=row(arg,y,pixels,sizeof(pixels));if (error) return error;
    }
    memset(out,0,sizeof(*out));return incomplete_frame ? -EAGAIN : 0;
}
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
    (void)t;
    if (capture_mode) {
        assert(flags==(VFS_WRITE|VFS_CREATE|VFS_TRUNCATE) && share==VFS_DENY_ALL && !attr);
        assert(opens<2);snprintf(capture_paths[opens],24,"%s",path);return ++opens;
    }
    assert(flags==VFS_READ && share==VFS_DENY_WRITE && !attr);
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
    capture_mode=short_write=commit_error=incomplete_frame=false;
    capture_bytes[0]=capture_bytes[1]=commits=tables_destroyed=0;
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
    assert(!title_install(&vm,"synthetic") && records==2 && edges==2 && opens==closes);
    assert(strstr(install_record,"case=install-tree stage=0 sha256=") && strstr(install_record,argv[1]));
    reset();timeout=true;assert(title_install(&vm,"synthetic")==-V86_ETIMEDOUT && records==2);
    reset();bad_exit=true;assert(title_install(&vm,"synthetic")==-EIO && !opens);
    reset();refuse_key=true;assert(title_install(&vm,"synthetic")==-EIO && !opens);
    reset();s->tree[0]='0';assert(title_install(&vm,"synthetic")==-EIO && opens==closes);
    struct dosvm_frame frame;
    reset();capture_mode=true;
    assert(!title_capture(&vm,"E:",0,&frame));
    assert(opens==2 && closes==2 && commits==2 && tables_destroyed==1);
    assert(!strcmp(capture_paths[0],"E:\\boot.PPM") && !strcmp(capture_paths[1],"E:\\boot.TXT"));
    assert(capture_bytes[0]==15+640*480*3 && capture_bytes[1]==4000);
    reset();capture_mode=short_write=true;
    assert(title_capture(&vm,"E:",1,&frame)==-EIO && opens==closes && tables_destroyed==1);
    reset();capture_mode=commit_error=true;
    assert(title_capture(&vm,"E:",1,&frame)==-EIO && opens==closes && tables_destroyed==1);
    reset();capture_mode=incomplete_frame=true;
    assert(title_capture(&vm,"E:",2,&frame)==-EAGAIN && opens==closes && tables_destroyed==1);
    puts("PASS: production installer/tree and checkpoint capture, RGB/text export, commit/error cleanup");return 0;
}
