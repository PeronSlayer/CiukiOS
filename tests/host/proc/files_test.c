/* Production paths/fds/syscalls plus the F1 fake block device and fake RAM.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main process_fixture_main
#include "proc_test.c"
#undef main
#include <ciuki/files.h>
#include <ciuki/com.h>
static uint32_t mock_com_generation;
static unsigned mock_com_closes, mock_com_reads, mock_com_writes, mock_com_controls;
bool com_snapshot(unsigned i,struct ciuki_com_status *out,const char **reason)
{
    if (i>1) return false;
    *out=(struct ciuki_com_status){.number=i+1,.base=i ? 0x2f8 : 0x3f8,
        .flags=i ? CIUKI_COM_READY : CIUKI_COM_LOGGER};
    if (reason) *reason="COM namespace fixture"; return true;
}
int com_device_open(struct file_description *d,uint32_t flags)
{
    if (flags&(O_APPEND|O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY)) return -EINVAL;
    if (!d->node->drive || mock_com_generation) return -EBUSY;
    d->com_generation=mock_com_generation=77; return 0;
}
void com_device_close(struct file_description *d)
{
    if (d->com_generation) { CHECK(d->com_generation==mock_com_generation); mock_com_generation=0; mock_com_closes++; }
}
int com_rw_user(struct file_description *d,uint32_t address,uint32_t bytes,bool write)
{
    CHECK(d->com_generation==77 && address==0x1234 && bytes==17);
    if (write) mock_com_writes++; else mock_com_reads++; return 7;
}
int com_devctl(struct process *p,int32_t fd,uint32_t op,uint32_t address,uint32_t bytes)
{
    CHECK(file_fd(p,fd)->com_generation==77 && op==COM_GET_STATUS && address==0x1234 && bytes==80);
    mock_com_controls++; return 0;
}
#include <ciuki/storage.h>
#include <ciuki/clock.h>
#include <ciuki/files_probe.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include "../fs/fake.h"
#include <stdarg.h>

extern bool f2_files_post_commit_fault(void);
void rec_emit(const char *probe, const char *event, const char *format, ...)
{
    CHECK(!strcmp(probe, "fd-table") && !strcmp(event, "DATA"));
    va_list args; va_start(args, format); vprintf(format, args); va_end(args); putchar('\n');
}

static char serial_bytes[16384], console_bytes[16384];
static unsigned serial_count, console_count;
void serial_write(const char *s, size_t n) { CHECK(serial_count + n < sizeof(serial_bytes)); memcpy(serial_bytes + serial_count, s, n); serial_count += n; }
void console_write(const char *s, size_t n) { CHECK(console_count + n < sizeof(console_bytes)); memcpy(console_bytes + console_count, s, n); console_count += n; }
void bootlog_capture(const char *s, size_t n) { (void)s; (void)n; }
bool supervisor_output(struct task *t, unsigned stream, const void *b, uint32_t n) { (void)t; (void)stream; (void)b; (void)n; return false; }
void supervisor_set_io_ops(const struct supervisor_io_ops *ops) { (void)ops; }
void grant_release(struct desktop_description *d) { (void)d; }
void channel_release(struct desktop_description *d) { (void)d; }
#include "../../../src/kernel/proc/surface.c"
#include "../../../src/kernel/proc/clock.c"
#include "../../../src/kernel/proc/posixpath.c"
#include "../../../src/kernel/proc/fdtable.c"
#include "../../../src/kernel/proc/devnodes.c"
#include "../../../src/kernel/proc/syscalls_file.c"
static bool setup_test_mode;
bool setup_mode(void) { return setup_test_mode; }
#include "../../../src/kernel/proc/setup_file.c"
#include "../../../src/kernel/proc/setup_clock.c"
static unsigned setup_rtc_writes;
static uint16_t setup_rtc_date,setup_rtc_time;
bool fs_rtc_store(uint16_t d,uint16_t t,uint8_t tenths) { CHECK(tenths<200);setup_rtc_writes++;setup_rtc_date=d;setup_rtc_time=t;return true; }
#include "../../../src/kernel/drivers/net_dev.c"
/* Register-model net suite checks the production snapshot. This boundary
 * fixture exercises the real namespace/fd/syscall/user-copy path without a NIC. */
void net_state_snapshot(struct ciuki_net_state *out)
{ *out = (struct ciuki_net_state){ .version = 1, .size = sizeof(*out), .reason = "no_adapter" }; }

static struct fake disk[2];
static struct block_cache cache;
static struct fat_volume volumes[2];
static struct vfs vfs;
static struct px_namespace *space;
static struct process *client;
static bool files_launch_verbose;
bool console_quiet(void) { return !files_launch_verbose; }
static struct proc_thread *client_thread;
#define USER (CIUKI_IMAGE_BASE + PAGE_SIZE)
#define USER2 (USER + 1024)

/* Direct host assertions obey the same namespace/admission contract as
 * native syscall callers; each wrapper releases any drive reservations. */
static int locked_seek(struct file_description *d,int64_t off,uint32_t whence,int64_t *out)
{ struct vfs *v=d->node->space->vfs; vfs_lock(v,0); int e=file_seek_locked(d,off,whence,out); vfs_unlock(v); return e; }
static int locked_truncate(struct file_description *d,uint64_t length)
{ struct vfs *v=d->node->space->vfs; vfs_lock(v,0); int e=file_truncate_locked(d,length); vfs_unlock(v); return e; }
static int locked_sync(struct file_description *d)
{ struct vfs *v=d->node->space->vfs; vfs_lock(v,0); int e=file_sync_locked(d); vfs_unlock(v); return e; }
static int locked_stat(struct px_node *n,struct ciuki_stat *out)
{ struct vfs *v=n->space->vfs; vfs_lock(v,0); int e=px_stat_locked(n,out); vfs_unlock(v); return e; }
static int locked_dents(struct file_description *d,struct ciuki_dirent *out)
{ struct vfs *v=d->node->space->vfs; vfs_lock(v,0); int e=px_getdents_locked(d,out); vfs_unlock(v); return e; }
static int locked_cwd(struct px_node *n,char out[CIUKI_PATH_MAX])
{ struct vfs *v=n->space->vfs; vfs_lock(v,0); int e=px_getcwd_locked(n,out); vfs_unlock(v); return e; }

static int resolve_path(const char *path, struct px_path *r)
{
    vfs_lock(&vfs,0);
    int err = px_resolve_locked(space, client->cwd, path, false, r);
    vfs_unlock(&vfs); return err;
}
static int make_dir(const char *path)
{
    vfs_lock(&vfs,0);
    int err = px_mkdir_locked(space, client->cwd, path, 0777);
    vfs_unlock(&vfs); return err;
}
static int rename_path(const char *from, const char *to)
{
    vfs_lock(&vfs,0);
    int err = px_rename_locked(space, client->cwd, from, to);
    vfs_unlock(&vfs); return err;
}
static int remove_path_native(const char *path, bool dir)
{
    vfs_lock(&vfs,0);
    int err = px_remove_locked(space, client->cwd, path, dir);
    vfs_unlock(&vfs); return err;
}
static int io(int fd, void *buffer, size_t bytes, bool write, bool positioned, uint64_t offset)
{
    struct file_description *d = file_fd(client, fd); CHECK(d);
    size_t done;
    vfs_lock(&vfs,0);
    int err = file_io_locked(d, buffer, bytes, write, positioned, offset, &done);
    vfs_unlock(&vfs); return done ? (int)done : err;
}
static int32_t call(uint32_t nr, uint32_t b, uint32_t c, uint32_t d, uint32_t s, uint32_t i)
{
    struct trap_frame tf = { .eax=nr, .ebx=b, .ecx=c, .edx=d, .esi=s, .edi=i };
    CHECK(file_syscall(&tf)); return (int32_t)tf.eax;
}
static void put_path(uint32_t va, const char *path) { CHECK(!ua_write(client->memory, va, path, (uint32_t)strlen(path) + 1)); }
static void all_closed(void) { for (int i = 0; i < CIUKI_OPEN_MAX; i++) if (client->fds[i].object) CHECK(!file_close(client, i)); }

static void test_paths(void)
{
    struct px_path r;
    CHECK(!resolve_path("/", &r) && r.node == space->root);
    CHECK(!resolve_path("///", &r) && r.node == space->root);
    CHECK(!resolve_path("/mnt", &r) && r.node == space->mnt);
    CHECK(!resolve_path("/mnt/d", &r) && r.node->drive == 3);
    CHECK(resolve_path("/mnt/c", &r) == -ENOENT);
    CHECK(!resolve_path("/mnt/d/..", &r) && r.node == space->mnt);
    CHECK(!resolve_path("/../../..", &r) && r.node == space->root);
    CHECK(!resolve_path("/DEV/NULL", &r) && r.node == space->null);
    CHECK(resolve_path("/dev/console/", &r) == -ENOTDIR);
    CHECK(resolve_path("missing/../SHORT.BIN", &r) == -ENOENT);
    CHECK(resolve_path("/SHORT.BIN/../x", &r) == -ENOTDIR);
    CHECK(!resolve_path("//Fixture dir///Nested.bin", &r) && r.node->entry.size == 12345);
    CHECK(!px_chdir(client, "/fixture DIR"));
    CHECK(!resolve_path("./Nested.bin", &r));
    char cwd[CIUKI_PATH_MAX]; CHECK(!locked_cwd(client->cwd, cwd) && !strcmp(cwd, "/Fixture dir"));
    CHECK(!px_chdir(client, "/"));
    const char *invalid[] = {"", "C:/SHORT.BIN", "/a\\b", "/x*", "/x?", "/x.", "/x ", "/\xc0\xaf", "/\xed\xa0\x80", "/\xf0\x9f\x90\xb6", "/\xe2\x82"};
    const int errors[] = {ENOENT, EINVAL, EINVAL, EINVAL, EINVAL, EINVAL, EINVAL, EILSEQ, EILSEQ, EILSEQ, EILSEQ};
    unsigned writes = disk[0].writes;
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) CHECK(resolve_path(invalid[i], &r) == -errors[i]);
    CHECK(disk[0].writes == writes);
    CHECK(!make_dir("/P"));
    char path[CIUKI_PATH_MAX]; strcpy(path, "/P/"); memset(path + 3, 'a', 254); path[257] = 0;
    int fd = file_open(client, path, O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0); CHECK(!file_close(client, fd));
    /* C:/P/<254 units> = 259; one further unit is too long. */
    CHECK(!resolve_path(path, &r)); path[257] = 'a'; path[258] = 0;
    CHECK(file_open(client, path, O_CREAT | O_RDWR, 0666) == -ENAMETOOLONG);
    strcpy(path, "/");
    for (unsigned i = 0; i < 255; i++) memcpy(path + 1 + i * 3, "\xe2\x94\x80", 3);
    path[766] = 0;
    fd = file_open(client, path, O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0); CHECK(!file_close(client, fd));
    CHECK(!resolve_path(path, &r)); memcpy(path + 766, "\xe2\x94\x80", 4);
    CHECK(file_open(client, path, O_CREAT | O_RDWR, 0666) == -ENAMETOOLONG);
    memset(path, '/', sizeof(path)); CHECK(px_validate(path) == -ENAMETOOLONG);
    memset(path, '/', sizeof(path)-1); path[sizeof(path)-1] = 0; CHECK(!resolve_path(path, &r));
    fd = file_open(client, "/Case.txt", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0); CHECK(!file_close(client, fd));
    CHECK(file_open(client, "/case.TXT", O_CREAT | O_EXCL | O_RDWR, 0666) == -EEXIST);
    puts("files paths: full acceptance matrix, 259/260 UCS-2 and 765/1040 UTF-8 bounds PASS");
}

static void test_fds(void)
{
    int fd = file_open(client, "/fd.bin", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0666); CHECK(fd == 0);
    CHECK(file_fcntl(client, fd, F_GETFD, 0) == FD_CLOEXEC);
    CHECK(io(fd, "abcdef", 6, true, false, 0) == 6);
    int copy = file_dup(client, fd, 0, false, 0); CHECK(copy == 1);
    CHECK(file_fd(client, copy) == file_fd(client, fd));
    CHECK(!file_fcntl(client, copy, F_GETFD, 0));
    CHECK(!file_fcntl(client, copy, F_SETFL, O_APPEND | O_NONBLOCK));
    CHECK(file_fcntl(client, fd, F_GETFL, 0) == (O_RDWR | O_APPEND | O_NONBLOCK));
    CHECK(file_fcntl(client, fd, F_SETFL, O_RDWR) == -EINVAL);
    CHECK(file_fcntl(client, fd, F_SETFD, 2) == -EINVAL);
    CHECK(file_dup(client, fd, fd, true, 0) == fd && client->fds[fd].flags == FD_CLOEXEC);
    CHECK(file_fcntl(client, copy, F_DUPFD_CLOEXEC, 7) == 7 && client->fds[7].flags == FD_CLOEXEC);
    int device = file_open(client, "/dev/null", O_RDWR, 0); CHECK(device == 2);
    CHECK(file_dup(client, copy, device, true, 0) == device && file_fd(client, device) == file_fd(client, fd));
    int surface = surface_create(client, 2, 2, CIUKI_SURFACE_XRGB8888); CHECK(surface == 3);
    CHECK(file_dup(client, surface, 4, true, 0) == 4 && desktop_fd(client, surface) == desktop_fd(client, 4));
    CHECK(file_dup(client, copy, 4, true, 0) == 4 && file_fd(client, 4) == file_fd(client, fd));
    CHECK(!file_close(client, surface));
    unsigned n = 0;
    while (file_dup(client, copy, 0, false, 0) >= 0) n++;
    CHECK(n == 123 && file_fd_slot(client, 0) == -EMFILE);
    CHECK(file_open(client, "/must-not-exist", O_CREAT | O_RDWR, 0666) == -EMFILE);
    struct px_path r; CHECK(resolve_path("/must-not-exist", &r) == -ENOENT);
    CHECK(!file_close(client, 31) && file_close(client, 31) == -EBADF);
    CHECK(file_dup(client, copy, 0, false, 0) == 31);
    all_closed();
    CHECK(!space->descriptions && !file_descriptions && !desktop_objects.descriptions);
    puts("files fd table: 128 slots, lowest free, EMFILE, dup/dup2/self, CLOEXEC, common desktop table, close-once PASS");
}

static void test_io(void)
{
    int a = file_open(client, "/io.bin", O_CREAT | O_RDWR | O_APPEND, 0666); CHECK(a >= 0);
    int b = file_open(client, "/io.bin", O_RDWR | O_APPEND, 0); CHECK(b >= 0);
    CHECK(io(a, "abc", 3, true, false, 0) == 3);
    CHECK(io(b, "def", 3, true, false, 0) == 3);
    CHECK(file_fd(client,a)->position == 3 && file_fd(client,b)->position == 6);
    CHECK(io(a, "Z", 1, true, true, 1) == 1 && file_fd(client,a)->position == 3);
    char buffer[128] = { 0 }; CHECK(io(b, buffer, 6, false, true, 0) == 6 && !memcmp(buffer,"aZcdef",6));
    CHECK(file_fd(client,b)->position == 6);
    int64_t position;
    CHECK(!locked_seek(file_fd(client,a), (int64_t)UINT32_MAX + 13, SEEK_SET, &position));
    CHECK(position == (int64_t)UINT32_MAX + 13);
    CHECK(!file_fcntl(client,a,F_SETFL,0));
    unsigned writes = disk[0].writes;
    CHECK(io(a,"x",1,true,false,0) == -EFBIG && disk[0].writes == writes);
    CHECK(io(a,buffer,1,false,false,0) == 0);
    CHECK(!locked_seek(file_fd(client,a), INT64_MAX, SEEK_SET, &position));
    CHECK(locked_seek(file_fd(client,a),1,SEEK_CUR,&position) == -EOVERFLOW);
    CHECK(locked_seek(file_fd(client,a),INT64_MIN,SEEK_CUR,&position) == -EINVAL);
    CHECK(!locked_truncate(file_fd(client,a),100));
    CHECK(file_fd(client,a)->position == INT64_MAX);
    CHECK(io(b,buffer,100,false,true,0) == 100);
    for (unsigned i=6;i<100;i++) CHECK(buffer[i] == 0);
    CHECK(!locked_sync(file_fd(client,a)));
    struct ciuki_stat st; CHECK(!locked_stat(file_fd(client,a)->node,&st));
    CHECK(st.st_dev==3 && st.st_mode==(S_IFREG|0666) && st.st_size==100 && st.st_nlink==1 && st.st_blocks==volumes[0].spc);
    CHECK(st.st_blksize==volumes[0].spc*512 && !st.st_uid && !st.st_gid && !st.reserved);
    uint64_t inode=st.st_ino;
    all_closed(); a=file_open(client,"/io.bin",O_RDONLY,0); CHECK(a>=0);
    CHECK(!locked_stat(file_fd(client,a)->node,&st) && st.st_ino==inode);
    CHECK(io(a,buffer,100,false,false,0)==100 && !memcmp(buffer,"aZcdef",6));
    all_closed();
    puts("files I/O: append, positioned I/O, 64-bit seeks, EFBIG before writes, truncate zero-fill, fsync/reopen, stat identity PASS");
}

static void test_namespace(void)
{
    CHECK(!make_dir("/Move")); CHECK(!make_dir("/Move/Child")); CHECK(!make_dir("/Target"));
    CHECK(!px_chdir(client,"/Move/Child"));
    CHECK(!rename_path("/Move","/Target/Renamed"));
    char path[CIUKI_PATH_MAX]; CHECK(!locked_cwd(client->cwd,path) && !strcmp(path,"/Target/Renamed/Child"));
    CHECK(remove_path_native(".",true)==-EINVAL);
    CHECK(remove_path_native("/Target/Renamed/Child",true)==-EBUSY);
    CHECK(!rename_path("/Target/Renamed/Child","/Target/Renamed/Child"));
    CHECK(!rename_path("/Target/Renamed/Child","/Target/Renamed/cHILD"));
    CHECK(!locked_cwd(client->cwd,path) && !strcmp(path,"/Target/Renamed/cHILD"));
    CHECK(rename_path("/Target","/Target/Renamed/cHILD/Descendant")==-EINVAL);
    CHECK(rename_path("/io.bin","/Target")==-EISDIR);
    CHECK(rename_path("/Target","/io.bin")==-ENOTDIR);
    CHECK(rename_path("/io.bin","/mnt/d/other")==-EXDEV);
    CHECK(rename_path("/Target/.","/x")==-EINVAL);
    CHECK(rename_path("/io.bin","/dev")==-EBUSY);
    CHECK(!rename_path("/Target","/Target"));
    CHECK(!px_chdir(client,"/"));
    int dir=file_open(client,"/Target/Renamed/cHILD",O_DIRECTORY|O_RDONLY,0); CHECK(dir>=0);
    CHECK(remove_path_native("/Target/Renamed/cHILD",true)==-EBUSY);
    CHECK(!file_close(client,dir)); CHECK(!remove_path_native("/Target/Renamed/cHILD",true));
    CHECK(!make_dir("/EmptyA")); CHECK(!make_dir("/EmptyB"));
    if (fat_rename_replace) CHECK(!rename_path("/EmptyA","/EmptyB"));
    else {
        unsigned before = disk[0].writes;
        CHECK(rename_path("/EmptyA","/EmptyB") == -EIO && before == disk[0].writes);
        puts("files replacement: NOT RUN (fat_rename_replace integration absent); safe refusal verified");
    }
    CHECK(!make_dir("/Full")); CHECK(!make_dir("/Full/inside"));
    CHECK(rename_path("/EmptyB","/Full")==-ENOTEMPTY);
    CHECK(remove_path_native("/Full",false)==-EISDIR);
    CHECK(remove_path_native("/dev",true)==-EBUSY);
    dir=file_open(client,"/",O_DIRECTORY,0); CHECK(dir>=0);
    struct ciuki_dirent entry; int n; uint64_t cookie=0; bool dev=false,mnt=false;
    while ((n=locked_dents(file_fd(client,dir),&entry))>0) {
        CHECK(n==(int)sizeof(struct ciuki_dirent) && entry.d_reclen==(int)sizeof(struct ciuki_dirent) && entry.d_off==(int64_t)++cookie);
        CHECK(entry.d_namlen==strlen(entry.d_name));
        for (unsigned i=entry.d_namlen;i<sizeof(entry.d_name);i++) CHECK(!entry.d_name[i]);
        CHECK(!entry.reserved[0] && !entry.reserved[1] && !entry.reserved[2]);
        if (!strcmp(entry.d_name,"dev")) { CHECK(!dev); dev=true; }
        if (!strcmp(entry.d_name,"mnt")) { CHECK(!mnt); mnt=true; }
    }
    CHECK(!n && dev && mnt);
    int64_t pos; CHECK(!locked_seek(file_fd(client,dir),0,SEEK_SET,&pos));
    CHECK(locked_dents(file_fd(client,dir),&entry)==(int)sizeof(struct ciuki_dirent) && !strcmp(entry.d_name,".") && entry.d_off==1);
    CHECK(locked_seek(file_fd(client,dir),1,SEEK_SET,&pos)==-EINVAL);
    all_closed();
    puts("files namespace: rename decision table, cwd after rename, pins, fixed dirents/cookies/rewind PASS");
}

static void test_syscalls(void)
{
    put_path(USER,"/sys.bin");
    CHECK(call(CIUKI_SYS_OPEN,USER,O_CREAT|O_RDWR,0666,0,0)==0);
    CHECK(call(CIUKI_SYS_WRITE,0,0,0,0,0)==0);
    CHECK(call(CIUKI_SYS_READ,0,UINT32_MAX,0,0,0)==0);
    unsigned writes=disk[0].writes;
    CHECK(call(CIUKI_SYS_WRITE,0,UINT32_MAX-1,16,0,0)==-EFAULT && disk[0].writes==writes);
    CHECK(call(CIUKI_SYS_WRITE,0,USER,CIUKI_IO_MAX+1,0,0)==-EINVAL);
    CHECK(call(CIUKI_SYS_LSEEK64,0,77,0,SEEK_SET,0)==-EFAULT && !file_fd(client,0)->position);
    CHECK(call(CIUKI_SYS_FSTAT,0,CIUKI_IMAGE_BASE,0,0,0)==-EFAULT);
    put_path(USER,"/invalid-output-created");
    CHECK(call(CIUKI_SYS_STAT,USER,0,0,0,0)==-EFAULT);
    CHECK(call(CIUKI_SYS_OPEN,USER,0x80000000,0,0,0)==-EINVAL && disk[0].writes==writes);
    CHECK(call(CIUKI_SYS_OPEN,USER,O_TRUNC|O_RDONLY,0,0,0)==-EINVAL);
    CHECK(call(CIUKI_SYS_GETCWD,USER,1,0,0,0)==-ERANGE);
    CHECK(call(CIUKI_SYS_GETCWD,USER,CIUKI_PATH_MAX,0,0,0)==2);
    char out[2]; CHECK(!ua_read(client->memory,out,USER,2) && !strcmp(out,"/"));
    CHECK(call(CIUKI_SYS_CLOSE,0,0,0,0,0)==0 && call(CIUKI_SYS_CLOSE,0,0,0,0,0)==-EBADF);
    put_path(USER,"/dev/null"); CHECK(call(CIUKI_SYS_OPEN,USER,O_RDWR,0,0,0)==0);
    CHECK(call(CIUKI_SYS_READ,0,USER2,12,0,0)==0);
    CHECK(call(CIUKI_SYS_WRITE,0,USER2,12,0,0)==12);
    CHECK(call(CIUKI_SYS_PREAD,0,0,0,0,0)==-ESPIPE);
    CHECK(call(CIUKI_SYS_LSEEK64,0,0,0,0,USER2)==-ESPIPE);
    CHECK(file_open(client,"/dev/console",O_RDONLY,0)==-EACCES);
    CHECK(file_open(client,"/dev/null",O_CREAT|O_WRONLY,0666)==-EINVAL);
    int console=file_open(client,"/dev/console",O_WRONLY,0); CHECK(console>=0);
    const char text[]="CIUKI_TEST v=1\n\tUTF8: \xc3\xa9\n";
    CHECK(io(console,(void *)text,sizeof(text)-1,true,false,0)==sizeof(text)-1);
    CHECK(console_count==sizeof(text)-1 && !memcmp(console_bytes,text,sizeof(text)-1));
    CHECK(!memcmp(serial_bytes,"[console] ",10) && !strstr(serial_bytes,"\nCIUKI_TEST"));
    all_closed();
    puts("files syscall validation: copied paths, buffer pins, bad output no position change, zero I/O, device rules, console framing PASS");
}

static int (*saved_read)(struct blkdev *, uint64_t, uint32_t, void *);
static unsigned interrupted_reads;
static int interrupt_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *buffer)
{
    int err = saved_read(dev, lba, count, buffer);
    interrupted_reads++;
    client_thread->interrupted = true;
    return err;
}
static void test_read_interruption(void)
{
    int fd = file_open(client, "/", O_RDONLY | O_DIRECTORY, 0); CHECK(fd >= 0);
    struct file_description *d = file_fd(client, fd);
    struct ciuki_dirent record;
    for (unsigned i = 0; i < 4; i++) CHECK(locked_dents(d, &record) == (int)sizeof(struct ciuki_dirent));
    CHECK(d->position == 4 && d->cursor == 0);
    memset(&record, 0xa5, sizeof(record));
    CHECK(!ua_write(client->memory, USER2, &record, sizeof(record)));
    cache_invalidate(&cache, &disk[0].dev, false);
    saved_read = disk[0].dev.read; disk[0].dev.read = interrupt_read;
    interrupted_reads = 0;
    CHECK(call(CIUKI_SYS_GETDENTS, fd, USER2, sizeof(record), 0, 0) == -EINTR);
    disk[0].dev.read = saved_read; client_thread->interrupted = false;
    CHECK(interrupted_reads && d->position == 4 && d->cursor == 0);
    uint8_t buffer[sizeof(record)]; CHECK(!ua_read(client->memory, buffer, USER2, sizeof(buffer)));
    for (unsigned i = 0; i < sizeof(buffer); i++) CHECK(buffer[i] == 0xa5);
    CHECK(call(CIUKI_SYS_GETDENTS, fd, USER2, sizeof(record), 0, 0) == (int)sizeof(struct ciuki_dirent) && d->position == 5);
    CHECK(!file_close(client, fd));
    puts("files interrupted directory read: drained record discarded, buffer/cookie unchanged, retry succeeds PASS");
}

/* f0-44 regression (desktop wallpaper "Device or resource busy"): every
 * uaddr walk, even ua_find, holds a whole-space mutation pin across its
 * service points, so a peer thread of the same process can be preempted
 * while holding one. read/stat/getdents pin their output before the
 * filesystem lock and copy under it; the kernel lock holds preemption
 * (fs_lock_take -> kpreempt_hold, which FS_HOST does not model), and a
 * second pin there refused with EBUSY instead of waiting. The disk read
 * below runs under that lock: it publishes the peer walk and the hold. */
static struct task peer_walker;
static struct ua_pin peer_walk;
static bool peer_walk_armed;
static unsigned peer_walk_reads;
static void peer_walk_begin(void)
{
    struct uaddr *u = client->memory;
    peer_walk = (struct ua_pin){ .base = 0, .end = CIUKI_MAIN_STACK_LIMIT, .mutation = true,
                                 .task = &peer_walker, .next = u->pins };
    u->pins = &peer_walk; g_current->preempt_holds++;
}
static void peer_walk_end(void)
{
    CHECK(peer_walk.task == &peer_walker && g_current->preempt_holds);
    ua_unpin(client->memory, &peer_walk); g_current->preempt_holds--;
    CHECK(!peer_walk.task);
}
static int peer_walk_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *buffer)
{
    int err = saved_read(dev, lba, count, buffer);
    if (peer_walk_armed) { peer_walk_armed = false; peer_walk_reads++; peer_walk_begin(); }
    return err;
}
static void test_peer_walk_copy(void)
{
    struct uaddr *u = client->memory;
    struct ciuki_mmap_args a = map_args(4 * PAGE_SIZE, PROT_READ | PROT_WRITE);
    int32_t mapped = ua_mmap(u, &a); CHECK(mapped > 0);
    uint32_t buffer = (uint32_t)mapped, bytes = 3 * PAGE_SIZE - 77; /* Nested.bin holds 12345 */
    /* Uncovered user copies under a hold still refuse rather than sleep
     * with the lock held; the caller's own prepared pin is the contract. */
    uint8_t probe[64]; memset(probe, 0x3c, sizeof(probe));
    peer_walk_begin();
    CHECK(copy_to_user(buffer, probe, sizeof(probe)) == -EBUSY);
    CHECK(copy_from_user(probe, buffer, sizeof(probe)) == -EBUSY);
    peer_walk_end();
    struct ua_pin own;
    CHECK(!ua_pin(u, &own, buffer, PAGE_SIZE, false));
    peer_walk_begin();
    CHECK(!copy_from_user(probe, buffer + 8, sizeof(probe)));
    CHECK(copy_to_user(buffer + 8, probe, sizeof(probe)) == -EBUSY); /* read pin */
    peer_walk_end(); ua_unpin(u, &own);
    CHECK(!ua_pin(u, &own, buffer, PAGE_SIZE, true));
    peer_walk_begin();
    CHECK(!copy_to_user(buffer + 8, probe, sizeof(probe)));
    CHECK(copy_to_user(buffer + PAGE_SIZE - 8, probe, 16) == -EBUSY); /* beyond the pin */
    peer_walk_end(); ua_unpin(u, &own);
    uint8_t check[64]; CHECK(!ua_read(u, check, buffer + 8, sizeof(check)) && !memcmp(check, probe, sizeof(check)));
    /* read(): file data reaches the pinned buffer under the lock. */
    int fd = file_open(client, "/Fixture dir/Nested.bin", O_RDONLY, 0); CHECK(fd >= 0);
    uint8_t *expected = malloc(bytes), *actual = malloc(bytes); CHECK(expected && actual);
    CHECK(io(fd, expected, bytes, false, true, 0) == (int)bytes);
    saved_read = disk[0].dev.read; disk[0].dev.read = peer_walk_read; peer_walk_reads = 0;
    cache_invalidate(&cache, &disk[0].dev, false); peer_walk_armed = true;
    CHECK(call(CIUKI_SYS_READ, fd, buffer, bytes, 0, 0) == (int)bytes);
    CHECK(peer_walk_reads == 1); peer_walk_end();
    CHECK(!ua_read(u, actual, buffer, bytes) && !memcmp(actual, expected, bytes));
    CHECK(file_fd(client, fd)->position == bytes);
    /* fstat and stat copy their record under the lock as well. */
    cache_invalidate(&cache, &disk[0].dev, false); peer_walk_armed = true;
    put_path(buffer + 2 * PAGE_SIZE, "/Fixture dir/Nested.bin");
    CHECK(call(CIUKI_SYS_STAT, buffer + 2 * PAGE_SIZE, buffer, 0, 0, 0) == 0);
    CHECK(peer_walk_reads == 2); peer_walk_end();
    struct ciuki_stat st; CHECK(!ua_read(u, &st, buffer, sizeof(st)) && st.st_size == 12345);
    disk[0].dev.read = saved_read; peer_walk_armed = false;
    for (const struct ua_pin *p = u->pins; p; p = p->next) CHECK(p->task != &peer_walker && p != &own);
    CHECK(!g_current->preempt_holds);
    free(expected); free(actual); CHECK(!file_close(client, fd));
    CHECK(!ua_munmap(u, buffer, 4 * PAGE_SIZE));
    puts("files peer walk: read/stat copy through the caller's own output pin under the filesystem lock, uncovered copies still refused PASS");
}

static void test_lifetime_and_legacy(void)
{
    struct vfs_table table; CHECK(!vfs_table_init(&vfs, &table, 2));
    int old = vfs_open(&table, "C:/SHORT.BIN", VFS_READ, VFS_DENY_WRITE, 0); CHECK(old >= 0);
    CHECK(file_open(client, "/SHORT.BIN", O_WRONLY, 0) == -EACCES);
    CHECK(!vfs_close(&table, old));
    int fd = file_open(client, "/SHORT.BIN", O_RDONLY, 0); CHECK(fd >= 0);
    CHECK(vfs_open(&table, "C:/SHORT.BIN", VFS_WRITE, VFS_DENY_READ, 0) == -FS_EACCES);
    old = vfs_open(&table, "C:/SHORT.BIN", VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0); CHECK(old >= 0);
    CHECK(!vfs_truncate(&table, old, 19) && file_fd(client, fd)->node->entry.size == 19);
    CHECK(!vfs_close(&table, old));
    struct process *child; CHECK(!proc_prepare(client, &child));
    struct ciuki_spawn_fd inherited = {fd, 10};
    CHECK(!proc_inherit(child, client, &inherited, 1));
    CHECK(child->fds[10].object == client->fds[fd].object && child->cwd == client->cwd);
    char data[64]; CHECK(io(fd, data, 3, false, false, 0) == 3);
    CHECK(file_fd(child, 10)->position == 3);
    proc_discard(child); all_closed();
    fd = file_open(client, "/orphan.bin", O_CREAT | O_RDWR, 0666); CHECK(fd >= 0);
    CHECK(io(fd, "old", 3, true, false, 0) == 3);
    if (!fat_native_ready || !fat_native_ready()) {
        unsigned before = disk[0].writes;
        CHECK(remove_path_native("/orphan.bin", false) == -EIO && disk[0].writes == before);
        puts("files open-unlink: NOT RUN (FAT detached-entry integration absent); safe refusal verified");
    } else {
        uint64_t ino = file_fd(client, fd)->node->ino;
        CHECK(!remove_path_native("/orphan.bin", false));
        struct px_path r; CHECK(resolve_path("/orphan.bin", &r) == -ENOENT);
        struct ciuki_stat st; CHECK(!locked_stat(file_fd(client, fd)->node, &st) && st.st_nlink == 0 && st.st_ino == ino);
        int fresh = file_open(client, "/orphan.bin", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fresh >= 0);
        CHECK(file_fd(client, fresh)->node->ino != ino);
        CHECK(io(fd, "N", 1, true, true, 1) == 1);
        CHECK(io(fd, data, 3, false, true, 0) == 3 && !memcmp(data, "oNd", 3));
        CHECK(file_fd(client, fresh)->node->entry.size == 0);
        CHECK(!locked_truncate(file_fd(client, fd), 20));
        CHECK(!locked_sync(file_fd(client, fd)));
        CHECK(!file_close(client, fd));
        fd = fresh;
        int replacement = file_open(client, "/replacement.bin", O_CREAT | O_RDWR, 0666); CHECK(replacement >= 0);
        CHECK(io(replacement, "retain", 6, true, false, 0) == 6);
        CHECK(!rename_path("/orphan.bin", "/replacement.bin"));
        CHECK(!locked_stat(file_fd(client, replacement)->node, &st) && !st.st_nlink);
        CHECK(io(replacement, data, 6, false, true, 0) == 6 && !memcmp(data, "retain", 6));
        CHECK(!file_close(client, replacement));
        /* The last legacy handle, rather than the last native fd, releases
         * a mixed-family orphan. Both families observe native writes. */
        old = vfs_open(&table, "C:/replacement.bin", VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0); CHECK(old >= 0);
        CHECK(io(fd, "last", 4, true, false, 0) == 4);
        CHECK(!remove_path_native("/replacement.bin", false));
        CHECK(!file_close(client, fd));
        size_t done; CHECK(!vfs_read(&table, old, data, 4, &done) && done == 4 && !memcmp(data, "last", 4));
        CHECK(!vfs_close(&table, old)); fd = -1;
        puts("files open-unlink/replacement: native+legacy lifetime, detached writes/truncate, slot reuse, nlink and final-chain release PASS");
    }
    if (fd >= 0) CHECK(!file_close(client, fd));
    fd = file_open(client, "/tracked.bin", O_CREAT | O_RDWR, 0666); CHECK(fd >= 0);
    CHECK(io(fd, "live", 4, true, false, 0) == 4);
    old = vfs_open(&table, "C:/tracked.bin", VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0); CHECK(old >= 0);
    CHECK(!rename_path("/tracked.bin", "/Full/long tracked name.bin"));
    size_t transferred;
    CHECK(!vfs_read(&table, old, data, 4, &transferred) && transferred == 4 && !memcmp(data,"live",4));
    CHECK(!vfs_write(&table, old, "!", 1, &transferred) && transferred == 1);
    CHECK(file_fd(client,fd)->node->entry.size == 5 && io(fd,data,5,false,true,0) == 5 && !memcmp(data,"live!",5));
    if (fat_native_ready && fat_native_ready()) {
        CHECK(!remove_path_native("/Full/long tracked name.bin",false));
        CHECK(!file_close(client,fd) && !vfs_close(&table,old));
    } else {
        CHECK(!file_close(client,fd) && !vfs_close(&table,old));
        CHECK(!remove_path_native("/Full/long tracked name.bin",false));
    }
    struct file_ledger start, end;
    files_snapshot(space,&start);
    for (unsigned i=0;i<32;i++) {
        fd=file_open(client,"/churn.bin",O_CREAT|O_EXCL|O_RDWR,0666); CHECK(fd>=0);
        CHECK(!file_close(client,fd) && !remove_path_native("/churn.bin",false));
    }
    files_snapshot(space,&end); CHECK(start.nodes == end.nodes && start.pins == end.pins);
    puts("files namespace lifetime: legacy handles follow rename; create/remove churn retains no dead nodes PASS");
    bool was_ro = volumes[0].readonly; volumes[0].readonly = true;
    CHECK(file_open(client, "/ro-created", O_CREAT | O_WRONLY, 0666) == -EROFS);
    volumes[0].readonly = was_ro;
    vfs_table_destroy(&table);
    puts("files inheritance/share: shared cwd/offsets, legacy/native share conflicts and metadata, readonly refusal PASS");
}

static bool overwrite_calendar(uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    *date = (46u << 9) | (10u << 5) | 16;
    *time = 0; *tenths = 0;
    return true;
}

static void test_overwrite_ctime(void)
{
    struct vfs_table table; CHECK(!vfs_table_init(&vfs, &table, 2));
    for (unsigned fallback = 0; fallback < 2; fallback++) {
        fs_set_calendar_clock(fallback ? 0 : overwrite_calendar);
        file_clock_init(42, false, false, 0, 0, 0, 0);
        int fd = file_open(client, "/ctime.bin", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0);
        CHECK(io(fd, "a", 1, true, true, 0) == 1);
        int old = vfs_open(&table, "C:/ctime.bin", VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0); CHECK(old >= 0);
        struct px_node *node = file_fd(client, fd)->node;
        struct fat_entry entry = node->entry;
        struct ciuki_stat before, after;
        CHECK(!locked_stat(node, &before));
        for (unsigned legacy = 0; legacy < 2; legacy++) {
            for (unsigned repeat = 0; repeat < 2; repeat++) {
                /* The valid sample remains in one packed FAT interval. The
                 * fallback also stays fixed when more than two seconds pass. */
                g_ticks += fallback ? 2100 : 100;
                char byte = (char)('b' + legacy * 2 + repeat);
                if (legacy) {
                    uint64_t position; size_t done;
                    CHECK(!vfs_seek(&table, old, 0, VFS_SEEK_SET, &position) && !position);
                    CHECK(!vfs_write(&table, old, &byte, 1, &done) && done == 1);
                } else CHECK(io(fd, &byte, 1, true, true, 0) == 1);
                CHECK(!memcmp(&entry, &node->entry, sizeof(entry)));
                CHECK(!memcmp(&entry, &table.handles[old]->node->entry, sizeof(entry)));
                CHECK(!locked_stat(node, &after));
                struct ciuki_timespec now; CHECK(!file_clock_now(CLOCK_REALTIME, client, &now));
                CHECK(after.st_ctim.tv_sec == now.tv_sec && after.st_ctim.tv_nsec == now.tv_nsec);
                CHECK(after.st_ctim.tv_sec > before.st_ctim.tv_sec ||
                      (after.st_ctim.tv_sec == before.st_ctim.tv_sec && after.st_ctim.tv_nsec > before.st_ctim.tv_nsec));
                CHECK(after.st_mtim.tv_sec == before.st_mtim.tv_sec && after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
                char readback; CHECK(io(fd, &readback, 1, false, true, 0) == 1 && readback == byte);
                before = after;
            }
            g_ticks += 100;
            if (legacy) {
                size_t done = 1; CHECK(!vfs_write(&table, old, "", 0, &done) && !done);
            } else CHECK(!io(fd, "", 0, true, true, 0));
            CHECK(!locked_stat(node, &after));
            CHECK(after.st_ctim.tv_sec == before.st_ctim.tv_sec && after.st_ctim.tv_nsec == before.st_ctim.tv_nsec);
        }
        CHECK(!vfs_close(&table, old) && !file_close(client, fd));
        CHECK(!remove_path_native("/ctime.bin", false));
    }
    fs_set_calendar_clock(0);
    vfs_table_destroy(&table);
    puts("files overwrite ctime: native+legacy repeated writes, unchanged FAT entry, advancing clock, fixed/fallback timestamps, zero-length no-ops PASS");
}

struct append_test { struct file_description *description; char byte; };
static void *append_worker(void *arg)
{
    struct append_test *test = arg;
    char block[4]; memset(block, test->byte, sizeof(block));
    for (unsigned i = 0; i < 16; i++) {
        size_t done = 0;
        vfs_lock(&vfs,0);
        int err = file_io_locked(test->description, block, sizeof(block), true, false, 0, &done);
        vfs_unlock(&vfs);
        if (err || done != sizeof(block)) return (void *)1;
    }
    return 0;
}
static void test_concurrent_append(void)
{
    int a=file_open(client,"/parallel.bin",O_CREAT|O_RDWR|O_APPEND,0666);
    int b=file_open(client,"/parallel.bin",O_RDWR|O_APPEND,0); CHECK(a>=0 && b>=0);
    struct append_test arg[2]={{file_fd(client,a),'A'},{file_fd(client,b),'B'}};
    pthread_t workers[2];
    for (unsigned i=0;i<2;i++) CHECK(!pthread_create(&workers[i],0,append_worker,&arg[i]));
    for (unsigned i=0;i<2;i++) { void *result; CHECK(!pthread_join(workers[i],&result) && !result); }
    char bytes[128]; CHECK(io(a,bytes,sizeof(bytes),false,true,0)==sizeof(bytes));
    unsigned as=0,bs=0;
    for (unsigned i=0;i<sizeof(bytes);i+=4) {
        CHECK(bytes[i]=='A' || bytes[i]=='B');
        as+=bytes[i]=='A'; bs+=bytes[i]=='B';
        for (unsigned j=1;j<4;j++) CHECK(bytes[i+j]==bytes[i]);
    }
    CHECK(as==16 && bs==16); all_closed();
    puts("files concurrent append: two host threads, two descriptions, 32 indivisible records PASS");
}

/* Each injected failure uses the original fake device and cache. A failed
 * filesystem is abandoned, never 'recovered' by clearing its sticky error. */
static void test_write_failures(const char *path)
{
    for (unsigned which=0;which<3;which++) {
        if (which==2 && (!fat_native_ready || !fat_native_ready())) continue;
        CHECK(!fake_open(&disk[0],path)); disk[0].undo_enabled=true;
        CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
        CHECK(!fat_mount(&volumes[0],&cache,&disk[0].dev,0,disk[0].dev.capacity,true));
        CHECK(!vfs_attach(&vfs,2,&volumes[0]) && !files_attach(&vfs,&space));
        client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
        client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
        int fd=file_open(client,"/fault.bin",O_CREAT|O_RDWR,0666); CHECK(fd>=0);
        CHECK(io(fd,"old",3,true,false,0)==3);
        if (which==0) {
            disk[0].fail_flush=true;
            CHECK(io(fd,"new",3,true,true,0)==-EIO);
            CHECK(file_fd(client,fd)->position==3 && volumes[0].readonly);
        } else if (which==1) {
            disk[0].fail_write=volumes[0].fsinfo;
            CHECK(io(fd,"new",3,true,true,volumes[0].spc*512)==3); /* allocation/owning entry committed */
            CHECK(volumes[0].readonly && locked_sync(file_fd(client,fd))==-EIO);
        } else {
            CHECK(!remove_path_native("/fault.bin",false)); disk[0].fail_flush=true;
            CHECK(file_close(client,fd)==-EIO && file_close(client,fd)==-EBADF);
        }
        all_closed(); stop_collect(client);
        vfs_destroy(&vfs); cache_destroy(&cache); fake_reset(&disk[0]); fake_close(&disk[0]);
        CHECK(!pages_used && heap_blocks==1);
    }
    puts("files write faults: precommit EIO, postcommit positive count, sticky fsync error, teardown ledger PASS");
    if (fat_native_ready && fat_native_ready()) puts("files close fault: orphan release EIO removes fd exactly once PASS");
}

static void sleep_interrupt(void) { if (g_ticks==1010) client_thread->interrupted=true; }
static void test_clocks(void)
{
    int64_t utc;
    CHECK(clock_fat_utc((20u<<9)|(2u<<5)|29,0,&utc) && utc==951782400);
    CHECK(!clock_fat_utc((120u<<9)|(2u<<5)|29,0,&utc));
    CHECK(!clock_fat_utc(0,0,&utc));
    file_clock_init(42,false,false,0,0,0,123);
    struct clock_seed seed; file_clock_snapshot(&seed); CHECK(!seed.source && seed.utc==42 && !seed.tick);
    struct ciuki_timespec now; g_ticks=1234;
    CHECK(!file_clock_now(CLOCK_REALTIME,client,&now) && now.tv_sec==43 && now.tv_nsec==234000000);
    file_clock_init(42,true,true,(20u<<9)|(1u<<5)|1,0,100,1000);
    file_clock_snapshot(&seed); CHECK(seed.source==1 && seed.utc==946684801 && seed.tick==1000);
    CHECK(!file_clock_now(CLOCK_REALTIME,client,&now) && now.tv_sec==946684801 && now.tv_nsec==234000000);
    file_clock_init(42,true,false,0,0,0,123);
    file_clock_snapshot(&seed); CHECK(!seed.source && seed.qualified && !seed.valid && !seed.tick);
    CHECK(!file_clock_res(CLOCK_PROCESS_CPUTIME_ID,&now) && !now.tv_sec && now.tv_nsec==1000000);
    CHECK(file_clock_res(3,&now)==-EINVAL && file_clock_now(3,client,&now)==-EINVAL);
    client->cpu_ticks=0; client_thread->task->state=T_RUNNING;
    for (unsigned i=0;i<100;i++) file_clock_tick();
    CHECK(!file_clock_now(CLOCK_PROCESS_CPUTIME_ID,client,&now) && now.tv_nsec==100000000);
    client_thread->task->state=T_BLOCKED; file_clock_tick(); CHECK(client->cpu_ticks==100); client_thread->task->state=T_RUNNING;
    uint64_t end;
    struct ciuki_timespec request={.tv_nsec=1};
    CHECK(!file_sleep_deadline(&request,7,&end) && end==8);
    request.tv_nsec=1000001; CHECK(!file_sleep_deadline(&request,7,&end) && end==9);
    CHECK(file_sleep_deadline(&request,UINT64_MAX-1,&end)==-EOVERFLOW);
    request=(struct ciuki_timespec){.tv_sec=INT64_MAX}; CHECK(file_sleep_deadline(&request,0,&end)==-EOVERFLOW);
    request=(struct ciuki_timespec){.tv_sec=-1}; CHECK(file_sleep_deadline(&request,0,&end)==-EINVAL);
    request=(struct ciuki_timespec){.tv_nsec=1000000000}; CHECK(file_sleep_deadline(&request,0,&end)==-EINVAL);
    request=(struct ciuki_timespec){.reserved=1}; CHECK(file_sleep_deadline(&request,0,&end)==-EINVAL);
    request=(struct ciuki_timespec){.tv_nsec=20000000}; CHECK(!ua_write(client->memory,USER,&request,sizeof(request)));
    g_ticks=1000; CHECK(call(CIUKI_SYS_NANOSLEEP,USER,USER2,0,0,0)==0 && g_ticks==1020);
    CHECK(!ua_read(client->memory,&now,USER2,sizeof(now)) && !now.tv_sec && !now.tv_nsec);
    g_ticks=1000; on_schedule=sleep_interrupt;
    CHECK(call(CIUKI_SYS_NANOSLEEP,USER,USER2,0,0,0)==-EINTR && g_ticks==1010);
    CHECK(!ua_read(client->memory,&now,USER2,sizeof(now)) && !now.tv_sec && now.tv_nsec==10000000);
    on_schedule=0; client_thread->interrupted=false;
    puts("files clocks: RTC/build selection, UTC leap days, 1ms CPU accounting, rounding/overflow, sleep/EINTR remainder PASS");
}
static void test_uname(void)
{
    CHECK(sizeof(struct ciuki_utsname) == 336 && offsetof(struct ciuki_utsname, abi_version) == 328 &&
          offsetof(struct ciuki_utsname, realtime_source) == 332);
    for (unsigned rtc = 0; rtc < 2; rtc++) {
        file_clock_init(42, rtc, rtc, (20u << 9) | (1u << 5) | 1, 0, 0, 1000);
        uint8_t canary[sizeof(struct ciuki_utsname) + 2]; memset(canary, 0xa5, sizeof(canary));
        CHECK(!ua_write(client->memory, USER, canary, sizeof(canary)));
        CHECK(!call(CIUKI_SYS_UNAME, USER + 1, 0, 0, 0, 0));
        CHECK(!ua_read(client->memory, canary, USER, sizeof(canary)) && canary[0] == 0xa5 && canary[sizeof(canary)-1] == 0xa5);
        struct ciuki_utsname u; memcpy(&u, canary + 1, sizeof(u));
        CHECK(!strcmp(u.sysname, "CiukiOS") && !strcmp(u.nodename, "ciuki") && !strcmp(u.machine, "i686"));
        CHECK(!strcmp(u.release, "unknown") && !strcmp(u.version, "unknown") && u.abi_version == 1 && u.realtime_source == rtc);
        CHECK(!u.reserved[0] && !u.reserved[1] && !u.reserved[2]);
        CHECK(!u.sysname[64] && !u.nodename[64] && !u.release[64] && !u.version[64] && !u.machine[64]);
    }
    CHECK(call(CIUKI_SYS_UNAME, CIUKI_IMAGE_BASE, 0, 0, 0, 0) == -EFAULT);
    CHECK(call(CIUKI_SYS_UNAME, UINT32_MAX - 8, 0, 0, 0, 0) == -EFAULT);
    CHECK(call(CIUKI_SYS_UNAME, USER + 2 * PAGE_SIZE - 8, 0, 0, 0, 0) == -EFAULT);
    puts("files uname: 336-byte layout, identity/build, RTC/fallback agreement, zero padding, bad ranges PASS");
}
#ifdef FD_TABLE_STACK_TEST
#define main files_fixture_main
#endif
static unsigned optical_writes;
static int native_optical_read(struct blkdev *b,uint64_t lba,uint32_t count,void *out)
{
    int e=blkdev_range(b,lba,count); if (e) return e;
    return fseek(b->ctx,(long)(lba*2048),SEEK_SET) || fread(out,2048,count,b->ctx)!=count ? -FS_EIO : 0;
}
static int native_optical_write(struct blkdev *b,uint64_t lba,uint32_t count,const void *out)
{ (void)b; (void)lba; (void)count; (void)out; optical_writes++; return -FS_EROFS; }
static void test_optical_native(void)
{
    FILE *f=fopen("build/host/optical/fixture-0.iso","rb"); CHECK(f);
    CHECK(!fseek(f,0,SEEK_END)); long size=ftell(f);
    struct blkdev b={.read=native_optical_read,.write=native_optical_write,.capacity=(uint64_t)size/2048,
        .sector_size=2048,.media_generation=1,.ctx=f};
    struct fs_ro_volume ro;
    CHECK(!fs_ro_mount(&ro,&iso9660_ops,&cache,&b,0,b.capacity)); CHECK(!vfs_attach_ro(&vfs,4,&ro));
    struct px_path path; CHECK(!resolve_path("/mnt/e/DATA/ODD.BIN",&path));
    struct ciuki_stat st; CHECK(!locked_stat(path.node,&st) && st.st_size==4099 && (st.st_mode&0777)==0444);
    int fd=file_open(client,"/mnt/e/DATA/ODD.BIN",O_RDONLY,0); CHECK(fd>=0);
    uint8_t bytes[11]; CHECK(io(fd,bytes,11,false,false,0)==11);
    for (unsigned i=0;i<sizeof(bytes);i++) CHECK(bytes[i]==(uint8_t)(i*17+3));
    CHECK(file_open(client,"/mnt/e/absent",O_RDONLY|O_CREAT,0666)==-EROFS);
    CHECK(file_open(client,"/mnt/e/DATA/ODD.BIN",O_WRONLY|O_TRUNC,0)==-EROFS);
    CHECK(io(fd,bytes,1,true,false,0)==-EROFS);
    CHECK(locked_truncate(file_fd(client,fd),0)==-EROFS);
    CHECK(!locked_sync(file_fd(client,fd)));
    CHECK(make_dir("/mnt/e/absent")==-EROFS); CHECK(remove_path_native("/mnt/e/DATA/ODD.BIN",false)==-EROFS);
    CHECK(rename_path("/mnt/e/DATA/ODD.BIN","/mnt/e/DATA/renamed")==-EROFS);
    CHECK(!px_chdir(client,"/mnt/e/DATA")); b.media_generation++;
    CHECK(io(fd,bytes,1,false,false,0)==-EIO && !vfs_drive_present(&vfs,4));
    CHECK(resolve_path(".",&path)==-ENOENT);
    CHECK(!fs_ro_mount(&ro,&iso9660_ops,&cache,&b,0,b.capacity)); CHECK(!vfs_attach_ro(&vfs,4,&ro));
    CHECK(io(fd,bytes,1,false,false,0)==-EIO); CHECK(!file_close(client,fd)); CHECK(!px_chdir(client,"/"));
    CHECK(!vfs_detach(&vfs,4)); CHECK(!optical_writes); fclose(f);
    puts("native optical: PASS (64-bit metadata/read, EROFS mutations, stale descriptions/cwd, remount identity, zero writes)");
}
static void test_launch_reads(void)
{
    files_launch_verbose=true;
    int fd=file_open(client,"/launch-read.bin",O_CREAT|O_RDWR,0666); CHECK(fd>=0);
    unsigned char buffer[5000]; memset(buffer,0x71,sizeof(buffer));
    CHECK(io(fd,buffer,sizeof(buffer),true,false,0)==sizeof(buffer));
    client->launch=(struct proc_launch){.active=true};
    CHECK(io(fd,buffer,4096,false,true,0)==4096);
    CHECK(client->launch.volume_reads==1 && client->launch.volume_bytes==4096 && client->launch.volume_max==4096);
    CHECK(io(fd,buffer,4096,false,true,4096)==904);
    CHECK(client->launch.volume_reads==2 && client->launch.volume_bytes==5000);
    CHECK(io(fd,buffer,4096,false,true,5000)==0);
    CHECK(client->launch.volume_reads==3 && client->launch.volume_bytes==5000);
    struct ciuki_file snapshot; CHECK(!image_open(client->cwd,"/launch-read.bin",&snapshot));
    CHECK(client->launch.volume_reads==4 && client->launch.volume_bytes==10000 && client->launch.volume_max==5000);
    uint64_t reads=client->launch.volume_reads;
    CHECK(!snapshot.read(snapshot.cookie,0,buffer,sizeof(buffer)));
    CHECK(client->launch.volume_reads==reads); /* immutable RAM snapshot isn't disk payload */
    snapshot.close(snapshot.cookie);
    client->launch.finished=true;
    CHECK(io(fd,buffer,10,false,true,0)==10 && client->launch.volume_reads==reads);
    memset(&client->launch,0,sizeof(client->launch)); CHECK(!file_close(client,fd));
    CHECK(!remove_path_native("/launch-read.bin",false));
    files_launch_verbose=false;
    puts("PASS native launch read accounting: short read, EOF, batched immutable snapshot once, stop at first frame");
}
static void test_launch_throughput(void)
{
    const unsigned bytes=1024*1024;
    uint8_t *payload=malloc(bytes), *readback=malloc(bytes); CHECK(payload && readback);
    for (unsigned i=0;i<bytes;i++) payload[i]=(uint8_t)(i*31+(i>>8));
    int fd=file_open(client,"/launch-throughput.bin",O_CREAT|O_RDWR,0666); CHECK(fd>=0);
    CHECK(io(fd,payload,bytes,true,false,0)==(int)bytes);
    cache_invalidate(&cache,&disk[0].dev,false);
    files_launch_verbose=true; client->launch=(struct proc_launch){.active=true};
    struct ciuki_file snapshot; CHECK(!image_open(client->cwd,"/launch-throughput.bin",&snapshot));
    CHECK(client->launch.volume_bytes==bytes && client->launch.volume_reads==16 && client->launch.volume_max==65536);
    CHECK(client->launch.disk_bytes>=bytes && client->launch.disk_max>=32768 && client->launch.disk_reads<64);
    printf("PASS 1 MiB native snapshot accounting: read_requests=%llu read_max=%u disk_requests=%llu disk_max=%u\n",
           (unsigned long long)client->launch.volume_reads,client->launch.volume_max,
           (unsigned long long)client->launch.disk_reads,client->launch.disk_max);
    CHECK(!snapshot.read(snapshot.cookie,0,readback,bytes) && !memcmp(readback,payload,bytes));
    snapshot.close(snapshot.cookie);
    unsigned pages=pages_used, heap=heap_blocks, live=fs_host_contig_live;
    fs_host_contig_fail=1;
    CHECK(image_open(client->cwd,"/launch-throughput.bin",&snapshot)==-ENOMEM);
    CHECK(pages_used==pages && heap_blocks==heap && fs_host_contig_live==live);
    static const int failures[]={0,1,2,18,257};
    for (unsigned i=0;i<ARRAY_SIZE(failures);i++) {
        alloc_fail=failures[i];
        CHECK(image_open(client->cwd,"/launch-throughput.bin",&snapshot)==-ENOMEM);
        alloc_fail=-1;
        CHECK(pages_used==pages && heap_blocks==heap && fs_host_contig_live==live);
    }
    struct ciuki_mmap_args mapping=map_args(2*BLKDEV_READ_BYTES,PROT_READ|PROT_WRITE);
    unsigned user=(uint32_t)ua_mmap(client->memory,&mapping);
    CHECK(user>=CIUKI_MMAP_BASE && user<CIUKI_MMAP_LIMIT);
    cache_invalidate(&cache,&disk[0].dev,false); client->launch=(struct proc_launch){.active=true};
    CHECK(call(CIUKI_SYS_PREAD,fd,user,BLKDEV_READ_BYTES+1,0,0)==BLKDEV_READ_BYTES);
    CHECK(!ua_read(client->memory,readback,user,BLKDEV_READ_BYTES) && !memcmp(readback,payload,BLKDEV_READ_BYTES));
    CHECK(client->launch.volume_max==65536 && client->launch.disk_max>=32768);
    CHECK(fs_host_contig_live==live);
    fs_host_contig_fail=1;
    CHECK(call(CIUKI_SYS_PREAD,fd,user,BLKDEV_READ_BYTES,0,0)==-ENOMEM && fs_host_contig_live==live);
    /* Writes use the same bounded run, and return short before another run. */
    CHECK(!ua_write(client->memory,user,payload,BLKDEV_READ_BYTES));
    unsigned writes=disk[0].writes;
    fs_host_contig_fail=1;
    CHECK(call(CIUKI_SYS_PWRITE,fd,user,BLKDEV_READ_BYTES,0,0)==-ENOMEM);
    CHECK(fs_host_contig_live==live && disk[0].writes==writes);
    /* The full request crosses UINT32_MAX even though its first run fits. */
    CHECK(call(CIUKI_SYS_PWRITE,fd,user,BLKDEV_READ_BYTES+1,UINT32_MAX-BLKDEV_READ_BYTES,0)==-EFBIG);
    CHECK(disk[0].writes==writes);
    uint64_t saved=file_fd(client,fd)->position;
    CHECK(call(CIUKI_SYS_PWRITE,fd,user,BLKDEV_READ_BYTES+1,0,0)==BLKDEV_READ_BYTES);
    CHECK(file_fd(client,fd)->position==saved && fs_host_contig_live==live);
    CHECK(fs_lock_try_take(&vfs.lock));vfs_unlock(&vfs);
    /* A directory poll can complete between the caller's bounded runs. */
    int directory=file_open(client,"/",O_DIRECTORY,0);CHECK(directory>=0);
    CHECK(call(CIUKI_SYS_GETDENTS,directory,user+BLKDEV_READ_BYTES,sizeof(struct ciuki_dirent),0,0)>0);
    CHECK(!file_close(client,directory));
    int append=file_open(client,"/launch-throughput.bin",O_WRONLY|O_APPEND,0);CHECK(append>=0);
    CHECK(call(CIUKI_SYS_WRITE,append,user,BLKDEV_READ_BYTES+1,0,0)==BLKDEV_READ_BYTES);
    CHECK(file_fd(client,append)->position==bytes+BLKDEV_READ_BYTES);
    CHECK(call(CIUKI_SYS_PWRITE,append,user,1,3,0)==1 && file_fd(client,append)->position==bytes+BLKDEV_READ_BYTES);
    CHECK(io(fd,readback,BLKDEV_READ_BYTES,false,true,bytes)==BLKDEV_READ_BYTES && !memcmp(readback,payload,BLKDEV_READ_BYTES));
    CHECK(!file_close(client,append));
    CHECK(!ua_munmap(client->memory,user,2*BLKDEV_READ_BYTES));
    client->launch=(struct proc_launch){0}; files_launch_verbose=false;
    CHECK(!file_close(client,fd) && !remove_path_native("/launch-throughput.bin",false));
    free(payload); free(readback);
    puts("PASS native 64 KiB read/write syscall/snapshot bytes, short run boundary, temporary buffer rollback");
}

/* Native devctl copies/dispatch use a bounded observation backend; the
 * BOT suite independently exercises actual mount/eject and physical I/O. */
static struct storage media_observation;
static unsigned media_removals;
static int media_result;
#ifndef NATIVE_DISK_TEST
struct storage *storage_get(void) { return &media_observation; }
#endif
int usb_storage_eject(struct storage *s,unsigned drive,uint32_t gen)
{ CHECK(s==&media_observation && drive==4 && gen==123); media_removals++; return media_result; }
/* f5-12B2: the CD-audio devctls (media_cd.c) are exercised by ata_test; here only the dispatch edge. */
static unsigned media_cd_calls;
int media_cd_devctl(struct process *p,struct storage *s,uint32_t op,uint32_t a,uint32_t b)
{ (void)p;(void)s;(void)a;(void)b; media_cd_calls++; return op==MEDIA_CD_STATE ? 0 : -ENOTTY; }
static unsigned optical_removals;
int optical_eject(struct storage *s,unsigned drive,uint32_t gen)
{
    CHECK(s==&media_observation);
    if (drive!=3) return -FS_ENOENT;
    if (gen!=321) return -FS_EIO;
    optical_removals++; return media_result;
}
/* MEDIA_GET_VOLUME formats stable IDs with the production bounded formatter. */
#include "../../../src/kernel/lib/fmt.c"
#include "../../../src/kernel/drivers/media_dev.c"
static void test_media_node(void)
{
    struct px_path path;CHECK(!resolve_path("/dev/media",&path)&&path.node==space->media);
    struct ciuki_stat st;CHECK(!locked_stat(path.node,&st)&&st.st_mode==(S_IFCHR|0444)&&st.st_rdev==6);
    CHECK(file_open(client,"/dev/media",O_RDWR,0)==-EACCES);
    int fd=file_open(client,"/dev/media",O_RDONLY|O_CLOEXEC,0);CHECK(fd>=0);
    CHECK(call(CIUKI_SYS_READ,fd,USER,1,0,0)==-EBADF && call(CIUKI_SYS_LSEEK64,fd,0,0,SEEK_SET,USER)==-ESPIPE);
    vfs_init(&media_observation.vfs);media_observation.ready=true;
    struct blkdev b={.media_generation=123};
    media_observation.volumes[4]=(struct storage_volume){.present=true,.removable=true,.part={.parent=&b}};
    media_observation.vfs.volumes[4]=&media_observation.volumes[4].fat;
    struct blkdev cd={.media_generation=321};
    media_observation.optical[2]=(struct fs_ro_volume){.mounted=true,.dev=&cd,.mount_generation=321};
    media_observation.vfs.ro_volumes[3]=&media_observation.optical[2];
    struct ciuki_media_state s={.version=CIUKI_MEDIA_VERSION,.size=sizeof(s)};CHECK(!ua_write(client->memory,USER,&s,sizeof(s)));
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_STATE,USER,sizeof(s),0));CHECK(!ua_read(client->memory,&s,USER,sizeof(s)));
    CHECK(s.removable_mask==((1u<<4)|(1u<<3))&&s.generation[4]==123&&!s.generation[2]);
    CHECK(s.type[4]==CIUKI_MEDIA_USB && s.type[3]==CIUKI_MEDIA_OPTICAL && s.generation[3]==321);
    /* One copied list per physical device, duplicate descriptors only once. */
    int held=file_open(client,"/media-busy.bin",O_CREAT|O_RDWR,0600); CHECK(held>=0);
    int duplicate=file_dup(client,held,0,false,0); CHECK(duplicate>=0);
    struct px_node *held_node=file_fd(client,held)->node;
    struct fat_volume *saved_volume=held_node->volume;
    struct vfs *saved_vfs=space->vfs;
    held_node->volume=&media_observation.volumes[5].fat; space->vfs=&media_observation.vfs;
    media_observation.volumes[5]=(struct storage_volume){.present=true,.removable=true,.part={.parent=&b}};
    struct ciuki_media_blockers blockers={.version=CIUKI_MEDIA_VERSION,.size=sizeof(blockers),.drive=4,.generation=123};
    CHECK(!ua_write(client->memory,USER,&blockers,sizeof(blockers)));
    /* f5-12B2: every MEDIA_CD_* op reaches media_cd.c through the same fd, nothing else does. */
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_CD_STATE,USER,48,0) && media_cd_calls==1);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_CD_POSITION,USER,48,0)==-ENOTTY && media_cd_calls==2);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_CD_POSITION+1,USER,48,0)==-ENOTTY && media_cd_calls==2);
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_BLOCKERS,USER,sizeof(blockers),0));
    CHECK(!ua_read(client->memory,&blockers,USER,sizeof(blockers)));
    CHECK(blockers.count==1 && blockers.programs[0].pid==client->pid && !strcmp(blockers.programs[0].name,client->name));
    blockers.generation++; CHECK(!ua_write(client->memory,USER,&blockers,sizeof(blockers)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_BLOCKERS,USER,sizeof(blockers),0)==-EIO);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_BLOCKERS,USER,sizeof(blockers)-1,0)==-EINVAL);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_BLOCKERS,UINT32_MAX,sizeof(blockers),0)==-EFAULT);
    held_node->volume=saved_volume; space->vfs=saved_vfs;
    CHECK(!file_close(client,held) && !file_close(client,duplicate));
    CHECK(!remove_path_native("/media-busy.bin",false)); media_observation.volumes[5].present=false;
    struct ciuki_media_state_v1 old={.version=1,.size=sizeof(old)};
    CHECK(!ua_write(client->memory,USER,&old,sizeof(old)));
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_STATE,USER,sizeof(old),0));
    CHECK(!ua_read(client->memory,&old,USER,sizeof(old)) && old.removable_mask==s.removable_mask && old.generation[3]==321 && old.version==1);
    /* F2-51 copied arrival metadata for both USB and optical ejectable volumes. */
    media_observation.vfs.generation[4]=7;
    media_observation.volumes[4].fat.volume_serial=0x12345678;
    media_observation.volumes[4].fat.sectors=1234;
    memcpy(media_observation.volumes[4].fat.volume_label,"MIXTAPE",8);
    struct ciuki_media_volume volume={.version=CIUKI_MEDIA_VERSION,.size=sizeof(volume),.drive=4};
    CHECK(!ua_write(client->memory,USER,&volume,sizeof(volume)));
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,USER,sizeof(volume),0)); CHECK(!ua_read(client->memory,&volume,USER,sizeof(volume)));
    CHECK(volume.flags==CIUKI_MEDIA_REMOVABLE && volume.generation==7 && !strcmp(volume.id,"fat-12345678-1234") && !strcmp(volume.label,"MIXTAPE"));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,UINT32_MAX,sizeof(volume),0)==-EFAULT);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,USER,sizeof(volume)-1,0)==-EINVAL);
    volume=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION_1,.size=sizeof(volume),.drive=4}; CHECK(!ua_write(client->memory,USER,&volume,sizeof(volume)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,USER,sizeof(volume),0)==-EOPNOTSUPP);
    volume.version=CIUKI_MEDIA_VERSION; volume.reserved[0]=1; CHECK(!ua_write(client->memory,USER,&volume,sizeof(volume)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,USER,sizeof(volume),0)==-EINVAL);
    struct fs_ro_volume optical={.ops=&iso9660_ops,.mounted=true,.dev=&b,.mount_generation=123,.volume_identity=0x123456789abcdef0ull,.volume_label="CIUKI_CD"};
    media_observation.vfs.ro_volumes[3]=&optical; media_observation.vfs.generation[3]=8;
    volume=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(volume),.drive=3}; CHECK(!ua_write(client->memory,USER,&volume,sizeof(volume)));
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,USER,sizeof(volume),0)); CHECK(!ua_read(client->memory,&volume,USER,sizeof(volume)));
    CHECK(volume.flags==CIUKI_MEDIA_OPTICAL && volume.generation==8 && !strcmp(volume.id,"iso-123456789abcdef0") && !strcmp(volume.label,"CIUKI_CD"));
    optical.mount_generation=122; volume=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(volume),.drive=3}; CHECK(!ua_write(client->memory,USER,&volume,sizeof(volume)));
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_VOLUME,USER,sizeof(volume),0)); CHECK(!ua_read(client->memory,&volume,USER,sizeof(volume))); CHECK(!volume.flags && !volume.generation && !volume.id[0]);
    media_observation.vfs.ro_volumes[3]=&media_observation.optical[2];
    CHECK(!ua_write(client->memory,USER,&s,sizeof(s)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_STATE,UINT32_MAX,sizeof(s),0)==-EFAULT);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_STATE,USER,sizeof(s)-1,0)==-EINVAL);
    s.reserved=1;CHECK(!ua_write(client->memory,USER,&s,sizeof(s)));CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_GET_STATE,USER,sizeof(s),0)==-EINVAL);
    struct ciuki_media_remove r={1,sizeof(r),4,123};CHECK(!ua_write(client->memory,USER,&r,sizeof(r)));
    media_result=-FS_EBUSY;CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_SAFE_REMOVE,USER,sizeof(r),0)==-EBUSY&&media_removals==1);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_SAFE_REMOVE,UINT32_MAX,sizeof(r),0)==-EFAULT&&media_removals==1);
    media_result=0;CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_SAFE_REMOVE,USER,sizeof(r),0)&&media_removals==2);
    r.generation=0;CHECK(!ua_write(client->memory,USER,&r,sizeof(r)));CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_SAFE_REMOVE,USER,sizeof(r),0)==-EINVAL&&media_removals==2);
    r=(struct ciuki_media_remove){CIUKI_MEDIA_VERSION,sizeof(r),3,320};CHECK(!ua_write(client->memory,USER,&r,sizeof(r)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_SAFE_REMOVE,USER,sizeof(r),0)==-EIO && !optical_removals && media_removals==2);
    r.generation=321;CHECK(!ua_write(client->memory,USER,&r,sizeof(r)));
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,MEDIA_SAFE_REMOVE,USER,sizeof(r),0) && optical_removals==1 && media_removals==2);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,0xffffffff,USER,sizeof(r),0)==-ENOTTY);
    CHECK(!file_close(client,fd));media_observation.vfs.volumes[4]=0;media_observation.vfs.ro_volumes[3]=0;vfs_destroy(&media_observation.vfs);media_observation.ready=false;
    puts("PASS /dev/media namespace/devctl: readonly/stream rules, copied state and removal, version/size/reserved/fault checks, busy/success, no invalid-pointer side effects");
}
static void test_com_nodes(void)
{
    /* Check all eight possible ports, including absent ones, against all
     * ATA slots and the fixed devices using the production stat path. */
    uint32_t numbers[6+4+CIUKI_COM_MAX]; unsigned count=0;
    struct px_node *fixed[]={space->null,space->console,space->audio,space->mixer,space->net,space->media};
    struct ciuki_stat device_stat;
    for (unsigned i=0;i<6;i++) {
        CHECK(!locked_stat(fixed[i],&device_stat) && device_stat.st_rdev==i+1);
        numbers[count++]=device_stat.st_rdev;
    }
    for (unsigned i=0;i<4;i++) {
        char name[]="/dev/ata0"; name[8]+=(char)i;
        struct px_path disk_path;
        CHECK(!resolve_path(name,&disk_path) && disk_path.node==space->disks[i]);
        CHECK(!locked_stat(disk_path.node,&device_stat) && device_stat.st_rdev==32+i);
        CHECK(device_stat.st_mode==(S_IFCHR|0644));
        numbers[count++]=device_stat.st_rdev;
    }
    for (unsigned i=0;i<CIUKI_COM_MAX;i++) {
        struct px_node port={.space=space,.kind=PX_COM,.drive=i,.linked=true};
        CHECK(!locked_stat(&port,&device_stat) && device_stat.st_rdev==16+i);
        numbers[count++]=device_stat.st_rdev;
    }
    for (unsigned i=0;i<count;i++) for (unsigned j=0;j<i;j++) CHECK(numbers[i]!=numbers[j]);
    struct px_path path; CHECK(!resolve_path("/dev/com2",&path) && path.node->kind==PX_COM);
    struct ciuki_stat st; CHECK(!locked_stat(path.node,&st) && st.st_mode==(S_IFCHR|0666) && st.st_rdev==17);
    CHECK(file_open(client,"/dev/com1",O_RDONLY,0)==-EBUSY);
    CHECK(file_open(client,"/dev/com2",O_RDWR|O_APPEND,0)==-EINVAL);
    int fd=file_open(client,"/dev/com2",O_RDWR,0); CHECK(fd>=0);
    CHECK(file_open(client,"/dev/com2",O_RDONLY,0)==-EBUSY);
    CHECK(file_fcntl(client,fd,F_SETFL,O_APPEND)==-EINVAL);
    CHECK(!file_fcntl(client,fd,F_SETFL,O_NONBLOCK));
    CHECK(file_fcntl(client,fd,F_GETFL,0)==(O_RDWR|O_NONBLOCK));
    CHECK(call(CIUKI_SYS_READ,fd,0x1234,17,0,0)==7 && mock_com_reads==1);
    CHECK(call(CIUKI_SYS_WRITE,fd,0x1234,17,0,0)==7 && mock_com_writes==1);
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,COM_GET_STATUS,0x1234,80,0) && mock_com_controls==1);
    CHECK(call(CIUKI_SYS_LSEEK64,fd,0,0,SEEK_SET,USER)==-ESPIPE);
    CHECK(call(CIUKI_SYS_PREAD,fd,USER,1,0,0)==-ESPIPE);
    CHECK(call(CIUKI_SYS_PWRITE,fd,USER,1,0,0)==-ESPIPE);
    CHECK(call(CIUKI_SYS_FSYNC,fd,0,0,0,0)==-EINVAL);
    CHECK(call(CIUKI_SYS_FTRUNCATE,fd,0,0,0,0)==-EINVAL);
    int dup=file_dup(client,fd,0,false,0); CHECK(dup>=0);
    CHECK(!file_close(client,fd) && !mock_com_closes && mock_com_generation==77);
    CHECK(!file_close(client,dup) && mock_com_closes==1 && !mock_com_generation);
    int dir=file_open(client,"/dev",O_RDONLY|O_DIRECTORY,0); CHECK(dir>=0);
    struct ciuki_dirent entry;
    const char *names[]={".","..","null","console","audio","mixer","net","media",
                         "ata0","ata1","ata2","ata3","com1","com2"};
    for (unsigned pass=0;pass<2;pass++) {
        for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
            CHECK(locked_dents(file_fd(client,dir),&entry)==(int)sizeof(entry));
            CHECK(!strcmp(entry.d_name,names[i]) && entry.d_off==(int64_t)i+1);
            CHECK(entry.d_type==(i<2 ? DT_DIR : DT_CHR));
        }
        uint64_t end=file_fd(client,dir)->position;
        CHECK(!locked_dents(file_fd(client,dir),&entry) && file_fd(client,dir)->position==end);
        CHECK(!locked_dents(file_fd(client,dir),&entry) && file_fd(client,dir)->position==end);
        int64_t pos; CHECK(!locked_seek(file_fd(client,dir),0,SEEK_SET,&pos) && !pos);
    }
    /* Sparse/unlinked ports must be skipped without dereferencing NULL or
     * disturbing the fixed-device and ATA prefix. Restore the fixture. */
    space->com[0]->linked=false;
    file_fd(client,dir)->position=12;
    CHECK(locked_dents(file_fd(client,dir),&entry)==(int)sizeof(entry));
    CHECK(!strcmp(entry.d_name,"com2") && entry.d_off==13);
    CHECK(!locked_dents(file_fd(client,dir),&entry));
    space->com[0]->linked=true;
    struct px_node *first=space->com[0]; space->com[0]=0;
    file_fd(client,dir)->position=12;
    CHECK(locked_dents(file_fd(client,dir),&entry)==(int)sizeof(entry));
    CHECK(!strcmp(entry.d_name,"com2") && entry.d_off==13);
    CHECK(!locked_dents(file_fd(client,dir),&entry));
    space->com[0]=first;
    CHECK(!file_close(client,dir));
    puts("PASS COM namespace/fd/syscall: lookup/stat/readdir, logger refusal, exclusive/dup/final-close and stream dispatch");
}
static void test_net_node(void)
{
    struct px_path path; CHECK(!resolve_path("/dev/net", &path) && path.node == space->net);
    struct ciuki_stat st; CHECK(!locked_stat(path.node, &st) && st.st_mode == (S_IFCHR | 0444) && st.st_rdev == 5);
    CHECK(file_open(client, "/dev/net", O_RDWR, 0) == -EACCES);
    CHECK(file_open(client, "/dev/net", O_WRONLY, 0) == -EACCES);
    CHECK(file_open(client, "/dev/net", O_RDONLY | O_TRUNC, 0) == -EINVAL);
    int fd = file_open(client, "/dev/net", O_RDONLY | O_CLOEXEC, 0); CHECK(fd >= 0);
    CHECK(file_fcntl(client, fd, F_GETFD, 0) == FD_CLOEXEC);
    CHECK(file_fcntl(client, fd, F_SETFL, O_APPEND) == -EINVAL);
    CHECK(call(CIUKI_SYS_READ, fd, USER, 1, 0, 0) == -EBADF);
    CHECK(call(CIUKI_SYS_WRITE, fd, USER, 1, 0, 0) == -EBADF);
    CHECK(call(CIUKI_SYS_LSEEK64, fd, 0, 0, SEEK_SET, USER) == -ESPIPE);
    struct ciuki_net_state state = { .version = 1, .size = sizeof(state) };
    CHECK(!ua_write(client->memory, USER, &state, sizeof(state)));
    CHECK(!call(CIUKI_SYS_DEVCTL, fd, NET_GET_STATE, USER, sizeof(state), 0));
    CHECK(!ua_read(client->memory, &state, USER, sizeof(state)) && !state.present && !strcmp(state.reason, "no_adapter"));
    CHECK(call(CIUKI_SYS_DEVCTL, fd, NET_GET_STATE, UINT32_MAX, sizeof(state), 0) == -EFAULT);
    CHECK(!file_close(client, fd));
    CHECK(call(CIUKI_SYS_DEVCTL, fd, NET_GET_STATE, USER, sizeof(state), 0) == -EBADF);
    int dir = file_open(client, "/dev", O_RDONLY | O_DIRECTORY, 0); CHECK(dir >= 0);
    struct ciuki_dirent entry; bool seen = false; int n;
    while ((n = locked_dents(file_fd(client, dir), &entry)) > 0)
        if (!strcmp(entry.d_name, "net")) { CHECK(entry.d_type == DT_CHR && !seen); seen = true; }
    CHECK(n == 0 && seen); CHECK(!file_close(client, dir));
    puts("PASS /dev/net namespace: lookup/stat/getdents, readonly fd/close, seek/stream rejection, actual devctl dispatch and user copies");
}
/* w32srv registry directory commit (apps/w32srv/directory.c save() and
 * storage.c wr_commit()) through the native syscall path exactly as
 * apps/w32srv/posix_io.c and sdk/libciuki/posix.c issue it: write_file
 * (open O_WRONLY|O_CREAT|O_TRUNC 0600, write loop, close), sync_file (open
 * O_RDWR, fsync, close), patch_file (open O_WRONLY, lseek64 32, write 4,
 * close), sync_file again; SERVICE.CRS.<slot> alternating, hive flushes
 * interleaved. Every failing syscall is reported with its step and errno. */
#define REG_PATH USER
#define REG_OUT (USER + 128)
#define REG_DATA (USER + 256)
#define REG_DATA_MAX (2u * PAGE_SIZE - 512u)
static void reg_put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (i * 8)); }
static void reg_fail(unsigned commit, const char *name, const char *step, int32_t r)
{
    fprintf(stderr, "FAIL registry-commit syscall commit=%u file=%s step=%s result=%d readonly=%s\n",
            commit, name, step, r, volumes[0].readonly ? "yes" : "no");
    exit(1);
}
static int32_t reg_open(const char *path, uint32_t flags, uint32_t mode)
{ put_path(REG_PATH, path); return call(CIUKI_SYS_OPEN, REG_PATH, flags, mode, 0, 0); }
static int32_t reg_write_all(int32_t fd, uint32_t va, uint32_t n)
{
    while (n) {
        int32_t w = call(CIUKI_SYS_WRITE, (uint32_t)fd, va, n, 0, 0);
        if (w <= 0) return w ? w : -EIO;
        va += (uint32_t)w; n -= (uint32_t)w;
    }
    return 0;
}
static void reg_commit(unsigned commit, const char *dir, const char *name, uint8_t *b, uint32_t n)
{
    char path[96]; snprintf(path, sizeof(path), "%s/%s", dir, name);
    CHECK(n <= REG_DATA_MAX && !ua_write(client->memory, REG_DATA, b, n));
    int32_t fd = reg_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600), r;
    if (fd < 0) reg_fail(commit, name, "write_file:open(O_WRONLY|O_CREAT|O_TRUNC)", fd);
    if ((r = reg_write_all(fd, REG_DATA, n))) reg_fail(commit, name, "write_file:write", r);
    if ((r = call(CIUKI_SYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0))) reg_fail(commit, name, "write_file:close", r);
    for (unsigned pass = 0; pass < 2; pass++) {
        if (pass) {
            uint8_t marker[4]; reg_put32(marker, 0x43524731u); memcpy(b + 32, marker, 4);
            CHECK(!ua_write(client->memory, REG_DATA, marker, 4));
            if ((fd = reg_open(path, O_WRONLY, 0)) < 0) reg_fail(commit, name, "patch_file:open(O_WRONLY)", fd);
            if ((r = call(CIUKI_SYS_LSEEK64, (uint32_t)fd, 32, 0, SEEK_SET, REG_OUT))) reg_fail(commit, name, "patch_file:lseek64(32)", r);
            int64_t at = -1; CHECK(!ua_read(client->memory, &at, REG_OUT, sizeof(at)) && at == 32);
            if ((r = reg_write_all(fd, REG_DATA, 4))) reg_fail(commit, name, "patch_file:write(4)", r);
            if ((r = call(CIUKI_SYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0))) reg_fail(commit, name, "patch_file:close", r);
        }
        if ((fd = reg_open(path, O_RDWR, 0)) < 0) reg_fail(commit, name, pass ? "sync_file_2:open(O_RDWR)" : "sync_file:open(O_RDWR)", fd);
        if ((r = call(CIUKI_SYS_FSYNC, (uint32_t)fd, 0, 0, 0, 0))) reg_fail(commit, name, pass ? "sync_file_2:fsync" : "sync_file:fsync", r);
        if ((r = call(CIUKI_SYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0))) reg_fail(commit, name, pass ? "sync_file_2:close" : "sync_file:close", r);
    }
    /* read_file of the next w32srv start: open O_RDONLY, fstat, read all. */
    if ((fd = reg_open(path, O_RDONLY, 0)) < 0) reg_fail(commit, name, "verify:open(O_RDONLY)", fd);
    if ((r = call(CIUKI_SYS_FSTAT, (uint32_t)fd, REG_OUT, 0, 0, 0))) reg_fail(commit, name, "verify:fstat", r);
    struct ciuki_stat st; CHECK(!ua_read(client->memory, &st, REG_OUT, sizeof(st)));
    if (st.st_size != n) { fprintf(stderr, "size %lld expected %u\n", (long long)st.st_size, n); reg_fail(commit, name, "verify:size", -EIO); }
    uint32_t got = 0;
    while (got < n) {
        r = call(CIUKI_SYS_READ, (uint32_t)fd, REG_DATA + got, n - got, 0, 0);
        if (r <= 0) reg_fail(commit, name, "verify:read", r ? r : -EIO);
        got += (uint32_t)r;
    }
    static uint8_t back[2 * PAGE_SIZE]; CHECK(!ua_read(client->memory, back, REG_DATA, n));
    if (memcmp(back, b, n)) reg_fail(commit, name, "verify:bytes", -EIO);
    if ((r = call(CIUKI_SYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0))) reg_fail(commit, name, "verify:close", r);
}
static uint32_t reg_record(uint8_t *b, unsigned commit)
{
    uint32_t n = 56; unsigned atoms = commit, windows = commit / 3;
    n += atoms * 25u + windows * 41u; CHECK(n <= REG_DATA_MAX);
    memset(b, 0, n); memcpy(b, "CRS1", 4); reg_put32(b + 4, 1); reg_put32(b + 12, commit + 1); reg_put32(b + 20, n - 40);
    reg_put32(b + 24, 0x5a5a0000u + commit); reg_put32(b + 28, 0xa5a50000u + commit);
    reg_put32(b + 40, 1 + 1024u * (commit + 1)); reg_put32(b + 44, 0xc000 + atoms); reg_put32(b + 48, atoms); reg_put32(b + 52, windows);
    uint8_t *p = b + 56;
    for (unsigned i = 0; i < atoms; i++) { reg_put32(p, 0xc000 + i); reg_put32(p + 4, 5); reg_put32(p + 8, 1); reg_put32(p + 12, 9); snprintf((char *)p + 16, 9, "atom%04u", i); p += 25; }
    for (unsigned i = 0; i < windows; i++) { for (unsigned j = 0; j < 24; j++) p[j] = (uint8_t)(j + i + commit); reg_put32(p + 24, 13); snprintf((char *)p + 28, 13, "window%05u", i); p += 41; }
    CHECK((uint32_t)(p - b) == n); return n;
}
static uint32_t reg_hive(uint8_t *b, unsigned commit)
{
    uint32_t n = 3797 + commit * 97u; CHECK(n <= REG_DATA_MAX);
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 7u + commit * 31u + (i >> 9));
    memcpy(b, "CRG1", 4); reg_put32(b + 32, 0); return n;
}
static void test_registry_commit(void)
{
    const char *dir = "/WINDOWS";
    put_path(REG_PATH, dir); CHECK(call(CIUKI_SYS_MKDIR, REG_PATH, 0755, 0, 0, 0) == 0);
    static uint8_t b[2 * PAGE_SIZE]; char name[24]; unsigned slot = 0;
    /* The image ships seeded hives; the service then writes the next slots. */
    for (unsigned h = 0; h < 2; h++) { snprintf(name, sizeof(name), "%s.CRG.0", h ? "USER" : "SYSTEM"); reg_commit(0, dir, name, b, reg_hive(b, h)); }
    for (unsigned c = 0; c < 24; c++) {
        slot = c ? 1 - slot : 0; snprintf(name, sizeof(name), "SERVICE.CRS.%u", slot);
        reg_commit(c, dir, name, b, reg_record(b, c));
        if (c % 6 == 5) {
            snprintf(name, sizeof(name), "%s.CRG.%u", c % 12 == 5 ? "SYSTEM" : "USER", 1 - (c / 6) % 2);
            reg_commit(c, dir, name, b, reg_hive(b, c));
        }
    }
    all_closed();
    puts("PASS registry directory commit via syscalls: open O_CREAT|O_TRUNC/write/close, open/fsync/close, open/lseek64/write 4/close, open/fsync/close on SERVICE.CRS.0/1 x24 plus hive flushes, reread exact");
}
static void test_setup_metadata(void)
{
    const char *name="/Setup long metadata name.bin";
    int fd=file_open(client,name,O_CREAT|O_RDWR,0600); CHECK(fd>=0);
    uint8_t data[9000]; memset(data,0x5a,sizeof(data)); CHECK(io(fd,data,sizeof(data),true,false,0)==sizeof(data));
    CHECK(!locked_sync(file_fd(client,fd)));
    struct ciuki_setup_fat q={.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(q),.cluster_index=CIUKI_SETUP_INDEX_NONE};
    CHECK(!ua_write(client->memory,USER,&q,sizeof(q)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,CIUKI_SETUP_FAT_QUERY,USER,sizeof(q),0)==-EPERM);
    client->setup_capability=true;
    CHECK(call(CIUKI_SYS_DEVCTL,fd,CIUKI_SETUP_FAT_QUERY,USER,sizeof(q),0)==-EPERM);
    setup_test_mode=true;
    CHECK(!call(CIUKI_SYS_DEVCTL,fd,CIUKI_SETUP_FAT_QUERY,USER,sizeof(q),0));
    CHECK(!ua_read(client->memory,&q,USER,sizeof(q)));
    CHECK(q.file_size==sizeof(data) && q.mount_generation && q.chain_generation && q.alias[0] && !strcmp(q.name,name+1));
    struct ciuki_setup_fat original=q;
    unsigned clusters=(sizeof(data)+q.cluster_sectors*512-1)/(q.cluster_sectors*512);
    uint64_t prior=0;
    for(unsigned i=0;i<clusters;i++) {
        q.cluster_index=i; CHECK(!ua_write(client->memory,USER,&q,sizeof(q)));
        CHECK(!call(CIUKI_SYS_DEVCTL,fd,CIUKI_SETUP_FAT_QUERY,USER,sizeof(q),0));
        CHECK(!ua_read(client->memory,&q,USER,sizeof(q)) && q.lba && q.lba!=prior);prior=q.lba;
    }
    q.cluster_index=clusters; CHECK(file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_QUERY,&q)==-ENOENT);
    q=original;q.mount_generation++;
    CHECK(file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_QUERY,&q)==-EAGAIN);
    q=original;q.chain_generation++;
    CHECK(file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_QUERY,&q)==-EAGAIN);
    q=original;q.attributes=FAT_ATTR_RO|FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM|FAT_ATTR_ARCHIVE;
    CHECK(!file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_ATTRIB,&q));
    CHECK(q.attributes==39);
    q.attributes|=FAT_ATTR_DIR;CHECK(file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_ATTRIB,&q)==-EINVAL);
    q.attributes=32; CHECK(!file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_ATTRIB,&q));
    CHECK(!locked_truncate(file_fd(client,fd),4096));
    q=original;CHECK(file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_QUERY,&q)==-EAGAIN);
    q=(struct ciuki_setup_fat){.version=1,.size=sizeof(q),.cluster_index=CIUKI_SETUP_INDEX_NONE};
    CHECK(!file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_QUERY,&q));
    space->vfs->generation[2]++;
    CHECK(file_setup_fat(client,file_fd(client,fd),CIUKI_SETUP_FAT_QUERY,&q)==-EIO);
    space->vfs->generation[2]--;
    CHECK(!file_close(client,fd));CHECK(!remove_path_native(name,false));
    struct ciuki_setup_clock clock={.version=1,.size=sizeof(clock),.seconds=1709251200,.offset_minutes=60,.rtc_local=1};
    CHECK(!file_setup_clock(client,&clock) && setup_rtc_writes==1);
    CHECK(setup_rtc_date==((2024-1980)*512+3*32+1) && (setup_rtc_time>>11)==1);
    clock.rtc_local=0;CHECK(!file_setup_clock(client,&clock) && (setup_rtc_time>>11)==0);
    clock.offset_minutes=841;CHECK(file_setup_clock(client,&clock)==-EINVAL && setup_rtc_writes==2);
    int netfd=file_open(client,"/dev/net",O_RDONLY,0);CHECK(netfd>=0);
    struct ciuki_setup_net net={.version=1,.size=sizeof(net)};
    CHECK(!ua_write(client->memory,USER,&net,sizeof(net)));
    CHECK(!call(CIUKI_SYS_DEVCTL,netfd,CIUKI_SETUP_NET_QUERY,USER,sizeof(net),0));
    CHECK(!ua_read(client->memory,&net,USER,sizeof(net)) && net.count==0);
    client->setup_capability=false;
    CHECK(call(CIUKI_SYS_DEVCTL,netfd,CIUKI_SETUP_NET_QUERY,USER,sizeof(net),0)==-EPERM);
    CHECK(!file_close(client,netfd));
    CHECK(file_setup_clock(client,&clock)==-EPERM);
    setup_test_mode=false;
    puts("Setup native metadata: authority, attributes, alias, bounded chain, EOF, mount/chain invalidation, UTC/local RTC PASS");
}
int main(int argc, char **argv)
{
    CHECK(argc==3 || argc==4);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2]));
    disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for (unsigned i=0;i<2;i++) { CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true)); CHECK(!vfs_attach(&vfs,i+2,&volumes[i])); }
    CHECK(volumes[0].volume_serial==0x00c1a001 && volumes[1].volume_serial==0x00c1a001);
    CHECK(!files_attach(&vfs,&space));
    client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
    test_paths(); test_fds(); test_io(); test_namespace(); test_syscalls(); test_read_interruption(); test_peer_walk_copy(); test_lifetime_and_legacy(); test_overwrite_ctime(); test_concurrent_append(); test_clocks(); test_net_node(); test_media_node();
    test_com_nodes(); test_uname(); test_optical_native();
    test_launch_reads(); test_launch_throughput();
    test_registry_commit(); test_setup_metadata();
    unsigned prior_pages = pages_used, prior_heap = heap_blocks;
    CHECK(f2_files_post_commit_fault());
    CHECK(pages_used == prior_pages && heap_blocks == prior_heap);
    puts("files guest fault boundary: production sparse FAT32/cache/fd positive count, sticky fsync and teardown PASS");
    if (argc == 4) {
        FILE *file = fopen(argv[3], "rb"); CHECK(file);
        CHECK(!fseek(file, 0, SEEK_END)); long size = ftell(file); CHECK(size == 12292);
        rewind(file); uint8_t *payload = malloc((size_t)size); CHECK(payload);
        CHECK(fread(payload, 1, (size_t)size, file) == (size_t)size); fclose(file);
        struct ciuki_file input = { .cookie=payload, .bytes=(uint32_t)size, .read=real_payload_read };
        struct elf_image im; CHECK(!elf_validate(&input,&im) && im.count == 2 && im.bytes == 4*PAGE_SIZE);
        struct uaddr memory; CHECK(!ua_init(&memory,&memory) && !elf_load(&input,&im,&memory));
        CHECK(!get_word(&memory,FILES_RESULT)); ua_destroy(&memory); free(payload);
        puts("files NASM ELF: production validate/load PASS (execution not run)");
    }
    all_closed(); stop_collect(client);
    struct file_ledger ledger; files_snapshot(space,&ledger); CHECK(!ledger.descriptions && !ledger.pins);
    for (unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,i+2));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    CHECK(!pages_used && heap_blocks==1);
    test_write_failures(argv[1]);
    printf("files final: descriptors=0 pins=0 pages=0 checks=%u PASS\n",checks);
    free(ram); return 0;
}
#ifdef FD_TABLE_STACK_TEST
#undef main
#endif
