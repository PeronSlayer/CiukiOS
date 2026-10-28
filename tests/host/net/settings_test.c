/* Production settings loader/writer on a faultable bounded VFS seam.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/net_control.h>
#include <ciuki/ipx_config.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static char disk[NET_SETTINGS_BYTES],temporary[NET_SETTINGS_BYTES],last_record[256];
static size_t length,temp_length;
static int present,fail_read,fail_write,fail_commit,open_fd,tables,swapped,fail_rename;
static struct vfs volume;
static char ipx_text[14]="frame_type=4\n";
static int ipx_present;
static void reseal(char *text)
{
    char *end=strstr(text,"checksum=");assert(end);uint32_t h=2166136261u;
    for(char *p=text;p<end;p++)h=(h^(uint8_t)*p)*16777619u;
    snprintf(end,32,"checksum=%u\n",h);
}
static struct px_namespace space;
int net_control_driver(unsigned index,struct ciuki_net_control *r,bool apply)
{
    assert(!apply && index<2);r->pci_vendor=0x8086;r->pci_device=0x1229;
    r->mac[0]=2;r->mac[5]=(uint8_t)((swapped ? index^1u : index)+1);return 0;
}
void vfs_lock(struct vfs *v,uint32_t drives){assert(v==&volume && !drives);}
void vfs_unlock(struct vfs *v){assert(v==&volume);}
int files_attach(struct vfs *v,struct px_namespace **out){assert(v==&volume);space.root=(void *)1;*out=&space;return 0;}
int px_rename_locked(struct px_namespace *s,struct px_node *root,const char *from,const char *to)
{
    assert(s==&space && root==space.root && !strcmp(from,"/system/NETWORK.NEW") && !strcmp(to,"/system/NETWORK.INI"));
    if(fail_rename)return -EIO;memcpy(disk,temporary,temp_length);length=temp_length;present=1;return 0;
}
int vfs_flush_drive(struct vfs_table *t,unsigned drive){(void)t;assert(drive==2);return 0;}
void kmutex_init(struct kmutex *m){memset(m,0,sizeof(*m));}
void kmutex_lock(struct kmutex *m){(void)m;}
void kmutex_unlock(struct kmutex *m){(void)m;}
void klog(const char *format,...){va_list a;va_start(a,format);vsnprintf(last_record,sizeof(last_record),format,a);va_end(a);}
int px_error(int e){return e ? -EIO : 0;}
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{assert(v==&volume && drive==2 && !tables);memset(t,0,sizeof(*t));tables++;return 0;}
void vfs_table_destroy(struct vfs_table *t){(void)t;assert(tables==1 && !open_fd);tables--;}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    (void)t;(void)share;(void)attr;assert(!open_fd);
    if(!strcmp(path,"C:\\SYSTEM\\NETWORK.NEW")) { assert(flags&VFS_TRUNCATE);temp_length=0;open_fd=1;return 8; }
    if(!strcmp(path,"C:\\SYSTEM\\SETTINGS\\ipx.cfg")) { if(!ipx_present)return -FS_ENOENT;open_fd=1;return 9; }
    assert(!strcmp(path,"C:\\SYSTEM\\NETWORK.INI"));
    if(!present)return -FS_ENOENT;
    open_fd=1;return 7;
}
int vfs_close(struct vfs_table *t,int fd){(void)t;assert((fd==7 || fd==8 || fd==9) && open_fd);open_fd=0;return 0;}
int vfs_read(struct vfs_table *t,int fd,void *out,size_t cap,size_t *done)
{(void)t;if(fd==9){*done=cap<13?cap:13;memcpy(out,ipx_text,*done);return 0;}assert(fd==7);if(fail_read)return -FS_EIO;*done=length<cap ? length : cap;memcpy(out,disk,*done);return 0;}
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t bytes,size_t *done)
{(void)t;assert(fd==8 && bytes<sizeof(temporary));*done=fail_write ? bytes/2 : bytes;memcpy(temporary,data,*done);temp_length=*done;return 0;}
int vfs_commit(struct vfs_table *t,int fd){(void)t;assert(fd==8);return fail_commit ? -FS_EIO : 0;}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    volume.volumes[2]=(void *)1;
    net_settings_load(&volume);assert(strstr(last_record,"boot=missing fallback=dhcp"));
    struct ciuki_net_config c,got;net_settings_get(0,&c);assert(c.enabled && c.dhcp);
    c.dhcp=0;const uint8_t ip[]={192,0,2,110},mask[]={255,255,255,0},gw[]={192,0,2,1};
    memcpy(&c.address,ip,4);memcpy(&c.mask,mask,4);memcpy(&c.gateway,gw,4);c.dns[0]=c.gateway;
    memset(c.hostname,0,sizeof(c.hostname));strcpy(c.hostname,"ciuki");strcpy(c.domain,"example.test");assert(!net_settings_save(0,&c));
    assert(length && !tables && !open_fd);net_settings_load(&volume);assert(strstr(last_record,"boot=saved fallback=none"));
    net_settings_get(0,&got);assert(!memcmp(&got,&c,sizeof(c)));net_settings_get(1,&got);assert(got.dhcp);
    swapped=1;net_settings_get(1,&got);assert(!memcmp(&got,&c,sizeof(c)));net_settings_get(0,&got);assert(got.dhcp);swapped=0;
    char bad[NET_SETTINGS_BYTES];struct ciuki_net_config parsed[2];
    memcpy(bad,disk,length);bad[length]=0;char *field=strstr(bad,"enabled=");assert(field);memcpy(field,"unknown",7);reseal(bad);
    assert(net_config_parse(bad,strlen(bad),parsed,NULL)==-EINVAL);
    memcpy(bad,disk,length);bad[length]=0;field=strstr(bad,"[en1]");assert(field);field[3]='0';reseal(bad);
    assert(net_config_parse(bad,strlen(bad),parsed,NULL)==-EINVAL);
    memcpy(bad,disk,length);bad[length]=0;field=strstr(bad,"vendor=");assert(field);memcpy(field,"device",6);reseal(bad);
    assert(net_config_parse(bad,strlen(bad),parsed,NULL)==-EINVAL);
    memcpy(bad,disk,length);bad[length]=0;field=strstr(bad,"mac=02:");assert(field);field[5]='1';reseal(bad);
    assert(net_config_parse(bad,strlen(bad),parsed,NULL)==-EINVAL);
    disk[30]^=1;net_settings_load(&volume);assert(strstr(last_record,"boot=invalid fallback=dhcp"));net_settings_get(0,&got);assert(got.dhcp);
    assert(!net_settings_save(0,&c));fail_read=1;net_settings_load(&volume);assert(strstr(last_record,"read_failed"));net_settings_get(0,&got);assert(got.dhcp);fail_read=0;
    size_t old_length=length;char old[NET_SETTINGS_BYTES];memcpy(old,disk,length);
    fail_write=1;assert(net_settings_save(0,&c)==-EIO);net_settings_get(0,&got);assert(got.dhcp);fail_write=0;
    assert(length==old_length && !memcmp(old,disk,length));
    net_settings_load(&volume);assert(strstr(last_record,"boot=saved fallback=none"));
    fail_rename=1;assert(net_settings_save(0,&c)==-EIO);fail_rename=0;assert(length==old_length && !memcmp(old,disk,length));
    fail_commit=1;assert(net_settings_save(0,&c)==-EIO);fail_commit=0;assert(!net_settings_save(0,&c));
    memset(disk,'x',sizeof(disk));length=sizeof(disk);net_settings_load(&volume);assert(strstr(last_record,"boot=invalid fallback=dhcp"));
    ipx_present=1;
    for(unsigned f=0;f<=4;f++){ipx_text[11]=(char)('0'+f);net_settings_load(&volume);assert(net_ipx_frame_type()==f);}
    ipx_text[11]='9';net_settings_load(&volume);assert(net_ipx_frame_type()==CIUKI_IPX_FRAME_AUTO);
    net_settings_load(NULL);assert(strstr(last_record,"storage_absent"));assert(net_settings_save(0,&c)==-ENODEV);
    assert(!tables && !open_fd);puts("PASS f2-100 persistence: committed round trip, boot load, missing/corrupt/oversized/read failure DHCP fallback, stable PCI/MAC binding across enumeration reorder, atomic replacement, short write/commit/rename errors, no handle leaks");return 0;
}
