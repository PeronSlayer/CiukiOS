/* Production paths/fds/syscalls plus the F1 fake block device and fake RAM.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main process_fixture_main
#include "proc_test.c"
#undef main
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/files_probe.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include "../fs/fake.h"

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

static struct fake disk[2];
static struct block_cache cache;
static struct fat_volume volumes[2];
static struct vfs vfs;
static struct px_namespace *space;
static struct process *client;
static struct proc_thread *client_thread;
#define USER (CIUKI_IMAGE_BASE + PAGE_SIZE)
#define USER2 (USER + 1024)

static int resolve_path(const char *path, struct px_path *r)
{
    fs_lock_take(&vfs.lock);
    int err = px_resolve_locked(space, client->cwd, path, false, r);
    fs_lock_drop(&vfs.lock); return err;
}
static int make_dir(const char *path)
{
    fs_lock_take(&vfs.lock);
    int err = px_mkdir_locked(space, client->cwd, path, 0777);
    fs_lock_drop(&vfs.lock); return err;
}
static int rename_path(const char *from, const char *to)
{
    fs_lock_take(&vfs.lock);
    int err = px_rename_locked(space, client->cwd, from, to);
    fs_lock_drop(&vfs.lock); return err;
}
static int remove_path_native(const char *path, bool dir)
{
    fs_lock_take(&vfs.lock);
    int err = px_remove_locked(space, client->cwd, path, dir);
    fs_lock_drop(&vfs.lock); return err;
}
static int io(int fd, void *buffer, size_t bytes, bool write, bool positioned, uint64_t offset)
{
    struct file_description *d = file_fd(client, fd); CHECK(d);
    size_t done;
    fs_lock_take(&vfs.lock);
    int err = file_io_locked(d, buffer, bytes, write, positioned, offset, &done);
    fs_lock_drop(&vfs.lock); return done ? (int)done : err;
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
    char cwd[CIUKI_PATH_MAX]; CHECK(!px_getcwd_locked(client->cwd, cwd) && !strcmp(cwd, "/Fixture dir"));
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
    CHECK(!file_seek_locked(file_fd(client,a), (int64_t)UINT32_MAX + 13, SEEK_SET, &position));
    CHECK(position == (int64_t)UINT32_MAX + 13);
    CHECK(!file_fcntl(client,a,F_SETFL,0));
    unsigned writes = disk[0].writes;
    CHECK(io(a,"x",1,true,false,0) == -EFBIG && disk[0].writes == writes);
    CHECK(io(a,buffer,1,false,false,0) == 0);
    CHECK(!file_seek_locked(file_fd(client,a), INT64_MAX, SEEK_SET, &position));
    CHECK(file_seek_locked(file_fd(client,a),1,SEEK_CUR,&position) == -EOVERFLOW);
    CHECK(file_seek_locked(file_fd(client,a),INT64_MIN,SEEK_CUR,&position) == -EINVAL);
    CHECK(!file_truncate_locked(file_fd(client,a),100));
    CHECK(file_fd(client,a)->position == INT64_MAX);
    CHECK(io(b,buffer,100,false,true,0) == 100);
    for (unsigned i=6;i<100;i++) CHECK(buffer[i] == 0);
    CHECK(!file_sync_locked(file_fd(client,a)));
    struct ciuki_stat st; CHECK(!px_stat_locked(file_fd(client,a)->node,&st));
    CHECK(st.st_dev==3 && st.st_mode==(S_IFREG|0666) && st.st_size==100 && st.st_nlink==1 && st.st_blocks==volumes[0].spc);
    CHECK(st.st_blksize==volumes[0].spc*512 && !st.st_uid && !st.st_gid && !st.reserved);
    uint64_t inode=st.st_ino;
    all_closed(); a=file_open(client,"/io.bin",O_RDONLY,0); CHECK(a>=0);
    CHECK(!px_stat_locked(file_fd(client,a)->node,&st) && st.st_ino==inode);
    CHECK(io(a,buffer,100,false,false,0)==100 && !memcmp(buffer,"aZcdef",6));
    all_closed();
    puts("files I/O: append, positioned I/O, 64-bit seeks, EFBIG before writes, truncate zero-fill, fsync/reopen, stat identity PASS");
}

static void test_namespace(void)
{
    CHECK(!make_dir("/Move")); CHECK(!make_dir("/Move/Child")); CHECK(!make_dir("/Target"));
    CHECK(!px_chdir(client,"/Move/Child"));
    CHECK(!rename_path("/Move","/Target/Renamed"));
    char path[CIUKI_PATH_MAX]; CHECK(!px_getcwd_locked(client->cwd,path) && !strcmp(path,"/Target/Renamed/Child"));
    CHECK(remove_path_native(".",true)==-EINVAL);
    CHECK(remove_path_native("/Target/Renamed/Child",true)==-EBUSY);
    CHECK(!rename_path("/Target/Renamed/Child","/Target/Renamed/Child"));
    CHECK(!rename_path("/Target/Renamed/Child","/Target/Renamed/cHILD"));
    CHECK(!px_getcwd_locked(client->cwd,path) && !strcmp(path,"/Target/Renamed/cHILD"));
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
    while ((n=px_getdents_locked(file_fd(client,dir),&entry))>0) {
        CHECK(n==792 && entry.d_reclen==792 && entry.d_off==(int64_t)++cookie);
        CHECK(entry.d_namlen==strlen(entry.d_name));
        for (unsigned i=entry.d_namlen;i<sizeof(entry.d_name);i++) CHECK(!entry.d_name[i]);
        CHECK(!entry.reserved[0] && !entry.reserved[1] && !entry.reserved[2]);
        if (!strcmp(entry.d_name,"dev")) { CHECK(!dev); dev=true; }
        if (!strcmp(entry.d_name,"mnt")) { CHECK(!mnt); mnt=true; }
    }
    CHECK(!n && dev && mnt);
    int64_t pos; CHECK(!file_seek_locked(file_fd(client,dir),0,SEEK_SET,&pos));
    CHECK(px_getdents_locked(file_fd(client,dir),&entry)==792 && !strcmp(entry.d_name,".") && entry.d_off==1);
    CHECK(file_seek_locked(file_fd(client,dir),1,SEEK_SET,&pos)==-EINVAL);
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
    for (unsigned i = 0; i < 4; i++) CHECK(px_getdents_locked(d, &record) == 792);
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
    CHECK(call(CIUKI_SYS_GETDENTS, fd, USER2, sizeof(record), 0, 0) == 792 && d->position == 5);
    CHECK(!file_close(client, fd));
    puts("files interrupted directory read: drained record discarded, buffer/cookie unchanged, retry succeeds PASS");
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
        struct ciuki_stat st; CHECK(!px_stat_locked(file_fd(client, fd)->node, &st) && st.st_nlink == 0 && st.st_ino == ino);
        int fresh = file_open(client, "/orphan.bin", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fresh >= 0);
        CHECK(file_fd(client, fresh)->node->ino != ino);
        CHECK(io(fd, "N", 1, true, true, 1) == 1);
        CHECK(io(fd, data, 3, false, true, 0) == 3 && !memcmp(data, "oNd", 3));
        CHECK(file_fd(client, fresh)->node->entry.size == 0);
        CHECK(!file_truncate_locked(file_fd(client, fd), 20));
        CHECK(!file_sync_locked(file_fd(client, fd)));
        CHECK(!file_close(client, fd));
        fd = fresh;
        int replacement = file_open(client, "/replacement.bin", O_CREAT | O_RDWR, 0666); CHECK(replacement >= 0);
        CHECK(io(replacement, "retain", 6, true, false, 0) == 6);
        CHECK(!rename_path("/orphan.bin", "/replacement.bin"));
        CHECK(!px_stat_locked(file_fd(client, replacement)->node, &st) && !st.st_nlink);
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

struct append_test { struct file_description *description; char byte; };
static void *append_worker(void *arg)
{
    struct append_test *test = arg;
    char block[4]; memset(block, test->byte, sizeof(block));
    for (unsigned i = 0; i < 16; i++) {
        size_t done = 0;
        fs_lock_take(&vfs.lock);
        int err = file_io_locked(test->description, block, sizeof(block), true, false, 0, &done);
        fs_lock_drop(&vfs.lock);
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
            CHECK(io(fd,"new",3,true,true,0)==3); /* owning entry committed */
            CHECK(volumes[0].readonly && file_sync_locked(file_fd(client,fd))==-EIO);
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
int main(int argc, char **argv)
{
    CHECK(argc==3 || argc==4);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2]));
    disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for (unsigned i=0;i<2;i++) { CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true)); CHECK(!vfs_attach(&vfs,i+2,&volumes[i])); }
    CHECK(!files_attach(&vfs,&space));
    client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
    test_paths(); test_fds(); test_io(); test_namespace(); test_syscalls(); test_read_interruption(); test_lifetime_and_legacy(); test_concurrent_append(); test_clocks();
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
