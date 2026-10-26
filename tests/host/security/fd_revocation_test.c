/* Production fdtable close/dup2 bodies, real channels and process teardown.
 * Only file/device I/O is outside this channel-specific harness. */
#define CIUKI_DESKTOP_HOST_MAIN fd_unused_desktop_main
#include "../proc/desktop_test.c"
static void reserved_ref(struct proc_object *o) { (void)o; }
static struct proc_object reserved_slot={reserved_ref,reserved_ref,false};
int file_description_close(struct file_description *d) { (void)d; CHECK(false);return -EIO; }
#include "fd_detach.inc"
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);
    controller.state=T_RUNNING;proc_init();g_current=&controller;
    struct process *a,*b;make_process(proc_supervisor(),&a);make_process(proc_supervisor(),&b);
    int32_t fds[2];CHECK(!channel_connect(a,b,fds));
    struct security_peer peer={0};CHECK(!channel_peer_get(b,fds[1],&peer));
    int duplicate=file_dup(a,fds[0],0,false,0);CHECK(duplicate>=0);
    CHECK(!file_close(a,fds[0]) && channel_peer_live(&peer));
    CHECK(!file_close(a,duplicate) && !channel_peer_live(&peer));
    channel_peer_put(&peer);CHECK(!file_close(b,fds[1]));
    CHECK(!channel_connect(a,b,fds));CHECK(!channel_peer_get(b,fds[1],&peer));
    int32_t replacement[2];CHECK(!channel_pair(a,replacement));
    CHECK(file_dup(a,replacement[0],fds[0],true,0)==fds[0] && !channel_peer_live(&peer));
    channel_peer_put(&peer);CHECK(!file_close(b,fds[1]));
    CHECK(!channel_connect(a,b,fds));CHECK(!channel_peer_get(b,fds[1],&peer));
    struct desktop_description *end=desktop_fd(a,fds[0]);struct channel *c=end->u.endpoint.channel;
    CHECK(!c->revoked[0]);stop_collect(a);CHECK(c->revoked[0] && !channel_peer_live(&peer));
    channel_peer_put(&peer);stop_collect(b);
    CHECK(!pages_used && !g_user_mappings && !desktop_objects.channels && !desktop_objects.descriptions);
    kfree(supervisor_task);supervisor_task=0;CHECK(!heap_blocks);free(ram);
    puts("H01 review (a): production close/dup2 replacement/teardown revoke retained peers, duplicates and fd reuse PASS");
    return 0;
}
