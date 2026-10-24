/* Real FAT32/mount/cache/display marker with a late removable volume.
 * An explicit cache-refusal injection tests the in-pass diagnostic; native
 * FIFO admission itself is tested in publish_fifo_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main storage_regression_main
#define klog storage_fixture_log
#include "test_storage.c"
#undef main
#undef klog
#include <stdatomic.h>
int display_attempt_marker(struct vfs *,const char *,uint16_t,uint16_t,const char *,bool,bool *,const char *);
static atomic_bool writer_stop, marker_started;
static atomic_uint writer_passes, retry_records;
static _Thread_local bool marker_actor;
static unsigned injected;
static char retry_record[240];
static void pause_ms(unsigned ms)
{ struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000}; nanosleep(&t,0); }
void klog(const char *fmt,...)
{
    char line[240]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(line,sizeof(line),fmt,ap); va_end(ap); CHECK(n>=0 && n<(int)sizeof(line));
    if (strstr(line,"[storage] publication_retry ")) {
        strcpy(retry_record,line); atomic_fetch_add(&retry_records,1);
    }
    storage_fixture_log("%s",line);
}
int __real_cache_publish(struct cache_reservation *,const struct cache_image *,unsigned,uint64_t);
int __wrap_cache_publish(struct cache_reservation *r,const struct cache_image *i,unsigned n,uint64_t d)
{
    if (marker_actor && injected<2100) {
        injected++; atomic_store(&marker_started,true); pause_ms(1); return -FS_EAGAIN;
    }
    return __real_cache_publish(r,i,n,d);
}
static void *writer_run(void *arg)
{
    struct storage *s=arg;
    while (!atomic_load(&writer_stop)) {
        cache_writer_wait(&s->cache,10);
        if (bootlog_due(fs_now_ms())) (void)bootlog_drain();
        OK(storage_writeback(s,fs_now_ms()));
        (void)storage_census_service(s,fs_now_ms());
        atomic_fetch_add(&writer_passes,1);
    }
    return 0;
}
static uint64_t request_now(void *arg) { (void)arg; return (uint64_t)fs_now_ms()*1000; }
static void request_pause(void *arg) { (void)arg; pause_ms(1); }
static void *marker_run(void *arg)
{
    struct vfs_request request={.now=request_now,.pause=request_pause,.deadline=UINT64_MAX,
        .task={.name="display-marker",.id=33},.operation="create",.site="display"};
    struct fs_request_scope scope; fs_request_enter(&scope,&request);
    marker_actor=true; bool previous=false;
    OK(display_attempt_marker(arg,"s3savage",0x5333,0x8c2e,0,false,&previous,0));
    CHECK(!previous); marker_actor=false;
    fs_request_leave(&scope); return 0;
}
int main(int argc,char **argv)
{
    CHECK(argc==3); OK(fake_open(&media[0],argv[1])); OK(fake_open(&media[1],argv[2]));
    baseline(); struct storage *s=start(); OK(storage_enable_write(s,2));
    OK(vfs_mkdir(&table,"C:/system/settings",0));
    OK(bootlog_activate(&s->vfs,true,s->sequence));
    char log_data[32768]; memset(log_data,'x',sizeof(log_data)); bootlog_capture(log_data,sizeof(log_data));
    s->cache.writer_online=true;
    pthread_t writer_thread,marker_thread;
    CHECK(!pthread_create(&writer_thread,0,writer_run,s));
    CHECK(!pthread_create(&marker_thread,0,marker_run,&s->vfs));
    while (!atomic_load(&marker_started)) pause_ms(1);
    /* T23: service begins at tick 1844; USB publication at tick 3459.
     * Keep the 1615 ms spacing, not a guessed CPU-cycle calibration. */
    pause_ms(1615); OK(storage_add_removable(s,&media[1].dev));
    CHECK(storage_activate_removable(s,&media[1].dev)==1);
    struct storage_volume *e=storage_volume(s,4);
    CHECK(e && e->removable && e->fat.type==32 && e->fat.spc==16);
    CHECK(!e->fat.readonly && e->fat.scan_deferred && !e->fat.free_unknown && !e->fat.census);
    unsigned reads=media[1].reads;
    struct vfs_table observer; OK(vfs_table_init(&s->vfs,&observer,2));
    for (unsigned i=0;i<32;i++) {
        uint64_t free_bytes; uint32_t cluster_bytes;
        OK(vfs_free_space(&observer,4,&free_bytes,&cluster_bytes));
        CHECK(free_bytes==(uint64_t)e->fat.free_clusters*8192 && cluster_bytes==8192);
        CHECK(!storage_census_service(s,fs_now_ms()));
    }
    CHECK(reads==media[1].reads); /* metadata/free-space observation never scans FAT */
    vfs_table_destroy(&observer); CHECK(!pthread_join(marker_thread,0));
    CHECK(injected==2100 && atomic_load(&retry_records)>=1 && atomic_load(&retry_records)<=2);
    CHECK(strstr(retry_record,"task=display-marker#33") && strstr(retry_record,"phase=cache"));
    CHECK(atomic_load(&writer_passes)>10);
    atomic_store(&writer_stop,true); cache_writer_kick(&s->cache); CHECK(!pthread_join(writer_thread,0));
    bool previous=false; OK(display_attempt_marker(&s->vfs,"s3savage",0x5333,0x8c2e,0,true,&previous,0));
    OK(bootlog_shutdown()); stop(s,true); fake_close(&media[0]); fake_close(&media[1]);
    printf("PASS T23 storage: FAT32 E arrives at +1615 ms during marker/log publication; deferred scan, trusted FSInfo, no census/observation I/O, writer passes=%u\n",atomic_load(&writer_passes));
    puts(retry_record); return 0;
}
