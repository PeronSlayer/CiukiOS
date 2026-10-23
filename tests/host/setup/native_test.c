/* SPDX-License-Identifier: MIT
 * Production adapter with syscall fixtures and real host VFS files. No ATA
 * mutation entry point is supplied: linking one would defeat this boundary. */
#include "../../../apps/setup/native.h"
#include "../../../sdk/libdisk/include/libdisk.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>
static uint32_t removable;
static unsigned power_calls, presents;
int ciuki_display_info(int fd,struct ciuki_framebuffer_info *v){assert(fd==61);v->width=800;v->height=600;v->bpp=32;return 0;}
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t fmt){assert(w==800&&h==600&&fmt==CIUKI_SURFACE_XRGB8888);return open("/dev/null",O_RDWR);}
void *ciuki_surface_map(int fd,int prot){(void)fd;int zero=open("/dev/zero",O_RDWR);assert(zero>=0);void *p=mmap(NULL,800*600*4,prot,MAP_PRIVATE,zero,0);close(zero);return p;}
int ciuki_present(int d,int s,const struct ciuki_rect *r){(void)s;assert(d==61&&r->width==800&&r->height==600);presents++;return 0;}
int ciuki_input_read(int fd,struct ciuki_input_event *e,uint32_t cap){(void)e;assert(fd==62&&cap);return 0;}
int ciuki_devices(struct ciuki_devices_info *d){d->count=0;return 0;}
int ciuki_display_query(struct ciuki_display_info *d){d->vbe_version=0x200;return 0;}
int ciuki_net_status(struct ciuki_net_state *s){s->present=0;return 0;}
int ciuki_power(uint32_t action){assert(action==CIUKI_POWER_REBOOT||action==CIUKI_POWER_OFF);power_calls++;return 0;}
int ciuki_power_status(struct ciuki_power_status *s){s->version=CIUKI_POWER_VERSION;s->size=sizeof(*s);s->shutdown_requests=5;return 0;}
int ciuki_proc_list(void *b,uint32_t cap){assert(cap==sizeof(struct ciuki_proc_system));((struct ciuki_proc_system *)b)->usable_pages=32768;return 0;}
int ciuki_devctl(int fd,uint32_t op,void *b,uint32_t cap){
    (void)fd;(void)cap;
    if(op==MEDIA_GET_STATE){struct ciuki_media_state *s=b;s->removable_mask=removable;s->type['F'-'A']=CIUKI_MEDIA_USB;s->type['D'-'A']=CIUKI_MEDIA_OPTICAL;return 0;}
    if(op==MEDIA_GET_VOLUME){struct ciuki_media_volume *v=b;if(v->drive==5)strcpy(v->label,"Rescue USB");return 0;}
    if(op==MEDIA_CD_STATE){struct ciuki_media_cd_state *s=b;if(s->slot!=2){errno=ENODEV;return -1;}s->present=1;s->mounted=1;s->drive=3;s->generation=7;s->state=CIUKI_CD_READY;return 0;}
    if(op==MEDIA_CD_INFO){struct ciuki_media_cd_info *s=b;assert(s->slot==2&&s->generation==7);strcpy(s->model,"Generic ATAPI");return 0;}
    if(op==MEDIA_CD_TRAY){struct ciuki_media_cd_tray *s=b;assert(s->slot==2&&s->generation==7&&s->action==CIUKI_CD_TRAY_OPEN);return 0;}
    errno=ENOTSUP;return -1;
}
const char *disk_refusal_text(enum disk_refusal r){(void)r;return "supported";}
int disk_enumerate(struct disk_list *v){
    memset(v,0,sizeof(*v));v->count=4;struct disk_entry *d=&v->disk[0];d->present=d->installable=1;
    strcpy(d->id.model,"Generic disk");strcpy(d->id.serial,"SERIAL");d->id.sector_size=512;d->id.sectors=16*1024*1024;
    d->partitions=d->volumes=1;d->partition[0]=(struct disk_partition){.type=12,.start=2048,.count=8*1024*1024};
    d->volume[0]=(struct disk_volume){.letter='E',.fs=32,.readonly=true,.partition=1};return 0;
}
static void put(const char *name,const char *bytes,size_t n){char p[1024];snprintf(p,sizeof(p),ST_NATIVE_ROOT "/%s",name);FILE *f=fopen(p,"wb");assert(f);assert(fwrite(bytes,1,n,f)==n);assert(!fclose(f));}
static void directory(const char *name){char p[1024];snprintf(p,sizeof(p),ST_NATIVE_ROOT "/%s",name);assert(!mkdir(p,0700));}
void native_tests(const uint32_t *portrait)
{
    directory("system");directory("system/assets");directory("SYSTEM");directory("dev");directory("mnt");directory("mnt/d");directory("mnt/e");directory("mnt/e/SYSTEM");directory("mnt/f");
    put("system/assets/ciuki-portrait.xrgb",(const char *)portrait,256*256*4);put("dev/media","",0);
    const char *state="phase=installed release=1111111111111111111111111111111111111111111111111111111111111111 firstboot=pending\n";
    put("mnt/e/SYSTEM/SETUP.STATE",state,strlen(state));
    const char *manifest="{\"boot_image\":{\"files\":[{\"component\":\"system\",\"path\":\"/System/VMM.ELF\",\"size\":4097},{\"component\":\"dos-unified\",\"path\":\"/DOS.IMG\",\"size\":1024}]},\"iso_tree\":[{\"path\":\"SOURCES/CIUKIOS.ZIP\",\"size\":8180}]}";
    put("mnt/d/CDMANIFE.JSN",manifest,strlen(manifest));
    struct st_host h;assert(!st_native_open(&h,61,62));removable=1u<<5;
    char data[4096];size_t n=0;
    assert(h.read_file(NULL,"E:\\SYSTEM\\SETUP.STATE",data,sizeof(data),&n)==0&&n==strlen(state));
    assert(h.read_file(NULL,"E:\\SYSTEM\\SETUP.STATE",data,4,&n)==-EFBIG);
    assert(h.write_file(NULL,"E:\\SYSTEM\\SETUP.STATE","bad",3)==-EROFS);
    assert(h.write_file(NULL,"F:\\..\\e\\SYSTEM\\SETUP.STATE","bad",3)==-EROFS);
    assert(h.write_file(NULL,"F:\\..\\..\\SYSTEM\\SETUP.STATE","bad",3)==-EROFS);
    assert(!h.write_file(NULL,"F:\\SETUP.STS","status=complete",15));
    removable=0;assert(h.write_file(NULL,"F:\\SETUP.STS","bad",3)==-EROFS);removable=1u<<5;
    char large[70000];memset(large,'q',sizeof(large));put("mnt/e/SYSTEM/BOOT.LOG",large,sizeof(large));
    assert(!h.copy_file(NULL,"E:\\SYSTEM\\BOOT.LOG","F:\\BOOT.LOG",data,sizeof(data)));
    char p[1024];snprintf(p,sizeof(p),ST_NATIVE_ROOT "/mnt/f/BOOT.LOG");FILE *f=fopen(p,"rb");assert(f);for(unsigned i=0;i<sizeof(large);i++)assert(fgetc(f)=='q');assert(fgetc(f)==EOF);fclose(f);
    assert(h.copy_file(NULL,"E:\\SYSTEM\\BOOT.LOG","E:\\SYSTEM\\COPY.LOG",data,sizeof(data))==-EROFS);
    assert(!h.read_file(NULL,"E:\\SYSTEM\\SETUP.STATE",data,sizeof(data),&n)&&n==strlen(state)&&!memcmp(data,state,n));
    struct st_size s[SETUP_COMP_COUNT];assert(!h.catalogue(NULL,s));assert(s[0].bytes==4097&&s[0].alloc_bytes==8192&&s[1].bytes==1024&&s[7].bytes==8180);
    struct st_disk d[4];assert(h.disks(NULL,d,4)==1&&d[0].install==ST_INSTALL_DONE&&strlen(d[0].release)==64);
    struct st_cd cd;assert(!h.cd(NULL,&cd)&&cd.slot==1&&cd.unit==0&&cd.ready);assert(!h.eject(NULL));assert(h.media_present(NULL)==1);
    struct st_volume v[26];int count=h.volumes(NULL,v,26);assert(count==4);int found=0;for(int i=0;i<count;i++)if(v[i].letter=='F'){assert(v[i].usb&&v[i].writable);found=1;}assert(found);
    uint64_t mem=0;assert(!h.memory(NULL,&mem)&&mem==128*1024);
    uint32_t *pixels=calloc(ST_W*ST_H,4);assert(pixels);h.present(NULL,pixels,ST_W,ST_H);free(pixels);assert(presents==1);
    assert(!h.reboot(NULL)&&power_calls==1);
    struct ciuki_power_status ps={0};assert(!h.power_status(NULL,&ps)&&ps.shutdown_requests==5&&ps.size==sizeof(ps));
    assert(!h.power_off(NULL)&&power_calls==2);st_native_close();
    const char *invalid[]={"{}","{\"x\":01}","{\"x\":1,}","{\"x\":true garbage}","{\"x\":\"unterminated}","{\"path\":\"/bad\",\"component\":\"system\",\"size\":18446744073709551615}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){FILE *in=tmpfile();assert(in);fputs(invalid[i],in);rewind(in);assert(st_manifest_sizes(in,s)<0);fclose(in);}
    puts("PASS production VFS adapter, USB provenance, streaming logs, manifest sizes, generic inventories");
}
