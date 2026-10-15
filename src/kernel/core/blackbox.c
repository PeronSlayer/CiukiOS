/* Sector records survive independently of FAT and its cache/write gate.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/blackbox.h>
#include "../fs/blkdev.h"
#ifndef FS_HOST
#include <ciuki/ata.h>
#endif
#define PAYLOAD 480u
static struct blkdev *device;
static uint64_t first_sector, sequence;
static unsigned pending_head,pending_used,replay_used;
static char pending[65536], replay[BLACKBOX_REPLAY_BYTES+1];
static bool flushing;
#ifdef FS_HOST
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static uint32_t lock(void) { pthread_mutex_lock(&guard);return 0; }
static void unlock(uint32_t f) { (void)f;pthread_mutex_unlock(&guard); }
#else
static uint32_t lock(void) { return irq_save(); }
static void unlock(uint32_t f) { irq_restore(f); }
#endif
static uint32_t crc(const uint8_t *b, unsigned n) {
    uint32_t x=~0u;
    for (unsigned i=0;i<n;i++) { x^=b[i];for (unsigned j=0;j<8;j++) x=(x>>1)^(0xedb88320u&-(x&1)); }
    return ~x;
}
static uint64_t serial_number(const uint8_t *b) { return (uint64_t)fs_rd32(b+4)|((uint64_t)fs_rd32(b+8)<<32); }
static bool record(const uint8_t *b) {
    return !memcmp(b,"CBR1",4) && serial_number(b) && fs_rd16(b+12)<=PAYLOAD &&
        fs_rd32(b+508)==crc(b,508);
}
static void encode(uint8_t b[512], uint64_t seq, const char *s, unsigned n) {
    memset(b,0,512);memcpy(b,"CBR1",4);fs_wr32(b+4,(uint32_t)seq);fs_wr32(b+8,(uint32_t)(seq>>32));
    fs_wr16(b+12,(uint16_t)n);memcpy(b+16,s,n);fs_wr32(b+508,crc(b,508));
}
void blackbox_reset(void) {
    device=0;first_sector=sequence=0;pending_head=pending_used=replay_used=0;flushing=false;
}
int blackbox_bind(struct blkdev *d, uint64_t start, uint64_t count,
                  void (*serial)(const char *, size_t)) {
    device=0;
    /* No implicit adoption of blank reserved sectors on foreign volumes. */
    uint8_t b[512],desc[512];
    if (!d || d->sector_size!=512 || blkdev_range(d,start,(uint32_t)(count>UINT32_MAX ? 0 : count))) return -FS_EINVAL;
    int e=d->read(d,start,1,b);if (e) return e;
    if (fs_rd16(b+11)!=512 || fs_rd16(b+14)!=BLACKBOX_RESERVED || fs_rd16(b+22) ||
        fs_rd16(b+48)!=1 || fs_rd16(b+50)!=6 || fs_rd16(b+510)!=0xaa55 || count<BLACKBOX_RESERVED)
        return -FS_ENOENT;
    if ((e=d->read(d,start+BLACKBOX_DESCRIPTOR,1,desc))) return e;
    if (memcmp(desc,"CBB1",4) || fs_rd32(desc+4)!=1 || fs_rd32(desc+8)!=BLACKBOX_FIRST ||
        fs_rd32(desc+12)!=BLACKBOX_SECTORS || fs_rd32(desc+508)!=crc(desc,508)) return -FS_ENOENT;
    uint64_t numbers[BLACKBOX_SECTORS]={0};
    uint64_t max=0;
    for (unsigned i=0;i<BLACKBOX_SECTORS;i++) {
        if ((e=d->read(d,start+BLACKBOX_FIRST+i,1,b))) return e;
        if (record(b) && (serial_number(b)-1)%BLACKBOX_SECTORS==i) {
            numbers[i]=serial_number(b);if (numbers[i]>max) max=numbers[i];
        }
    }
    replay_used=0;
    uint64_t low=max>=BLACKBOX_SECTORS ? max-BLACKBOX_SECTORS+1 : 1;
    for (uint64_t seq=low;seq && seq<=max;seq++) {
        unsigned i=(unsigned)((seq-1)%BLACKBOX_SECTORS);
        if (numbers[i]!=seq) continue;
        if ((e=d->read(d,start+BLACKBOX_FIRST+i,1,b))) return e;
        if (!record(b) || serial_number(b)!=seq) continue;
        unsigned n=fs_rd16(b+12);
        /* Each raw record is bounded; add a tag even to a continuation. */
        char line[512];unsigned used=0;memcpy(line,"[blackbox] ",11);used=11;
        for (unsigned j=0;j<n;j++) {
            uint8_t c=b[16+j];line[used++]=(c>=32 || c=='\n' || c=='\t') ? (char)c : '?';
            if (c=='\n' || j+1==n) {
                if (line[used-1]!='\n') line[used++]='\n';
                if (replay_used+used>(sizeof(replay)-1)) {
                    unsigned drop=0;
                    while (drop<replay_used && (replay_used-drop+used>(sizeof(replay)-1) || !drop || replay[drop-1]!='\n')) drop++;
                    memmove(replay,replay+drop,replay_used-drop);replay_used-=drop;
                }
                memcpy(replay+replay_used,line,used);replay_used+=used;
                if (serial) serial(line,used);
                memcpy(line,"[blackbox] ",11);used=11;
            }
        }
    }
    replay[replay_used]=0;
    device=d;first_sector=start+BLACKBOX_FIRST;sequence=max;
    return 0;
}
const char *blackbox_replay(size_t *n) { *n=replay_used;return replay; }
void blackbox_capture(const char *s, size_t n) {
    if (!s) return;
    while (n) {
        unsigned take=n>256 ? 256 : (unsigned)n;uint32_t f=lock();
        for (unsigned i=0;i<take;i++) {
            if (pending_used==sizeof(pending)) { pending_head=(pending_head+1)%sizeof(pending);pending_used--; }
            pending[(pending_head+pending_used)%sizeof(pending)]=s[i];pending_used++;
        }
        unlock(f);s+=take;n-=take;
    }
}
static int flush_records(unsigned budget) {
#ifndef FS_HOST
    if (!(read_eflags()&0x200)) return -FS_EBUSY;
#endif
    uint32_t f=lock();
    if (!device || flushing || !blkdev_durable(device)) { unlock(f);return -FS_EAGAIN; }
    if (!pending_used) { unlock(f);return 0; }
    flushing=true;unlock(f);
    int e=0;
    /* Bound a worker turn, even when producers keep appending. */
    for (unsigned turn=0;turn<budget;turn++) {
        char data[PAYLOAD];unsigned n;
        f=lock();n=pending_used>PAYLOAD ? PAYLOAD : pending_used;
        /* End at a record boundary whenever possible. */
        unsigned end=0;
        for (unsigned i=0;i<n;i++) { data[i]=pending[(pending_head+i)%sizeof(pending)];if (data[i]=='\n') end=i+1; }
        if (end) n=end;
        if (n) { pending_head=(pending_head+n)%sizeof(pending);pending_used-=n; }
        unlock(f);if (!n) break;
        uint8_t b[512];uint64_t seq=++sequence;
        encode(b,seq,data,n);
        e=device->write ? device->write(device,first_sector+(seq-1)%BLACKBOX_SECTORS,1,b) : -FS_EROFS;
        if (e) break;
    }
    if (!e && device->write_cache_state!=BLKDEV_CACHE_DISABLED)
        e=device->flush ? device->flush(device) : -FS_EROFS;
    f=lock();flushing=false;unlock(f);return e;
}
int blackbox_flush(void) { return flush_records(8); }
bool blackbox_available(void) {
    uint32_t f=lock();bool ready=device && blkdev_durable(device);unlock(f);return ready;
}
int blackbox_checkpoint(void) {
#ifndef FS_HOST
    if (!(read_eflags()&0x200)) return -FS_EBUSY;
#endif
    /* Shutdown/refusal must not race the worker's last raw transaction.
     * Wait without storage locks owned by this module, with a finite bound.
     * An older volume without a descriptor has no checkpoint obligation. */
    uint32_t start=fs_now_ms();
    for (unsigned turn=0;turn<2000;turn++) {
        uint32_t f=lock();bool available=device!=0,busy=flushing;
        bool durable=available && blkdev_durable(device);
        unlock(f);
        if (!available) return 0;
        if (!durable) return -FS_EROFS;
        if (!busy) {
            int e=flush_records(512);
            if (e!=-FS_EAGAIN) return e;
        }
        if ((uint32_t)(fs_now_ms()-start)>=2000) break;
#ifdef FS_HOST
        const struct timespec pause={0,1000000};nanosleep(&pause,0);
#else
        task_sleep_ms(1);
#endif
    }
    return -FS_EBUSY;
}
void blackbox_panic(const char *s,size_t n) {
#ifndef FS_HOST
    if (read_eflags()&0x200) return;
#endif
    /* Never wait for a filesystem/cache/mutex owner interrupted by panic.
     * The emergency device boundary admits only an idle ATA channel. */
    if (!device || flushing || !blkdev_durable(device) || !device->emergency_write) return;
    uint8_t b[512];unsigned take=n>PAYLOAD ? PAYLOAD : (unsigned)n;
    uint64_t seq=++sequence;encode(b,seq,s,take);
    device->emergency_write(device,first_sector+(seq-1)%BLACKBOX_SECTORS,b);
}
