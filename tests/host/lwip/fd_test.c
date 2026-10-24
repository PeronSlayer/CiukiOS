/* F6 socket object integration with the production descriptor/process paths.
 * The real transport is tested separately by stack_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#undef main
/* The descriptor fixture has no storage-maintenance backend. Keep the
 * media dispatcher linked while explicitly refusing these operations. */
int storage_convert(struct storage *s,struct ciuki_media_convert *r,bool apply)
{ (void)s; (void)r; (void)apply; return -FS_EOPNOTSUPP; }
int storage_defrag(struct storage *s,struct ciuki_media_defrag *r,bool step)
{ (void)s; (void)r; (void)step; return -FS_EOPNOTSUPP; }
int storage_format_removable(struct storage *s,struct ciuki_media_format *r,bool apply)
{ (void)s; (void)r; (void)apply; return -FS_EOPNOTSUPP; }
int storage_floppy_copy(struct storage *s,struct ciuki_floppy_image *r,storage_floppy_copy_fn fn,void *ctx,bool write)
{ (void)s; (void)r; (void)fn; (void)ctx; (void)write; return -FS_EOPNOTSUPP; }
static struct {
    struct proc_object object;
    unsigned references, flags, reads, writes;
} socket_fixture;
static void socket_retain(struct proc_object *o) { CHECK(o==&socket_fixture.object);socket_fixture.references++; }
static void socket_release(struct proc_object *o) { CHECK(o==&socket_fixture.object && socket_fixture.references);socket_fixture.references--; }
uint32_t net_socket_count(void) { return !!socket_fixture.references; }
bool net_socket_object(struct proc_object *o) { return o==&socket_fixture.object; }
uint32_t *net_socket_flags(struct proc_object *o) { return net_socket_object(o) ? &socket_fixture.flags : NULL; }
void net_fd_changed(struct process *p,unsigned fd) { CHECK(fd<CIUKI_OPEN_MAX);if(p)p->fd_generation[fd]++; }
int32_t net_socket_rw(struct process *p,int fd,uint32_t b,uint32_t n,bool write)
{
    (void)b;CHECK(p==client && net_socket_object(file_fd_object(p,fd)));
    if(write)socket_fixture.writes++;else socket_fixture.reads++;
    return (int32_t)n;
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);controller.state=T_RUNNING;g_current=&controller;proc_init();
    client_thread=make_process(proc_supervisor(),&client);g_current=client_thread->task;g_current->state=T_RUNNING;
    socket_fixture.object=(struct proc_object){socket_retain,socket_release,true};
    socket_fixture.references=1;socket_fixture.flags=O_RDWR;
    int fd=file_fd_reserve(client);CHECK(fd==0 && !file_fd_object(client,fd));
    CHECK(file_fd_publish(client,fd,&socket_fixture.object,FD_CLOEXEC)==fd);
    unsigned generation=client->fd_generation[fd];
    int copy=file_dup(client,fd,0,false,0);CHECK(copy==1 && socket_fixture.references==2);
    CHECK(!file_fcntl(client,copy,F_GETFD,0));
    CHECK(!file_fcntl(client,copy,F_SETFL,O_NONBLOCK));
    CHECK(file_fcntl(client,fd,F_GETFL,0)==(O_RDWR|O_NONBLOCK));
    CHECK(file_fcntl(client,fd,F_SETFL,O_APPEND)==-EINVAL);
    CHECK(file_fcntl(client,fd,F_GETFL,0)==(O_RDWR|O_NONBLOCK));
    /* fx-11, T23 trial 423b803a: NetSurf's open_connection (sdk/netsurf/port/
     * net.c) sets O_NONBLOCK with the POSIX idiom. F_GETFL reports O_RDWR for
     * a socket, and file_fcntl rejected that bit: "[netsurf] fetch ...
     * stage=error errno=22 at=connect" 1 ms after stage=connect, no SYN. */
    CHECK(!file_fcntl(client,fd,F_SETFL,0));
    CHECK(file_fcntl(client,fd,F_GETFL,0)==O_RDWR);
    int status=file_fcntl(client,fd,F_GETFL,0);
    CHECK(file_fcntl(client,fd,F_SETFL,(uint32_t)status|O_NONBLOCK)==0);
    CHECK(file_fcntl(client,fd,F_GETFL,0)==(O_RDWR|O_NONBLOCK));
    CHECK(file_fcntl(client,fd,F_SETFL,O_RDWR|O_APPEND)==-EINVAL);
    CHECK(file_fcntl(client,fd,F_SETFL,O_RDWR|O_NONBLOCK|0x1000000u)==-EINVAL);
    CHECK(file_fcntl(client,fd,F_GETFL,0)==(O_RDWR|O_NONBLOCK));
    struct trap_frame tf={.eax=CIUKI_SYS_READ,.ebx=(unsigned)copy,.ecx=USER,.edx=2};
    CHECK(file_syscall(&tf) && (int32_t)tf.eax==2 && socket_fixture.reads==1);
    tf.eax=CIUKI_SYS_WRITE;CHECK(file_syscall(&tf) && (int32_t)tf.eax==2 && socket_fixture.writes==1);
    unsigned calls[]={CIUKI_SYS_FSTAT,CIUKI_SYS_LSEEK64,CIUKI_SYS_PREAD,CIUKI_SYS_PWRITE,CIUKI_SYS_FSYNC,CIUKI_SYS_FTRUNCATE};
    int errors[]={-EOPNOTSUPP,-ESPIPE,-ESPIPE,-ESPIPE,-EINVAL,-EINVAL};
    for(unsigned i=0;i<ARRAY_SIZE(calls);i++){tf=(struct trap_frame){.eax=calls[i],.ebx=(unsigned)copy};CHECK(file_syscall(&tf) && (int32_t)tf.eax==errors[i]);}
    struct process *child;CHECK(!proc_prepare(client,&child));
    struct ciuki_spawn_fd inherited={copy,10};CHECK(!proc_inherit(child,client,&inherited,1));
    CHECK(child->fds[10].object==&socket_fixture.object && child->fds[1].object==&socket_fixture.object && socket_fixture.references==4);
    CHECK(!file_close(client,fd) && client->fd_generation[fd]!=generation);
    CHECK(!file_close(client,copy) && socket_fixture.references==2);
    proc_discard(child);CHECK(!socket_fixture.references);
    int reserved=file_fd_reserve(client);CHECK(reserved==0);file_fd_cancel(client,reserved);CHECK(!client->fds[0].object);
    stop_collect(client);CHECK(!pages_used && heap_blocks==1);free(ram);
    puts("PASS production socket fd reserve/publish/close/dup/fcntl, read/write/error dispatch and spawn inheritance; no pages retained");
    return 0;
}
