/* Raw recorder, exact production code, no filesystem/cache dependency. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../src/kernel/core/blackbox.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"blackbox:%d %s\n",__LINE__,#x);exit(1); } } while (0)
static uint8_t disk[512*400];
static unsigned writes,flushes,emergency,serial_lines;
static unsigned fail_write,tear_bytes;
static pthread_mutex_t transport=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool block_writer,entered;

static int rd(struct blkdev *d,uint64_t lba,uint32_t n,void *b) {
    CHECK(!blkdev_range(d,lba,n));memcpy(b,disk+lba*512,n*512);return 0;
}
static int wr(struct blkdev *d,uint64_t lba,uint32_t n,const void *b) {
    CHECK(!blkdev_range(d,lba,n) && lba>=BLACKBOX_FIRST && lba+n<=BLACKBOX_RESERVED);
    pthread_mutex_lock(&transport);
    if (block_writer) { entered=true;pthread_cond_broadcast(&changed); }
    while (block_writer) pthread_cond_wait(&changed,&transport);
    pthread_mutex_unlock(&transport);
    writes++;
    if (writes==fail_write) { if (tear_bytes) memcpy(disk+lba*512,b,tear_bytes);return -FS_EIO; }
    memcpy(disk+lba*512,b,n*512);return 0;
}
static int flush(struct blkdev *d) { (void)d;flushes++;return 0; }
static int urgent(struct blkdev *d,uint64_t lba,const void *b) { emergency++;return wr(d,lba,1,b); }
static struct blkdev dev={.read=rd,.write=wr,.flush=flush,.emergency_write=urgent,
    .capacity=400,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED};
static void serial(const char *s,size_t n) { CHECK(n && !strncmp(s,"[blackbox] ",11) && s[n-1]=='\n');serial_lines++; }
static void setup(void) {
    blackbox_reset();memset(disk,0,sizeof(disk));writes=flushes=emergency=serial_lines=fail_write=tear_bytes=0;
    fs_wr16(disk+11,512);fs_wr16(disk+14,BLACKBOX_RESERVED);fs_wr16(disk+48,1);fs_wr16(disk+50,6);fs_wr16(disk+510,0xaa55);
    uint8_t *d=disk+512*BLACKBOX_DESCRIPTOR;memcpy(d,"CBB1",4);fs_wr32(d+4,1);fs_wr32(d+8,BLACKBOX_FIRST);fs_wr32(d+12,BLACKBOX_SECTORS);fs_wr32(d+508,crc(d,508));
}
static void bind(void) { CHECK(!blackbox_bind(&dev,0,400,serial)); }
static void *worker(void *unused) { (void)unused;CHECK(!blackbox_flush());return NULL; }
static void *release_writer(void *unused) {
    (void)unused;const struct timespec pause={0,10000000};nanosleep(&pause,NULL);
    pthread_mutex_lock(&transport);block_writer=false;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&transport);return NULL;
}
static void overlapping_checkpoint(void) {
    setup();bind();block_writer=true;entered=false;
    blackbox_capture("worker record\n",14);pthread_t producer,release;
    CHECK(!pthread_create(&producer,NULL,worker,NULL));
    pthread_mutex_lock(&transport);while (!entered) pthread_cond_wait(&changed,&transport);pthread_mutex_unlock(&transport);
    blackbox_capture("shutdown marker\n",16);
    CHECK(!pthread_create(&release,NULL,release_writer,NULL));
    CHECK(!blackbox_checkpoint());CHECK(!pthread_join(producer,NULL));CHECK(!pthread_join(release,NULL));
    blackbox_reset();bind();size_t length;const char *s=blackbox_replay(&length);
    CHECK(length && strstr(s,"worker record") && strstr(s,"shutdown marker"));
}
/* T23 2ca3b0b6 review P1: the stall watchdog requests persistence with a
 * stalled executor (this device's write blocks). Every request returns at
 * once while the flusher is inside the blocked write; the records reach the
 * sectors after the device resumes. A blocking request would hang here. */
static void async_flush_request(void) {
    setup();bind();CHECK(!blackbox_flusher_start());
    block_writer=true;entered=false;
    blackbox_capture("[storage] writer_stall ms=5000\n",31);
    struct timespec before,after;clock_gettime(CLOCK_MONOTONIC,&before);
    blackbox_flush_request();
    pthread_mutex_lock(&transport);while (!entered) pthread_cond_wait(&changed,&transport);pthread_mutex_unlock(&transport);
    blackbox_capture("[storage] writer_stall ms=15000\n",32);
    blackbox_flush_request();blackbox_flush_request();
    clock_gettime(CLOCK_MONOTONIC,&after);
    long long ms=(after.tv_sec-before.tv_sec)*1000LL+(after.tv_nsec-before.tv_nsec)/1000000;
    CHECK(block_writer && !blackbox_flush_served() && ms<1000);
    pthread_mutex_lock(&transport);block_writer=false;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&transport);
    for (unsigned i=0;blackbox_flush_served()!=3;i++) {
        CHECK(i<5000);const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    blackbox_flusher_stop();
    blackbox_reset();bind();size_t length;const char *s=blackbox_replay(&length);
    CHECK(length && strstr(s,"writer_stall ms=5000") && strstr(s,"writer_stall ms=15000"));
    printf("PASS blackbox async flush: three requests returned in %lld ms beside a blocked device write, persisted after it resumed\n",ms);
}
int main(void) {
    alarm(30); /* a request that waits for the blocked device fails, never hangs */
    overlapping_checkpoint();
    async_flush_request();
    setup();CHECK(!blackbox_available());bind();CHECK(blackbox_available() && !writes && !flushes);
    blackbox_capture("last before lock\n",17);CHECK(!blackbox_flush());CHECK(writes==1 && flushes==1);
    dev.quarantined=true;CHECK(!blackbox_available());blackbox_capture("device stopped\n",15);CHECK(blackbox_flush()==-FS_EAGAIN);
    dev.quarantined=false;blackbox_reset();bind();size_t n;const char *s=blackbox_replay(&n);
    CHECK(n && strstr(s,"last before lock"));CHECK(serial_lines==1);
    /* Every prefix of a torn sector preserves its predecessor. */
    for (unsigned torn=0;torn<512;torn++) {
        setup();bind();blackbox_capture("stable\n",7);CHECK(!blackbox_flush());
        fail_write=2;tear_bytes=torn;blackbox_capture("interrupted\n",12);CHECK(blackbox_flush()==-FS_EIO);
        blackbox_reset();bind();s=blackbox_replay(&n);CHECK(n && strstr(s,"stable") && !strstr(s,"interrupted"));
    }
    setup();bind();
    for (unsigned i=0;i<BLACKBOX_SECTORS+9;i++) {
        char line[32];int len=snprintf(line,sizeof(line),"record=%03u\n",i);blackbox_capture(line,(size_t)len);CHECK(!blackbox_flush());
    }
    blackbox_panic("panic reason=fixture\n",21);CHECK(emergency==1);
    blackbox_reset();serial_lines=0;bind();s=blackbox_replay(&n);
    CHECK(serial_lines==BLACKBOX_SECTORS && n && !strstr(s,"record=000") && strstr(s,"panic reason=fixture"));
    setup();bind();
    char full[256];memset(full,'X',sizeof(full));full[255]='\n';
    for (unsigned i=0;i<256;i++) blackbox_capture(full,sizeof(full));
    blackbox_capture("readonly reason=full_queue\n",26);CHECK(!blackbox_checkpoint());
    blackbox_reset();bind();s=blackbox_replay(&n);CHECK(strstr(s,"readonly reason=full_queue"));
    unsigned before=writes;disk[BLACKBOX_DESCRIPTOR*512+8]^=1;CHECK(blackbox_bind(&dev,0,400,serial)==-FS_ENOENT);
    blackbox_capture("refused\n",8);CHECK(blackbox_flush()==-FS_EAGAIN && before==writes);
    setup();fs_wr16(disk+14,32);CHECK(blackbox_bind(&dev,0,400,serial)==-FS_ENOENT && !writes);
    puts("PASS blackbox: raw 64KiB ring, 512 torn-sector prefixes, sequence wrap, checksum rejection, pre-mount serial replay, panic callback, overlapping worker/checkpoint, quarantine/old-volume refusal");
    return 0;
}
