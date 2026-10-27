/* SPDX-License-Identifier: MIT
 * Production surface/input grants and read-only inventory. All ordinary
 * application writes are checked against current USB FAT provenance; ATA
 * mutation stays exclusively inside the real setup_txn_execute backend. */
#include "native.h"
#include "setup_info.h"
#include "../accounts/production.h"   /* sec-user-02: acct_io_posix for the account repair */
#include "../../sdk/libdisk/include/libdisk.h"
#include "../../sdk/sysroot-overlay/include/ciuki/devices.h"
#include "../../sdk/sysroot-overlay/include/ciuki/display.h"
#include <ciuki/media.h>
#include "../../sdk/sysroot-overlay/include/ciuki/net.h"
#include "../../sdk/sysroot-overlay/include/ciuki/power.h"
#include "../../sdk/sysroot-overlay/include/ciuki/entropy.h"
#include <ciuki/proc.h>
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <poll.h>
#include <ciuki/surface.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>
#ifndef ST_NATIVE_ROOT
#define ST_NATIVE_ROOT ""
#endif
static struct {
    int display, input, surface, media;
    unsigned width,height,bpp; size_t mapped;
    uint32_t *pixels,portrait[256*256];
    struct disk_list disks;
    unsigned cd_slot; uint32_t cd_generation;
    int mouse_x,mouse_y;
    uint32_t export_generation[26], export_pinned;
} N;
static int path(char *out,size_t cap,const char *dos)
{
    if(strlen(dos)<3 || dos[0]<'C'||dos[0]>'Z'||dos[1]!=':'||(dos[2]!='\\'&&dos[2]!='/'))return -EINVAL;
    int n=dos[0]=='C'?snprintf(out,cap,ST_NATIVE_ROOT "/"):snprintf(out,cap,ST_NATIVE_ROOT "/mnt/%c/",dos[0]-'A'+'a');
    if(n<0||(size_t)n>=cap)return -ENAMETOOLONG;
    size_t used=(size_t)n;const char *p=dos+3;
    while(*p){
        const char *s=p;while(*p&&*p!='\\'&&*p!='/')p++;
        size_t k=(size_t)(p-s);
        if(!k||(k==1&&s[0]=='.')||(k==2&&s[0]=='.'&&s[1]=='.')||memchr(s,':',k))return -EINVAL;
        if(used+k+2>cap)return -ENAMETOOLONG;
        memcpy(out+used,s,k);used+=k;if(*p){out[used++]='/';p++;}
    }
    out[used]=0;return 0;
}
static int read_file(void *ctx,const char *dos,char *buf,size_t cap,size_t *length)
{
    (void)ctx;char name[512];int r=path(name,sizeof(name),dos);if(r)return r;
    int fd=open(name,O_RDONLY);if(fd<0)return -errno;
    size_t used=0;
    while(used<cap){ssize_t n=read(fd,buf+used,cap-used);if(n<0&&errno==EINTR)continue;if(n<0){r=-errno;break;}if(!n)break;used+=(size_t)n;}
    if(!r&&used==cap){char extra;ssize_t n;do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);if(n>0)r=-EFBIG;else if(n<0)r=-errno;}
    if(close(fd)&&!r)r=-errno;
    *length=used;return r;
}
static int usb(const char *dos)
{
    char converted[512];if(path(converted,sizeof(converted),dos))return 0;
    unsigned letter=(unsigned)(dos[0]-'A');
    struct ciuki_media_state state={.version=CIUKI_MEDIA_VERSION,.size=sizeof(state)};
    if(N.media<0||ciuki_devctl(N.media,MEDIA_GET_STATE,&state,sizeof(state)) ||
       !(state.removable_mask&(1u<<letter)) || state.type[letter]!=CIUKI_MEDIA_USB)return 0;
    /* D: is reserved for the boot ISO even if a broken inventory claims it. */
    if(letter<4)return 0;
    if(N.export_pinned&(1u<<letter))return state.generation[letter]==N.export_generation[letter];
    N.export_pinned|=1u<<letter;N.export_generation[letter]=state.generation[letter];
    return 1;
}
static int write_all(int fd,const char *buf,size_t n)
{
    while(n){ssize_t k=write(fd,buf,n);if(k<0&&errno==EINTR)continue;if(k<=0)return k<0?-errno:-EIO;buf+=k;n-=(size_t)k;}
    return 0;
}
static int write_file(void *ctx,const char *dos,const char *buf,size_t n)
{
    (void)ctx;if(!usb(dos))return -EROFS;
    char name[512];int r=path(name,sizeof(name),dos);if(r)return r;
    int fd=open(name,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -errno;
    r=write_all(fd,buf,n);if(!r&&!usb(dos))r=-ENXIO;if(!r&&fsync(fd))r=-errno;if(close(fd)&&!r)r=-errno;return r;
}
static int make_dir(void *ctx,const char *dos)
{
    (void)ctx;
    if(!usb(dos))return -EROFS;
    char name[512];int r=path(name,sizeof(name),dos);if(r)return r;
    return mkdir(name,0700)?-errno:0;
}
static int copy_file(void *ctx,const char *from,const char *to,char *buffer,size_t cap)
{
    (void)ctx;if(!usb(to))return -EROFS;
    char source[512],dest[512];int r=path(source,sizeof(source),from);if(r)return r;r=path(dest,sizeof(dest),to);if(r)return r;
    int in=open(source,O_RDONLY);if(in<0)return -errno;
    int out=open(dest,O_WRONLY|O_CREAT|O_TRUNC,0600);if(out<0){r=-errno;close(in);return r;}
    for(;;){ssize_t n=read(in,buffer,cap);if(n<0&&errno==EINTR)continue;if(n<=0){if(n<0)r=-errno;break;}
        if(!usb(to)){r=-ENXIO;break;}
        r=write_all(out,buffer,(size_t)n);if(r)break;}
    if(!r&&!usb(to))r=-ENXIO;
    if(!r&&fsync(out))r=-errno;
    if(close(out)&&!r)r=-errno;if(close(in)&&!r)r=-errno;return r;
}
static int list_dir(void *ctx,const char *dos,char (*names)[64],unsigned cap)
{
    (void)ctx;char name[512];int r=path(name,sizeof(name),dos);if(r)return r;
    DIR *d=opendir(name);if(!d)return -errno;unsigned count=0;struct dirent *e;
    while((e=readdir(d))){if(e->d_name[0]=='.')continue;
        if(count==cap||strlen(e->d_name)>=64){closedir(d);return -EOVERFLOW;}
        snprintf(names[count++],64,"%s",e->d_name);}
    closedir(d);return (int)count;
}
/* Headless hardware report (report_link.h): the same recipe as the desktop's
 * spawn_demo for a child's channel on descriptor 3. The program sends its text
 * over the channel; no window server is involved. */
/* <sys/wait.h> and <signal.h> clash with abi.h under the host test; abi.h supplies WNOHANG and SIGKILL. */
int waitpid(int,int *,int);
static int link_pair(void *c,int fds[2]){(void)c;return ciuki_channel_pair(fds)?-errno:0;}
static int link_spawn(void *c,const char *path,char *const argv[],int child_end,int child_fd)
{
    (void)c;
    /* channel_pair creates CLOEXEC endpoints and the explicit spawn mapping
     * rejects CLOEXEC sources: admit only the child's end (spawn_demo). */
    if(fcntl(child_end,F_SETFD,0))return -errno;
    char *env[]={"LC_ALL=C","TZ=UTC0",NULL};
    struct ciuki_spawn_fd mapping={child_end,child_fd};
    ciuki_pid_t pid=ciuki_spawn(path,argv,env,&mapping,1,0);
    return pid<0?-errno:(int)pid;
}
static int link_wait(void *c,int fd,unsigned ms)
{
    (void)c;struct pollfd p={fd,POLLIN,0};
    int r=poll(&p,1,(int)ms);
    return r<0?-errno:r?1:0;
}
static int link_recv(void *c,int fd,uint8_t data[256],size_t *bytes)
{
    (void)c;struct ciuki_message m;
    int r=ciuki_channel_recv(fd,&m,DONTWAIT);
    if(r<0)return -errno;
    if(!r)return 0;
    for(unsigned i=0;i<m.fd_count&&i<4;i++)close(m.fds[i]);   /* the program never sends descriptors */
    if(m.length>sizeof(m.data)||m.fd_count)return -EILSEQ;
    memcpy(data,m.data,m.length);*bytes=m.length;return 1;
}
static int link_reap(void *c,int pid,int *status)
{(void)c;int r=waitpid(pid,status,WNOHANG);return r<0?-errno:r==pid?1:0;}
static int link_kill(void *c,int pid){(void)c;return ciuki_proc_terminate((ciuki_pid_t)pid,SIGKILL,-1)?-errno:0;}
static void link_sleep(void *c,unsigned ms){(void)c;struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000L};nanosleep(&t,NULL);}
static void link_close(void *c,int fd){(void)c;close(fd);}
static uint64_t link_now(void *c){(void)c;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;}
static int report_app(void *ctx,char *buffer,size_t cap,size_t *length,char reason[24])
{
    const struct st_link_io io={ctx,link_pair,link_spawn,link_wait,link_recv,link_reap,link_kill,link_sleep,link_close,link_now};
    return st_link_report(&io,buffer,cap,length,reason);
}
static uint64_t monotonic(void *c){(void)c;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+(unsigned)t.tv_nsec;}
static int64_t realtime(void *c){(void)c;return (int64_t)time(NULL);}
static void idle(void *c,uint64_t ns){(void)c;struct timespec t={.tv_sec=(time_t)(ns/1000000000ull),.tv_nsec=(long)(ns%1000000000ull)};nanosleep(&t,NULL);}
static int input(void *c,struct ciuki_input_event *e,unsigned cap)
{
    (void)c;int n=ciuki_input_read(N.input,e,cap);if(n<0&&(errno==EAGAIN||errno==EINTR))return 0;
    /* Input grants carry relative motion; widget coordinates are local to
     * the centred design canvas. Clamp exactly as the desktop does. */
    for(int i=0;i<n;i++)if(e[i].type==CIUKI_INPUT_MOTION){
        N.mouse_x+=e[i].value;N.mouse_y+=e[i].value2;
        if(N.mouse_x<0)N.mouse_x=0;if(N.mouse_x>=ST_W)N.mouse_x=ST_W-1;
        if(N.mouse_y<0)N.mouse_y=0;if(N.mouse_y>=ST_H)N.mouse_y=ST_H-1;
        e[i].value=N.mouse_x;e[i].value2=N.mouse_y;
    }
    return n;
}
static void record(void *c,const char *text){(void)c;char line[512];int n=snprintf(line,sizeof(line),"%.500s\n",text);if(n>0)write_all(2,line,(size_t)n);}
static void present(void *c,const uint32_t *pixels,int width,int height)
{
    (void)c;unsigned ox=(N.width-(unsigned)width)/2,oy=(N.height-(unsigned)height)/2;
    for(unsigned y=0;y<N.height;y++)for(unsigned x=0;x<N.width;x++)N.pixels[(size_t)y*N.width+x]=ST_BACKGROUND;
    for(int y=0;y<height;y++)memcpy(N.pixels+(size_t)(oy+(unsigned)y)*N.width+ox,pixels+(size_t)y*(unsigned)width,(size_t)width*4);
    for(unsigned y=0;y<12;y++)for(unsigned x=0;x<=y/2;x++){
        unsigned px=ox+(unsigned)N.mouse_x+x,py=oy+(unsigned)N.mouse_y+y;
        if(px<N.width&&py<N.height)N.pixels[(size_t)py*N.width+px]=(!x||x==y/2)?0:0xffffffu;
    }
    struct ciuki_rect rect={.src_x=0,.src_y=0,.dst_x=0,.dst_y=0,.width=N.width,.height=N.height};
    if(ciuki_present(N.display,N.surface,&rect))record(NULL,"[setup] display present failed");
}
static int devices(void *c,struct ciuki_devices_info *v){(void)c;return ciuki_devices(v);}
static int memory(void *c,uint64_t *k){(void)c;struct ciuki_proc_system s={0};int r=ciuki_proc_list(&s,sizeof(s));if(r<0)return -1;*k=(uint64_t)s.usable_pages*4;return 0;}
static int display(void *c,struct ciuki_display_info *v){(void)c;return ciuki_display_query(v);}
static int framebuffer(void *c,unsigned *w,unsigned *h,unsigned *b){(void)c;*w=N.width;*h=N.height;*b=N.bpp;return 0;}
static int network(void *c,struct ciuki_net_state *v){(void)c;return ciuki_net_status(v);}
static int disks(void *c,struct st_disk *out,unsigned cap)
{
    (void)c;int r=disk_enumerate(&N.disks);if(r)return r;
    unsigned used=0;
    for(unsigned i=0;i<DISK_SLOTS&&used<cap;i++){
        const struct disk_entry *e=&N.disks.disk[i];if(!e->present)continue;
        struct st_disk *d=&out[used++];memset(d,0,sizeof(*d));d->present=1;d->slot=(uint8_t)(i/2);d->unit=(uint8_t)(i%2);
        memcpy(d->model,e->id.model,sizeof(d->model));memcpy(d->serial,e->id.serial,sizeof(d->serial));
        d->sectors=e->id.sectors;d->sector_size=e->id.sector_size;memcpy(d->fingerprint,e->id.table_fingerprint,32);
        d->occupied=e->partitions>0||e->volumes>0||e->setup_incomplete;d->installable=e->installable;snprintf(d->refusal,sizeof(d->refusal),"%s",disk_refusal_text(e->refusal));
        d->part_count=e->partitions;
        for(unsigned p=0;p<d->part_count&&p<ST_PARTS;p++){
            const struct disk_partition *q=&e->partition[p];struct st_part *v=&d->parts[p];
            v->type=q->type;v->start=q->start;v->count=q->count;v->active=q->bootable;v->logical=q->logical;
            for(unsigned k=0;k<e->volumes;k++)if(e->volume[k].partition==p+1){
                const struct disk_volume *vol=&e->volume[k];v->letter=vol->letter;memcpy(v->label,vol->label,sizeof(v->label));
                snprintf(v->fs,sizeof(v->fs),vol->fs==7?"NTFS":"FAT%u",vol->fs);
                char name[64],state[1024];size_t length=0;snprintf(name,sizeof(name),"%c:\\SYSTEM\\SETUP.STATE",vol->letter);
                if(!read_file(NULL,name,state,sizeof(state)-1,&length)){
                    state[length]=0;const char *phase=strstr(state,"phase=installed"),*sha=strstr(state,"release=");
                    if(phase && (phase==state||phase[-1]=='\n'||phase[-1]==' '))d->install=ST_INSTALL_DONE;
                    else d->install=ST_INSTALL_INCOMPLETE;
                    if(sha){sha+=8;unsigned n=0;while(n<64&&((sha[n]>='0'&&sha[n]<='9')||(sha[n]>='a'&&sha[n]<='f')))n++;if(n==64){memcpy(d->release,sha,64);d->release[64]=0;}}
                }
            }
        }
        if(e->setup_incomplete)d->install=ST_INSTALL_INCOMPLETE;
    }
    return (int)used;
}
static int volumes(void *c,struct st_volume *out,unsigned cap)
{
    (void)c;struct ciuki_media_state s={.version=CIUKI_MEDIA_VERSION,.size=sizeof(s)};
    if(N.media<0||ciuki_devctl(N.media,MEDIA_GET_STATE,&s,sizeof(s)))memset(&s,0,sizeof(s));
    unsigned n=0;for(unsigned letter=2;letter<26&&n<cap;letter++){
        char name[64];snprintf(name,sizeof(name),"%c:\\",'A'+letter);char unix_path[512];struct stat statbuf;
        if(path(unix_path,sizeof(unix_path),name)||stat(unix_path,&statbuf)||!S_ISDIR(statbuf.st_mode))continue;
        struct st_volume *v=&out[n++];memset(v,0,sizeof(*v));v->letter=(char)('A'+letter);
        v->usb=(s.removable_mask&(1u<<letter))&&s.type[letter]==CIUKI_MEDIA_USB;v->writable=v->usb;
        if(v->usb)strcpy(v->fs,"FAT"); /* media v2 reports FAT provenance, not its subtype */
        struct ciuki_media_volume info={.version=CIUKI_MEDIA_VERSION,.size=sizeof(info),.drive=letter};
        if(N.media>=0&&!ciuki_devctl(N.media,MEDIA_GET_VOLUME,&info,sizeof(info)))snprintf(v->label,sizeof(v->label),"%s",info.label);
    }
    return (int)n;
}
static int cd(void *c,struct st_cd *v)
{
    (void)c;memset(v,0,sizeof(*v));if(N.media<0)return -ENODEV;
    for(unsigned slot=0;slot<4;slot++){
        struct ciuki_media_cd_state s={.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(s),.slot=slot};
        if(ciuki_devctl(N.media,MEDIA_CD_STATE,&s,sizeof(s))||!s.present||!s.mounted||s.drive!=3)continue;
        N.cd_slot=slot;N.cd_generation=s.generation;v->present=1;v->ready=s.state==CIUKI_CD_READY;v->slot=(uint8_t)(slot/2);v->unit=(uint8_t)(slot%2);v->generation=s.generation;
        struct ciuki_media_cd_info info={.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(info),.slot=slot,.generation=s.generation};
        if(!ciuki_devctl(N.media,MEDIA_CD_INFO,&info,sizeof(info)))memcpy(v->model,info.model,sizeof(v->model));return 0;
    }
    return -ENODEV;
}
static int catalogue(void *c,struct st_size *out){(void)c;FILE *f=fopen(ST_NATIVE_ROOT "/mnt/d/CDMANIFE.JSN","rb");if(!f)return -errno;int r=st_manifest_sizes(f,out);fclose(f);return r;}
static int release(void *c,char *out,size_t cap){(void)c;static char data[4096];size_t n=0;struct setup_release r;int e=read_file(NULL,"C:\\SYSTEM\\RELEASE.JSON",data,sizeof(data),&n);if(e)return e;if(setup_release_parse(data,n,&r))return -EINVAL;snprintf(out,cap,"%s",r.content_digest);return 0;}
static int eject(void *c){(void)c;struct ciuki_media_cd_tray t={.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(t),.slot=N.cd_slot,.generation=N.cd_generation,.action=CIUKI_CD_TRAY_OPEN};return ciuki_devctl(N.media,MEDIA_CD_TRAY,&t,sizeof(t));}
static int media_present(void *c){(void)c;struct ciuki_media_cd_state s={.version=CIUKI_MEDIA_CD_VERSION,.size=sizeof(s),.slot=N.cd_slot};if(N.media<0||ciuki_devctl(N.media,MEDIA_CD_STATE,&s,sizeof(s)))return -1;N.cd_generation=s.generation;return s.state==CIUKI_CD_NO_MEDIUM?0:1;}
static int reboot(void *c){(void)c;return ciuki_power(CIUKI_POWER_REBOOT);}
static int power_off(void *c){(void)c;return ciuki_power(CIUKI_POWER_OFF);}
static int power_status(void *c,struct ciuki_power_status *s){(void)c;return ciuki_power_status(s)?-errno:0;}
static char dest_letter(void *c){(void)c;if(disk_enumerate(&N.disks))return 0;return N.disks.disk[0].volumes?N.disks.disk[0].volume[0].letter:0;}
/* sec-user-02: kernel entropy for salts (never a fallback), and the
 * protected account store of one identified installed volume for the
 * confirmed repair. The account service's own POSIX store binding is reused:
 * names stay below that volume's /System/Security, rename never replaces. */
static int entropy(void *c,void *out,size_t n){(void)c;return getentropy(out,n)?-errno:0;}
static struct acct_posix_io repair_state;
static int repair_io(void *c,char letter,struct acct_io *out)
{
    (void)c;if(letter<'E'||letter>'Z')return -EINVAL;
    char base[64];int n=snprintf(base,sizeof(base),ST_NATIVE_ROOT "/mnt/%c/System/Security",letter-'A'+'a');
    if(n<0||(size_t)n>=sizeof(base))return -ENAMETOOLONG;
    return acct_io_posix(&repair_state,base,out);
}
int st_native_open(struct st_host *h,int display_fd,int input_fd)
{
    memset(&N,0,sizeof(N));N.surface=N.media=-1;N.display=display_fd;N.input=input_fd;
    struct ciuki_framebuffer_info fb={.size=sizeof(fb)};
    if(ciuki_display_info(N.display,&fb))return -errno;
    if(fb.width<ST_W||fb.height<ST_H||fb.width>2048||fb.height>2048)return -ENOTSUP;
    N.width=fb.width;N.height=fb.height;N.bpp=fb.bpp;N.mouse_x=ST_W/2;N.mouse_y=ST_H/2;
    N.surface=ciuki_surface_create(N.width,N.height,CIUKI_SURFACE_XRGB8888);if(N.surface<0)return -errno;
    N.mapped=((size_t)N.width*N.height*4+4095)&~(size_t)4095;
    N.pixels=ciuki_surface_map(N.surface,PROT_READ|PROT_WRITE);
    if(N.pixels==MAP_FAILED){N.pixels=NULL;int e=-errno;st_native_close();return e;}
    N.media=open(ST_NATIVE_ROOT "/dev/media",O_RDONLY);
    size_t length=0;int r=read_file(NULL,"C:\\system\\assets\\ciuki-portrait.xrgb",(char *)N.portrait,sizeof(N.portrait),&length);
    if(r||length!=sizeof(N.portrait)){st_native_close();return r?r:-EINVAL;}
    *h=(struct st_host){.monotonic=monotonic,.realtime=realtime,.input=input,.present=present,.idle=idle,
        .devices=devices,.memory=memory,.display=display,.framebuffer=framebuffer,.network=network,.disks=disks,.volumes=volumes,.cd=cd,.catalogue=catalogue,.release=release,
        .read_file=read_file,.write_file=write_file,.copy_file=copy_file,.make_dir=make_dir,.list_dir=list_dir,.report_app=report_app,.record=record,.eject=eject,.media_present=media_present,.reboot=reboot,.power_off=power_off,.power_status=power_status,.dest_letter=dest_letter,.portrait=N.portrait,
        .entropy=entropy,.repair_io=repair_io};
    return 0;
}
void st_native_close(void){if(N.pixels){munmap(N.pixels,N.mapped);N.pixels=NULL;}if(N.surface>=0){close(N.surface);N.surface=-1;}if(N.media>=0){close(N.media);N.media=-1;}}
