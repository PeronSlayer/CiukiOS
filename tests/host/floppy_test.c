/* Native FDC register/DMA, ownership and failure qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "floppy_fixture.h"
int main(void)
{
    struct fdc_evidence ev;
    assert(fdc_discover(&ev,0,0x40)==-FS_ENOENT);
    assert(fdc_discover(&ev,1,0x60)==-FS_EOPNOTSUPP);
    assert(!fdc_discover(&ev,1,0x40));
    assert(fdc_pnp(&ev,"PNP0700",0x1000003f0ull,8,6,2)==-FS_EOPNOTSUPP);
    assert(!fdc_pnp(&ev,"PNP0700",0x3f0,8,6,2));
    for(unsigned type=2;type<=5;type++) {
        struct floppy_fixture *m=calloc(1,sizeof(*m));assert(m);assert(!fixture_open(m,type));
        assert(m->fdc.claim_count==6 && m->irq_enabled);
        struct fdc conflict;
        unsigned touched=m->writes;
        assert(fdc_attach(&conflict,&m->fdc.io,&m->fdc.evidence,m->bounce,0x200000,&m->pin)==-FS_EBUSY);
        assert(m->writes==touched && m->irq_enabled);
        struct mm_pin bad=m->pin;bad.boundary=0;
        assert(fdc_attach(&conflict,&m->fdc.io,&m->fdc.evidence,m->bounce,0x200000,&bad)==-FS_EINVAL);
        assert(m->writes==touched);
        struct blkdev *b;assert(!fdc_native_open(&m->fdc,0,&b));
        uint64_t warm=m->now;unsigned reset_count=m->resets;uint8_t probe[512];
        assert(!b->read(b,0,1,probe));assert(m->resets==reset_count && m->now-warm<1000);
        struct fdc_lease second={0};assert(fdc_acquire(&m->fdc,0,2,1,&second)==-FS_EBUSY);
        struct fdc_lease lease=m->fdc.drive[0].native,stale=lease;
        uint8_t data[1024],got[1024];for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*37+type);
        assert(!b->write(b,b->capacity-2,2,data));assert(!b->read(b,b->capacity-2,2,got));assert(!memcmp(data,got,sizeof(data)));
        struct blkdev *drive_b;assert(!fdc_native_open(&m->fdc,1,&drive_b));
        assert(!drive_b->write(drive_b,0,1,data));assert(!drive_b->read(drive_b,0,1,got));assert(!memcmp(data,got,512));
        assert(!b->read(b,0,1,got));for(unsigned i=0;i<512;i++)assert(!got[i]);
        assert(!fdc_native_close(&m->fdc,1));
        assert(!fdc_format(&lease,3,1,0xe5));
        assert(!b->read(b,(3*2+1)*m->fdc.drive[0].geometry.sectors,1,got));assert(got[0]==0xe5 && got[511]==0xe5);
        unsigned writes=m->data_writes;m->st1=2;
        assert(b->write(b,0,1,data)==-FS_EROFS && m->data_writes==writes);m->st1=0;
        m->transient_errors=2;assert(!b->read(b,0,1,got));assert(m->fdc.retries>=2);
        m->st1=1;unsigned programs=m->dma_programs;assert(b->read(b,0,1,got)==-FS_EIO);assert(m->dma_programs==programs+3);m->st1=0;
        m->st2=0x20;assert(b->read(b,0,1,got)==-FS_EIO);m->st2=0;
        memset(got,0xa5,512);m->short_dma=true;assert(b->read(b,0,1,got)==-FS_EIO);m->short_dma=false;
        for(unsigned i=0;i<512;i++)assert(got[i]==0xa5);
        touched=m->writes;m->cancel=true;assert(b->read(b,0,1,got)==-VFS_ECANCELLED);assert(m->writes==touched);m->cancel=false;
        uint32_t generation=b->media_generation;m->changed=true;
        assert(b->media_check(b)==-FS_EAGAIN && b->media_generation==generation+1);
        assert(b->media_check(b)==-FS_EAGAIN && b->media_generation==generation+1);
        assert(b->read(b,0,1,got)==-FS_EAGAIN);
        assert(!fdc_revalidate(&lease,type));assert(!b->read(b,0,1,got));
        unsigned before=m->writes;stale.serial++;assert(fdc_transfer(&stale,0,1,got,false)==-FS_EACCES);assert(m->writes==before);
        assert(fdc_detach(&m->fdc)==-FS_EBUSY);
        assert(!fdc_native_close(&m->fdc,0));assert(fdc_release(&lease)==-FS_EACCES);
        assert(!fdc_acquire(&m->fdc,0,7,9,&second));assert(fdc_release(&lease)==-FS_EACCES);assert(!fdc_release(&second));
        m->now+=2001;fdc_idle(&m->fdc);assert(!(m->dor&0xf0) && (m->mask&4));
        fixture_close(m);free(m);
    }
    for(unsigned type=2;type<=5;type++) {
        struct floppy_fixture *blank=calloc(1,sizeof(*blank));assert(blank);assert(!fixture_open(blank,type));
        struct fdc_lease lease;assert(!fdc_acquire(&blank->fdc,0,7,1,&lease));
        blank->blank=blank->changed=true;
        assert(fdc_revalidate(&lease,type)==-FS_EIO);
        blank->absent=blank->changed=true;
        assert(fdc_format(&lease,0,0,0xf6)==-FS_EIO && !blank->formats);
        blank->absent=false;blank->st1=2;
        assert(fdc_format(&lease,0,0,0xf6)==-FS_EROFS && !blank->formats);
        blank->st1=0;assert(!fdc_format(&lease,0,0,0xf6) && blank->formats==1);
        uint8_t data[512];assert(!fdc_transfer(&lease,0,1,data,false));
        for(unsigned i=0;i<512;i++)assert(data[i]==0xf6);
        assert(!fdc_release(&lease));fixture_close(blank);free(blank);
    }
    struct floppy_fixture *m=calloc(1,sizeof(*m));assert(m);assert(!fixture_open(m,4));
    struct blkdev *b;assert(!fdc_native_open(&m->fdc,0,&b));uint8_t data[512];
    m->suppress_irq=true;uint64_t now=m->now;assert(b->read(b,0,1,data)==-FS_EIO);assert(m->now-now<=8000);assert(m->mask&4);m->suppress_irq=false;
    m->stuck_mask=true;assert(b->read(b,0,1,data)==-FS_EQUARANTINED);assert(b->quarantined && m->fdc.quarantined && !m->irq_enabled);
    assert(!fdc_native_close(&m->fdc,0));assert(fdc_detach(&m->fdc)==-FS_EQUARANTINED);free(m->disk);free(m->disk_b);free(m);
    puts("floppy: PASS (rates, DMA, blank/absent/protected format, retries, media, exclusive leases, timeout, quarantine)");
    fflush(stdout);
}
