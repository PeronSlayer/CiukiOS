/* SPDX-License-Identifier: MIT */
#include "desktop/replay_base.h"
#include <assert.h>
#include <stdarg.h>
static FILE *report_fopen(const char *,const char *);
static int report_open(const char *,int,...);
static int report_rename(const char *,const char *);
static int report_unlink(const char *);
#define fopen report_fopen
#define open report_open
#define rename report_rename
#define unlink report_unlink
#include "../../apps/hwreport/native.c"
#undef fopen
#undef open
#undef rename
#undef unlink
static const char *folder;
static unsigned media_queries, generation=7;
static void path(char out[4096],const char *in)
{
    const char *base=strrchr(in,'/'); assert(base);
    snprintf(out,4096,"%s/%s",folder,base+1);
}
static FILE *report_fopen(const char *name,const char *mode)
{ char p[4096]; path(p,name); return fopen(p,mode); }
static int report_open(const char *name,int flags,...)
{
    if (!strcmp(name,"/dev/media")) return open("/dev/null",O_RDONLY);
    mode_t mode=0;
    if (flags&O_CREAT) { va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap); }
    char p[4096]; path(p,name); return open(p,flags,mode);
}
static int report_rename(const char *a,const char *b)
{ char x[4096],y[4096]; path(x,a); path(y,b); return rename(x,y); }
static int report_unlink(const char *a)
{ char x[4096]; path(x,a); return unlink(x); }
/* sec-sysvc: the SYSTEM log service. 0: ENOSYS (an older kernel, the files
 * are read directly), 1: serves the folder's logs, 2: EPERM. */
static int log_service; static unsigned service_reads;
int ciuki_system_log_read(unsigned scope,const char *name,uint32_t offset,void *buffer,size_t capacity,size_t *got,uint32_t *total)
{
    assert(scope==CIUKI_SYSTEM_LOG_MACHINE && capacity<=CIUKI_SYSTEM_LOG_DATA_MAX && got && total);
    if (!log_service) { errno=ENOSYS; return -1; }
    if (log_service==2) { errno=EPERM; return -1; }
    char p[4096],in[64]; snprintf(in,sizeof(in),"/%s",name); path(p,in);
    FILE *f=fopen(p,"rb"); if (!f) { errno=ENOENT; return -1; }
    assert(!fseek(f,0,SEEK_END)); long size=ftell(f); assert(size>=0);
    *total=(uint32_t)size; *got=0;
    if (offset<(uint32_t)size && capacity) { assert(!fseek(f,(long)offset,SEEK_SET)); *got=fread(buffer,1,capacity,f); }
    fclose(f); service_reads++; return 0;
}
int ciuki_devctl(int fd,uint32_t op,void *out,uint32_t size)
{
    (void)fd; assert(op==MEDIA_GET_STATE && size==sizeof(struct ciuki_media_state)); media_queries++;
    struct ciuki_media_state *s=out; s->removable_mask=8; s->type[3]=CIUKI_MEDIA_USB; s->generation[3]=generation; return 0;
}
int main(int argc,char **argv)
{
    assert(argc==2); folder=argv[1]; char p[4096];
    path(p,"/BOOT.LOG"); FILE *f=fopen(p,"wb"); assert(f);
    for (unsigned i=0;i<28000;i++) assert(fputs("[ata] count=2 error=-5 user=private-user path=/private/file hostname=private-host address=10.1.2.3\n",f)>=0);
    assert(!fclose(f));
    path(p,"/BOOT1.LOG"); f=fopen(p,"wb"); assert(f && fputs("[cpu] pages=123 mac=00:01:02:03:04:05\n",f)>=0 && !fclose(f));
    struct hr_native *n=calloc(1,sizeof(*n)); assert(n);
    n->selected=3; n->usb=n->writable=8; n->media.generation[3]=7;
    assert(!hr_native_prepare_logs(n) && n->archive && n->full_length>HR_TEXT_MAX);
    assert(strstr(n->full_logs,"BOOT2.LOG") && strstr(n->full_logs,"Not present"));
    assert(!strstr(n->full_logs,"private") && !strstr(n->full_logs,"10.1.2.3") && !strstr(n->full_logs,"00:01:"));
    assert(!hr_native_save(n,"report bytes\n",13) && !n->archive_result && media_queries>=4);
    path(p,"/CIUKIREP.TXT"); f=fopen(p,"rb"); char got[32]={0}; assert(f && fread(got,1,13,f)==13 && !fclose(f) && !strcmp(got,"report bytes\n"));
    generation=8;
    assert(hr_native_save(n,"replacement\n",12)==-ENODEV);
    f=fopen(p,"rb"); memset(got,0,sizeof(got)); assert(f && fread(got,1,13,f)==13 && !fclose(f) && !strcmp(got,"report bytes\n"));
    n->rescue=1; n->selected=2; assert(hr_native_save(n,"replacement\n",12)==-ENODEV);
    n->selected=3; n->rescue=0; generation=7;
    /* Physical EOF contains old records once the disk log wraps. */
    path(p,"/BOOT.LOG"); f=fopen(p,"wb"); assert(f);
    char ring[1024]; memset(ring,'\n',sizeof(ring));
    memcpy(ring+128,"[ata] count=42\n",15); memcpy(ring+160,"[ata] count=41\n",15);
    assert(fwrite(ring,1,sizeof(ring),f)==sizeof(ring) && !fclose(f));
    struct ciuki_hardware_info h={.log_active=1,.log_position=160,.log_ring_start=128,.log_limit=1024};
    char recent[HR_LOG_MAX+1]; size_t recent_length=0;
    assert(!read_log(recent,&recent_length,1,&h) && recent_length==896);
    recent[recent_length]=0;
    assert(strstr(recent,"count=41")<strstr(recent,"count=42"));
    assert(!memcmp(recent+recent_length-32,"[ata] count=42\n",15));
    /* sec-sysvc: the same reads through the SYSTEM log service, which an
     * administrator uses on a managed system volume; EPERM otherwise. */
    {
        log_service=1; char through[HR_LOG_MAX+1]; size_t again=0;
        assert(!read_log(through,&again,1,&h) && again==recent_length && !memcmp(through,recent,again));
        assert(!hr_native_prepare_logs(n) && strstr(n->full_logs,"count=42") && strstr(n->full_logs,"[cpu] pages=123") && service_reads>=4);
        log_service=2; assert(read_log(through,&again,0,&h)==-EPERM && hr_native_prepare_logs(n)==-EPERM);
        log_service=0;
    }
    h.log_position=2048; assert(read_log(recent,&recent_length,1,&h)==-EIO);
    h.log_active=0; assert(read_log(recent,&recent_length,1,&h)==-EAGAIN);
    path(p,"/BOOT.LOG"); assert(!unlink(p)); assert(hr_native_prepare_logs(n)==-ENOENT);
    /* An existing temp file is never overwritten or deleted on an open failure. */
    path(p,"/CIUKIREP.TMP"); f=fopen(p,"wb"); assert(f && fputs("keep",f)>=0 && !fclose(f));
    assert(hr_native_save(n,"replacement\n",12)==-EEXIST);
    generation=8; assert(io_remove(n,"/mnt/d/CIUKIREP.TMP")==-ENODEV);
    f=fopen(p,"rb"); memset(got,0,sizeof(got)); assert(f && fread(got,1,4,f)==4 && !fclose(f) && !strcmp(got,"keep"));
    generation=7; assert(!unlink(p));
    path(p,"/CIUKILOG.TMP"); f=fopen(p,"wb"); assert(f && fputs("keep archive",f)>=0 && !fclose(f));
    assert(!hr_native_save(n,"replacement\n",12) && n->archive_result==-EEXIST);
    n->pci_names=strdup("{\"8086:1237\": \"PCI name\"}"); n->usb_names=strdup("{\"1234:0001\": \"USB name\"}");
    assert(n->pci_names && n->usb_names);
    char product[64]; assert(!device_name(n,1,0x8086,0x1237,product,sizeof(product)) && !strcmp(product,"PCI name"));
    assert(!device_name(n,3,0x1234,1,product,sizeof(product)) && !strcmp(product,"USB name"));
    assert(device_name(n,1,0,0,product,sizeof(product))==-ENOENT);
    /* /System/BUILD.TXT: missing, present, exactly at the bound, and oversized. */
    {
        char build[HR_BUILD_MAX+1]; size_t length=99;
        path(p,"/BUILD.TXT"); (void)unlink(p);
        assert(read_build(build,&length)==-ENOENT && !length);
        f=fopen(p,"wb"); assert(f && fputs("schema=1\nsource_commit=unknown\n",f)>=0 && !fclose(f));
        assert(!read_build(build,&length) && length==31 && !memcmp(build,"schema=1\n",9) && !build[length]);
        f=fopen(p,"wb"); assert(f); for (unsigned i=0;i<HR_BUILD_MAX;i++) assert(fputc('a',f)=='a'); assert(!fclose(f));
        assert(!read_build(build,&length) && length==HR_BUILD_MAX);
        f=fopen(p,"ab"); assert(f && fputc('a',f)=='a' && !fclose(f));
        assert(read_build(build,&length)==-EFBIG && !length);
        assert(!unlink(p));
    }
    hr_native_free(n); free(n);
    puts("PASS native report: complete sanitized log archive, absent rotated log, actual atomic files, USB generation/rescue checks, existing temp, hardware names, bounded BUILD.TXT read; SYSTEM log service reads");
}
