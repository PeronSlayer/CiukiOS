/* The DOS CPU harness substitutes VFS/transport. Supply its request context
 * too; the standalone H49 harness tests the production f1-83 context registry. */
static const struct vfs_request *host_storage_request;
const struct vfs_request *fs_request_current(void) { return host_storage_request; }
int fs_request_check(void) {
    const struct vfs_request *r=host_storage_request;
    if (!r) return 0;
    if (r->cancelled && r->cancelled(r->arg)) return -VFS_ECANCELLED;
    return r->now(r->arg)>=r->deadline ? -VFS_ETIMEOUT : 0;
}
void fs_request_wait(enum vfs_wait_reason reason,uint64_t begin,int outcome) {
    const struct vfs_request *r=host_storage_request;
    if (r && r->wait && (r->now(r->arg)!=begin || outcome))
        *r->wait=(struct vfs_wait_info){r->owner,r->now(r->arg)-begin,1u<<2,reason,outcome,true};
}
