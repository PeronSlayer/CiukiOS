/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <ciuki/devices.h>
#include <ciuki/display.h>
#include <ciuki/media.h>
#include <ciuki/power.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *names(const char *path)
{
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    char *out=NULL;
    if (!fseek(f,0,SEEK_END)) {
        long n=ftell(f);
        if (n>0 && n<=400*1024 && !fseek(f,0,SEEK_SET)) {
            out=malloc((size_t)n+1);
            if (out && fread(out,1,(size_t)n,f)==(size_t)n) out[n]=0;
            else { free(out); out=NULL; }
        }
    }
    fclose(f); return out;
}
static int device_name(void *ctx,unsigned bus,unsigned vendor,unsigned device,char *out,size_t cap)
{
    struct hr_native *n=ctx;
    const char *db=bus==CIUKI_BUS_USB ? n->usb_names : n->pci_names;
    if (!db || !cap) return -ENOENT;
    char key[16]; snprintf(key,sizeof(key),"\"%04x:%04x\"",vendor,device);
    const char *p=strstr(db,key); if (!p) return -ENOENT;
    p+=strlen(key); while (*p==' ' || *p==':') p++;
    if (*p++!='"') return -EINVAL;
    size_t i=0;
    while (p[i] && p[i]!='"' && i+1<cap) { out[i]=p[i]; i++; }
    if (p[i]!='"') return -EINVAL;
    out[i]=0; return 0;
}
static int firmware(void *ctx,struct ciuki_firmware_page *p)
{ (void)ctx; return ciuki_firmware_inventory(p) ? -errno : 0; }
static int read_span(int fd,char *out,size_t size,off_t offset,size_t *length)
{
    size_t done=0;
    while (done<size) {
        ssize_t got=pread(fd,out+done,size-done,offset+(off_t)done);
        if (got<0 && errno==EINTR) continue;
        if (got<0) return -errno;
        if (!got) return -EIO;
        done+=(size_t)got;
    }
    *length+=done; return 0;
}
static int read_log(char *out,size_t *length,int tail,const struct ciuki_hardware_info *h)
{
    *length=0;
    if (tail && (!h || !h->log_active)) return -EAGAIN;
    int fd=open("/system/logs/BOOT.LOG",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    struct stat st; int e=0;
    if (fstat(fd,&st)) e=-errno;
    else if (st.st_size<0) e=-EIO;
    else {
        off_t upper=st.st_size, lower=0;
        if (tail) {
            if (h->log_position>h->log_limit || h->log_ring_start>=h->log_limit) e=-EIO;
            if (upper>(off_t)h->log_position) upper=(off_t)h->log_position;
            if (upper>(off_t)h->log_ring_start) lower=(off_t)h->log_ring_start;
        }
        size_t take=(uint64_t)(upper-lower)>HR_LOG_MAX ? HR_LOG_MAX : (size_t)(upper-lower);
        off_t offset=tail ? upper-(off_t)take : 0;
        off_t first_offset=offset;
        if (!tail) take=(uint64_t)st.st_size>HR_LOG_MAX ? HR_LOG_MAX : (size_t)st.st_size;
        if (!e && tail && take<HR_LOG_MAX && lower && (uint64_t)st.st_size>=h->log_limit) {
            size_t before=HR_LOG_MAX-take;
            if (before>h->log_limit-h->log_position) before=h->log_limit-h->log_position;
            if (before) { first_offset=(off_t)h->log_limit-(off_t)before; e=read_span(fd,out,before,first_offset,length); }
        }
        if (!e && take) e=read_span(fd,out+*length,take,offset,length);
        /* Discard a partial first tail record; never interpret it as a full one. */
        char previous='\n'; size_t one=0;
        if (!e && tail && first_offset>0) e=read_span(fd,&previous,1,first_offset-1,&one);
        if (tail && previous!='\n' && *length) {
            size_t skip=0; while (skip<*length && out[skip]!='\n') skip++;
            if (skip<*length) skip++;
            memmove(out,out+skip,*length-skip); *length-=skip;
        }
    }
    if (close(fd) && !e) e=-errno;
    return e;
}
/* Read-only, bounded read of the guest build identity. A file larger than
 * HR_BUILD_MAX is -EFBIG (one extra byte is read to prove it), never truncated. */
static int read_build(char *out,size_t *length)
{
    *length=0;
    int fd=open("/system/BUILD.TXT",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int e=0; size_t used=0;
    while (used<HR_BUILD_MAX+1u) {
        ssize_t got=read(fd,out+used,HR_BUILD_MAX+1u-used);
        if (got<0 && errno==EINTR) continue;
        if (got<0) { e=-errno; break; }
        if (!got) break;
        used+=(size_t)got;
    }
    if (!e && used>HR_BUILD_MAX) e=-EFBIG;
    if (close(fd) && !e) e=-errno;
    if (!e) { out[used]=0; *length=used; }
    return e;
}
static void drives(struct hr_native *n)
{
    n->media=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(n->media)};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC);
    int known=fd>=0 && !ciuki_devctl(fd,MEDIA_GET_STATE,&n->media,sizeof(n->media));
    for (unsigned i=0;i<26;i++) {
        char root[16]; struct stat st;
        snprintf(root,sizeof(root),"/mnt/%c",'a'+i);
        if (stat(root,&st) || !S_ISDIR(st.st_mode) || !(st.st_mode&0222)) continue;
        if (known && n->media.type[i]==CIUKI_MEDIA_OPTICAL) continue;
        if (known && n->media.type[i]==CIUKI_MEDIA_USB && (n->media.removable_mask&(1u<<i))) {
            struct ciuki_media_volume v={.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=i};
            if (ciuki_devctl(fd,MEDIA_GET_VOLUME,&v,sizeof(v)) || !(v.flags&CIUKI_MEDIA_REMOVABLE)) continue;
            n->usb|=1u<<i;
        }
        if (!n->rescue || (n->usb&(1u<<i))) n->writable|=1u<<i;
    }
    if (fd>=0) close(fd);
    n->selected=hr_default_drive(n->writable,n->usb,n->rescue);
}
void hr_native_collect(struct hr_native *n)
{
    struct hr_source *s=&n->source; s->ctx=n; s->firmware=firmware; s->device_name=device_name;
    n->devices=(struct ciuki_devices_info){.version=1,.size=sizeof(n->devices)};
    if (!ciuki_devices(&n->devices)) s->devices=&n->devices;
    n->hardware=(struct ciuki_hardware_info){.version=CIUKI_HARDWARE_VERSION,.size=sizeof(n->hardware)};
    if (!ciuki_hardware_inventory(&n->hardware)) s->hardware=&n->hardware;
    n->display=(struct ciuki_display_info){.version=1,.size=sizeof(n->display)};
    if (!ciuki_display_query(&n->display)) s->display=&n->display;
    int fd=open("/dev/net",O_RDONLY|O_CLOEXEC);
    if (fd>=0) {
        n->network=(struct ciuki_net_state){.version=CIUKI_NET_VERSION,.size=sizeof(n->network)};
        if (!ciuki_devctl(fd,NET_GET_STATE,&n->network,sizeof(n->network))) s->network=&n->network;
        close(fd);
    }
    if (!ciuki_power_status(&n->power)) s->power=&n->power; /* fx-18: cached, no firmware call */
    n->pci_names=names("/system/hwids/pci-ids.json"); n->usb_names=names("/system/hwids/usb-ids.json");
    s->boot_error=read_log(n->boot,&s->boot_length,0,s->hardware); if (!s->boot_error) s->boot_log=n->boot;
    s->latest_error=read_log(n->latest,&s->latest_length,1,s->hardware); if (!s->latest_error) s->latest_log=n->latest;
    s->build_error=read_build(n->build,&n->build_length); if (!s->build_error) { s->build_txt=n->build; s->build_length=n->build_length; }
    drives(n);
}
void hr_native_free(struct hr_native *n)
{ free(n->pci_names); free(n->usb_names); free(n->full_logs); free(n->archive); }
int hr_native_prepare_logs(struct hr_native *n)
{
    static const char *const paths[]={"/system/logs/BOOT.LOG","/system/logs/BOOT1.LOG","/system/logs/BOOT2.LOG"};
    char *text=malloc(HR_FULL_LOG_MAX);
    if (!text) return -ENOMEM;
    size_t used=0; int e=0;
    for (unsigned i=0;i<3 && !e;i++) {
        FILE *f=fopen(paths[i],"rb");
        if (!f && (i==0 || errno!=ENOENT)) { e=-errno; break; }
        int bytes=snprintf(text+used,HR_FULL_LOG_MAX-used,"\n[%s - full privacy-filtered log]\n%s\n",i==0 ? "BOOT.LOG" : i==1 ? "BOOT1.LOG" : "BOOT2.LOG",f ? "" : "Not present");
        if (bytes<0 || (size_t)bytes>=HR_FULL_LOG_MAX-used) { if (f) fclose(f); e=-EFBIG; break; }
        used+=(size_t)bytes;
        if (!f) continue;
        /* Fixed whitelist, bounded whole-file scan, no user-selected inputs. */
        char line[2048]; size_t read_bytes=0,lines=0;
        while (!e && fgets(line,sizeof(line),f)) {
            size_t got=strlen(line),written=0; read_bytes+=got;
            if (read_bytes>16u*1024u*1024u || ++lines>65536) { e=-EFBIG; break; }
            e=hr_filter_log(line,got,text+used,HR_FULL_LOG_MAX-used,&written);
            if (e==-ENOSPC) e=-EFBIG;
            used+=written;
        }
        if (!e && ferror(f)) e=-EIO;
        if (fclose(f) && !e) e=-errno;
    }
    unsigned char *archive=NULL; size_t length=0;
    if (!e) e=hr_gzip(text,used,&archive,&length);
    if (e) { free(text); return e; }
    free(n->full_logs); free(n->archive);
    n->full_logs=text; n->full_length=used; n->archive=archive; n->archive_length=length;
    return 0;
}
static int io_open(void *ctx,const char *path)
{ (void)ctx; int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600); return fd<0 ? -errno : fd; }
static ssize_t io_write(void *ctx,int fd,const void *p,size_t n)
{ (void)ctx; ssize_t wrote=write(fd,p,n); return wrote<0 ? -errno : wrote; }
static int io_sync(void *ctx,int fd) { (void)ctx; return fsync(fd) ? -errno : 0; }
static int io_close(void *ctx,int fd) { (void)ctx; return close(fd) ? -errno : 0; }
static int io_rename(void *ctx,const char *a,const char *b) { (void)ctx; return rename(a,b) ? -errno : 0; }
static int present(void *ctx);
static int io_remove(void *ctx,const char *path)
{ int e=present(ctx); return e ? e : unlink(path) ? -errno : 0; }
static int present(void *ctx)
{
    struct hr_native *n=ctx; unsigned d=n->selected;
    if (d>=26 || !(n->writable&(1u<<d)) || (n->rescue && !(n->usb&(1u<<d)))) return -ENODEV;
    if (!(n->usb&(1u<<d))) return 0;
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    struct ciuki_media_state s={.version=CIUKI_MEDIA_VERSION,.size=sizeof(s)};
    int e=ciuki_devctl(fd,MEDIA_GET_STATE,&s,sizeof(s)) ? -errno : 0;
    if (!e && (!(s.removable_mask&(1u<<d)) || s.type[d]!=CIUKI_MEDIA_USB || s.generation[d]!=n->media.generation[d])) e=-ENODEV;
    close(fd); return e;
}
int hr_native_save(struct hr_native *n,const char *text,size_t length)
{
    char root[16]; snprintf(root,sizeof(root),"/mnt/%c",'a'+n->selected);
    struct hr_io io={n,io_open,io_write,io_sync,io_close,io_rename,io_remove,present};
    int e=hr_save(&io,root,text,length);
    n->archive_result=0;
    if (!e && n->archive) n->archive_result=hr_save_logs(&io,root,n->archive,n->archive_length);
    return e;
}
