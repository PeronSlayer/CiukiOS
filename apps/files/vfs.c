/* SPDX-License-Identifier: MIT
 * POSIX adapter for the Files model (CiukiOS SDK: readdir over getdents,
 * stat, mkdir, rename, unlink, rmdir, open/read/write/fsync/close). */
#define _DEFAULT_SOURCE 1
#include "model.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(__ciuki__) || defined(__i386__)
#include <ciuki/fileinfo.h>
#endif

static int metadata_stat(void *ctx, const char *path, struct fm_meta *m)
{
    (void)ctx;
    struct stat s;
    if (stat(path, &s)) return -errno;
    *m = (struct fm_meta){S_ISDIR(s.st_mode) ? FM_DIR : S_ISREG(s.st_mode) ? FM_FILE : FM_OTHER, (uint64_t)s.st_size,
                          (uint64_t)s.st_ino, (uint32_t)s.st_dev, (uint32_t)s.st_mode, (int64_t)s.st_mtime, 0, 0, (uint64_t)s.st_blocks * 512u, 1};
#if defined(__ciuki__) || defined(__i386__)
    struct ciuki_file_info info;
    if (!ciuki_file_info_get(path, &info)) {
        m->attributes = info.attributes; m->allocated = info.allocated;
        m->allocated_known = !!(info.mask & CIUKI_FILE_INFO_ALLOCATED);
    }
#else
    m->attributes = (s.st_mode & 0200 ? 0u : 1u) | (S_ISDIR(s.st_mode) ? 16u : 0u);
#endif
    return 0;
}

/* Files without an extension are classified by their ELF32 i386 header. */
static int sniff(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char h[20];
    ssize_t n = read(fd, h, sizeof(h));
    close(fd);
    return n == (ssize_t)sizeof(h) && !memcmp(h, "\177ELF\1\1\1", 7) && h[16] == 2 && h[18] == 3;
}

static int list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    int r = 0; unsigned sniffed = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) r = -errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char full[FM_PATH]; struct fm_meta meta = {.type=FM_OTHER, .ino=entry->d_ino};
#if defined(CIUKI_DIRENT_METADATA)
        meta.type = entry->d_type == DT_DIR ? FM_DIR : entry->d_type == DT_REG ? FM_FILE : FM_OTHER;
        meta.size = entry->d_size; meta.modified = entry->d_mtime; meta.attributes = entry->d_attributes;
#elif defined(DT_DIR)
        /* Portable host builds lack Ciuki's metadata extension. */
        meta.type = entry->d_type == DT_DIR ? FM_DIR : entry->d_type == DT_REG ? FM_FILE : FM_OTHER;
#endif
        if (flags & FM_LIST_SNIFF) {
            r = fm_join(full, path, entry->d_name);
            if (!r) r = metadata_stat(ctx, full, &meta);
        }
#if !defined(CIUKI_DIRENT_METADATA)
        else if (flags & FM_LIST_METADATA) {
            r = fm_join(full, path, entry->d_name);
            if (!r) r = metadata_stat(ctx, full, &meta);
        }
#endif
        if (r == -ENOENT) { r = 0; continue; }   /* removed during enumeration */
        if (!r && (flags & FM_LIST_SNIFF) && meta.type == FM_FILE && meta.size >= 52 && !strchr(entry->d_name, '.') && sniffed < 128) {
            sniffed++; meta.exec = (uint32_t)sniff(full);
        }
        if (!r) r = emit(entry->d_name, &meta, arg);
        if (r) break;
    }
    if (closedir(dir) && !r) r = -errno;
    return r;
}

static int make_dir(void *ctx, const char *p) { (void)ctx; return mkdir(p, 0777) ? -errno : 0; }
static int move(void *ctx, const char *a, const char *b) { (void)ctx; return rename(a, b) ? -errno : 0; }
static int remove_file(void *ctx, const char *p) { (void)ctx; return unlink(p) ? -errno : 0; }
static int remove_dir(void *ctx, const char *p) { (void)ctx; return rmdir(p) ? -errno : 0; }
static int open_file(void *ctx, const char *p, int create)
{ (void)ctx; int fd = open(p, create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, 0666); return fd < 0 ? -errno : fd; }
static int read_file(void *ctx, int fd, void *p, unsigned n) { (void)ctx; int r = (int)read(fd, p, n); return r < 0 ? -errno : r; }
static int write_file(void *ctx, int fd, const void *p, unsigned n) { (void)ctx; int r = (int)write(fd, p, n); return r < 0 ? -errno : r; }
static int sync_file(void *ctx, int fd) { (void)ctx; return fsync(fd) ? -errno : 0; }
static int close_file(void *ctx, int fd) { (void)ctx; return close(fd) ? -errno : 0; }

const struct fm_vfs *fm_native_vfs(void)
{
    static const struct fm_vfs fs = {NULL, list, metadata_stat, make_dir, move, remove_file, remove_dir,
                                     open_file, read_file, write_file, sync_file, close_file, NULL};
    return &fs;
}

/* G6: read PE32's RT_VERSION as data, never as an executable. Bounds are
 * checked before pread; no ABI, Win32 DLL or untrusted pointer is involved.
 * See docs/design/file-properties.md for Microsoft's format references. */
static uint16_t ver16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static uint32_t ver32(const unsigned char *p) { return ver16(p) | (uint32_t)ver16(p+2)<<16; }
struct version_file { int fd; uint64_t size; uint32_t sections, count, resource, bytes; };
static int version_read(struct version_file *v,uint64_t at,void *out,size_t n)
{
    if (at>v->size || n>v->size-at) return 0;
    unsigned char *p=out;
    while (n) {
        ssize_t r=pread(v->fd,p,n,(off_t)at);
        if (r<0 && errno==EINTR) continue;
        if (r<=0) return 0;
        p+=r; at+=(uint64_t)r; n-=(size_t)r;
    }
    return 1;
}
static int version_rva(struct version_file *v,uint32_t rva,uint32_t size,uint32_t *offset)
{
    for (uint32_t i=0;i<v->count;i++) {
        unsigned char s[40]; if (!version_read(v,(uint64_t)v->sections+i*40,s,sizeof(s))) return 0;
        uint32_t va=ver32(s+12),bytes=ver32(s+16),raw=ver32(s+20);
        if (rva>=va && rva-va<=bytes && size<=bytes-(rva-va) &&
            (uint64_t)raw+rva-va<=UINT32_MAX && (uint64_t)raw+rva-va+size<=v->size) {
            *offset=raw+(rva-va); return 1;
        }
    }
    return 0;
}
static int version_resource(struct version_file *v,uint32_t at,void *out,unsigned n)
{ return at<=v->bytes && n<=v->bytes-at && version_read(v,(uint64_t)v->resource+at,out,n); }
static int version_child(struct version_file *v,uint32_t at,uint32_t id,int any,int directory,uint32_t *child)
{
    unsigned char h[16]; if (!version_resource(v,at,h,sizeof(h))) return 0;
    unsigned count=(unsigned)ver16(h+12)+ver16(h+14);
    if (count>256 || count>(v->bytes-at-16)/8) return 0;
    for (unsigned i=0;i<count;i++) {
        unsigned char e[8]; if (!version_resource(v,at+16+i*8,e,sizeof(e))) return 0;
        uint32_t name=ver32(e),value=ver32(e+4);
        if ((int)(value>>31)!=directory) continue;
        if (name==id || any) { *child=value&0x7fffffffu; return 1; }
    }
    return 0;
}
static int version_key(const unsigned char *p,unsigned n,const char *key)
{
    unsigned k=0;
    while (k*2+2<=n) { unsigned c=ver16(p+k*2); if (c!=(unsigned char)key[k]) return 0; if (!c) return 1; k++; }
    return 0;
}
static void version_string(char *out,size_t cap,const char *key,const unsigned char *p,unsigned bytes)
{
    char value[192]; unsigned n=0;
    for (unsigned i=0;i+2<=bytes && n+4<sizeof(value);i+=2) {
        unsigned c=ver16(p+i); if (!c) break;
        if (c<32 || c==127 || (c>=0xd800 && c<=0xdfff)) c='?';
        if (c<128) value[n++]=(char)c;
        else if (c<2048) { value[n++]=(char)(0xc0|(c>>6)); value[n++]=(char)(0x80|(c&63)); }
        else { value[n++]=(char)(0xe0|(c>>12)); value[n++]=(char)(0x80|((c>>6)&63)); value[n++]=(char)(0x80|(c&63)); }
    }
    value[n]=0;
    size_t used=strlen(out); if (used<cap) snprintf(out+used,cap-used,"\n%s:\t%s",key,value);
}
static int version_node(const unsigned char *p,unsigned available,unsigned depth,char *out,size_t cap)
{
    if (depth>4 || available<8) return 0;
    unsigned bytes=ver16(p),values=ver16(p+2),type=ver16(p+4),at=6;
    if (bytes<8 || bytes>available || type>1) return 0;
    while (at+2<=bytes && ver16(p+at)) at+=2;
    if (at+2>bytes) return 0;
    unsigned key_bytes=at-6+2; at=(at+5)&~3u;
    unsigned value_bytes=type ? values*2 : values;
    if (at>bytes || value_bytes>bytes-at) return 0;
    if (!depth) {
        if (type || value_bytes<52 || !version_key(p+6,key_bytes,"VS_VERSION_INFO") || ver32(p+at)!=0xfeef04bdu) return 0;
        uint32_t fm=ver32(p+at+8),fl=ver32(p+at+12),pm=ver32(p+at+16),pl=ver32(p+at+20);
        snprintf(out,cap,"File version:\t%u.%u.%u.%u\nProduct version:\t%u.%u.%u.%u",
            fm>>16,fm&65535,fl>>16,fl&65535,pm>>16,pm&65535,pl>>16,pl&65535);
    } else if (type && value_bytes) {
        static const char *const keys[]={"CompanyName","FileDescription","ProductName","LegalCopyright","OriginalFilename"};
        static const char *const labels[]={"Company","Description","Product","Copyright","Original filename"};
        for (unsigned i=0;i<5;i++) if (version_key(p+6,key_bytes,keys[i])) version_string(out,cap,labels[i],p+at,value_bytes);
    }
    at=(at+value_bytes+3)&~3u;
    while (at+8<=bytes) {
        unsigned child=ver16(p+at);
        if (child<8 || child>bytes-at || !version_node(p+at,child,depth+1,out,cap)) return 0;
        at=(at+child+3)&~3u;
    }
    return 1;
}
int fm_native_version(const char *path,char *out,size_t cap)
{
    if (!out || !cap) return -EINVAL;
    out[0]=0; int fd=open(path,O_RDONLY); if (fd<0) return -errno;
    struct stat st; int result=-ENOENT;
    if (fstat(fd,&st) || st.st_size<64) { close(fd); return result; }
    struct version_file v={.fd=fd,.size=(uint64_t)st.st_size};
    unsigned char dos[64],nt[248],entry[16]; uint32_t pe,off,size,type,name,leaf;
    unsigned char *blob=NULL;
    if (!version_read(&v,0,dos,sizeof(dos)) || memcmp(dos,"MZ",2)) goto done;
    pe=ver32(dos+60);
    if (!version_read(&v,pe,nt,sizeof(nt)) || ver32(nt)!=0x4550 || ver16(nt+4)!=0x14c || ver16(nt+24)!=0x10b) goto done;
    unsigned optional=ver16(nt+20); v.count=ver16(nt+6);
    if (optional<224 || !v.count || v.count>96 || pe>UINT32_MAX-24-optional || ver32(nt+116)<3) goto done;
    v.sections=pe+24+optional; v.bytes=ver32(nt+140);
    if (v.bytes<16 || !version_rva(&v,ver32(nt+136),v.bytes,&v.resource) ||
        !version_child(&v,0,16,0,1,&type) || !version_child(&v,type,0,1,1,&name) ||
        !version_child(&v,name,0,1,0,&leaf) || !version_resource(&v,leaf,entry,sizeof(entry))) goto done;
    size=ver32(entry+4);
    if (size<8 || size>65535 || !version_rva(&v,ver32(entry),size,&off)) goto done;
    blob=malloc(size); if (!blob) { result=-ENOMEM; goto done; }
    if (version_read(&v,off,blob,size) && version_node(blob,size,0,out,cap)) result=0;
done:
    free(blob); close(fd); if (result) out[0]=0; return result;
}
