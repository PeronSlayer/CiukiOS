/* SPDX-License-Identifier: MIT
 * Wallpaper disk/cache operations and asynchronous frame ownership. */
#define _POSIX_C_SOURCE 200809L
#include "wallpaper_io.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* f1-39 reads regular files in 64 KiB transfers. Writes can still return
 * a page at a time; both loops advance by the actual count and retry EINTR. */
#define WALL_IO_CHUNK (64u * 1024u)

/* SHA-256 of the short, versioned cache identity. All inputs fit four blocks. */
static uint32_t rotate(uint32_t x, unsigned n) { return x>>n | x<<(32-n); }
static uint32_t sha8(const char *text)
{
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t state[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    unsigned char blocks[384]={0}; size_t n=strlen(text), length=(n+9+63)&~(size_t)63;
    memcpy(blocks,text,n); blocks[n]=128;
    uint64_t bits=(uint64_t)n*8;
    for (unsigned i=0;i<8;i++) blocks[length-1-i]=(unsigned char)(bits>>(8*i));
    for (size_t pos=0;pos<length;pos+=64) {
        uint32_t w[64];
        for (unsigned i=0;i<16;i++) { const unsigned char *p=blocks+pos+4*i; w[i]=(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
        for (unsigned i=16;i<64;i++) {
            uint32_t a=w[i-15],b=w[i-2];
            w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for (unsigned i=0;i<64;i++) {
            uint32_t t=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            h=g; g=f; f=e; e=d+t; d=c; c=b; b=a; a=t+u;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }
    return state[0];
}
void desk_wall_cache_name(char out[DESK_WALL_NAME_MAX], const struct desk_wall_request *r, uint64_t size, int64_t mtime)
{
    char identity[320];
    snprintf(identity,sizeof(identity),"wallpaper-v1\n%s\n%llu\n%lld\n%ux%u\n%d\n%06x\n%u,%u\n",
             r->path,(unsigned long long)size,(long long)mtime,r->width,r->height,r->mode,r->colour&0xffffffu,r->top,r->bottom);
    snprintf(out,DESK_WALL_NAME_MAX,"%08x-%ux%u-%s.raw",sha8(identity),r->width,r->height,cfg_mode_names[r->mode]);
}
static int join_path(char *out, size_t n, const char *dir, const char *name)
{
    int length=snprintf(out,n,"%s/%s",dir,name);
    return length<0 || (size_t)length>=n ? ENAMETOOLONG : 0;
}
static int raw_name(const char *name)
{
    if (strlen(name)>=DESK_WALL_NAME_MAX || strlen(name)<17) return 0;
    for (unsigned i=0;i<8;i++) if (!strchr("0123456789abcdef",name[i])) return 0;
    return name[8]=='-' && !strcmp(name+strlen(name)-4,".raw") && !strchr(name,'/');
}
static int make_directory(const char *path)
{
    char copy[512]; if (strlen(path)>=sizeof(copy)) return ENAMETOOLONG;
    strcpy(copy,path);
    for (char *p=copy+1;*p;p++) if (*p=='/') {
        *p=0; int e=mkdir(copy,0755) && errno!=EEXIST ? errno : 0; *p='/'; if (e) return e;
    }
    return mkdir(copy,0755) && errno!=EEXIST ? errno : 0;
}
static int write_atomic(const char *path, const void *data, size_t bytes)
{
    char temporary[544]; snprintf(temporary,sizeof(temporary),"%s.new",path);
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644); if (fd<0) return errno;
    size_t done=0; int error=0;
    while (done<bytes) {
        size_t request=bytes-done<WALL_IO_CHUNK ? bytes-done : WALL_IO_CHUNK;
        ssize_t n=write(fd,(const char *)data+done,request);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { error=n<0 ? errno : EIO; break; } done+=(size_t)n;
    }
    if (!error && fsync(fd)) error=errno;
    if (close(fd) && !error) error=errno;
    if (!error && rename(temporary,path)) error=errno;
    if (error) unlink(temporary);
    return error;
}
/* Persist a tiny use-order journal: cache hits are promoted as well as writes.
 * The five build seeds are trimmed on first use. Ignore unrelated files. */
static void cache_promote(const char *dir, const char *name)
{
    char path[512], keep[3][DESK_WALL_NAME_MAX]={{0}};
    if (join_path(path,sizeof(path),dir,"recent")) return;
    strcpy(keep[0],name); unsigned count=1;
    FILE *f=fopen(path,"r");
    if (f) {
        char line[DESK_WALL_NAME_MAX+2];
        while (count<3 && fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            if (raw_name(line) && strcmp(line,name) && (count==1 || strcmp(line,keep[1]))) strcpy(keep[count++],line);
        }
        fclose(f);
    }
    DIR *d=opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e=readdir(d)) && count<3) {
        if (!raw_name(e->d_name)) continue;
        unsigned i=0; while (i<count && strcmp(keep[i],e->d_name)) i++;
        if (i==count) strcpy(keep[count++],e->d_name);
    }
    rewinddir(d);
    char journal[3*DESK_WALL_NAME_MAX+1]; size_t n=0;
    for (unsigned i=0;i<count;i++) n+=(size_t)snprintf(journal+n,sizeof(journal)-n,"%s\n",keep[i]);
    if (!write_atomic(path,journal,n)) while ((e=readdir(d))) {
        if (!raw_name(e->d_name)) continue;
        unsigned i=0; while (i<count && strcmp(keep[i],e->d_name)) i++;
        if (i==count && !join_path(path,sizeof(path),dir,e->d_name)) unlink(path);
    }
    closedir(d);
}
static int read_frame(const char *path, uint32_t *pixels, size_t bytes)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC); if (fd<0) return -1;
    struct stat s;
    int valid=!fstat(fd,&s) && S_ISREG(s.st_mode) && s.st_size==(int64_t)bytes;
    size_t done=0;
    while (valid && done<bytes) {
        size_t request=bytes-done<WALL_IO_CHUNK ? bytes-done : WALL_IO_CHUNK;
        ssize_t n=read(fd,(char *)pixels+done,request);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break; done+=(size_t)n;
    }
    close(fd); return done==bytes ? 0 : -1;
}
int desk_wall_prepare(const struct desk_wall_request *r, const char *dir, uint32_t **out, int *hit, void (*stage)(const char *))
{
    *out=NULL; *hit=0;
    if (!r->width || !r->height || r->width>2048 || r->height>2048 || r->mode<0 || r->mode>=CFG_WALL_MODES || r->path[0]!='/') return EINVAL;
    struct stat s;
    if (stat(r->path,&s)) return errno;
    if (!S_ISREG(s.st_mode) || s.st_size<=0 || s.st_size>DESK_IMAGE_FILE_MAX) return EFBIG;
    char name[DESK_WALL_NAME_MAX], path[512];
    desk_wall_cache_name(name,r,(uint64_t)s.st_size,(int64_t)s.st_mtime);
    size_t bytes=(size_t)r->width*r->height*4;
    uint32_t *pixels=malloc(bytes); if (!pixels) return ENOMEM;
    if (dir && !join_path(path,sizeof(path),dir,name) && !read_frame(path,pixels,bytes)) {
        *out=pixels; *hit=1;
        if (stage) stage("wallpaper_cache_read");
        cache_promote(dir,name); return 0;
    }
    int fd=open(r->path,O_RDONLY|O_CLOEXEC); int error=fd<0 ? errno : 0;
    size_t n=(size_t)s.st_size, done=0; uint8_t *data=error ? NULL : malloc(n);
    if (!error && !data) error=ENOMEM;
    while (!error && done<n) {
        size_t request=n-done<WALL_IO_CHUNK ? n-done : WALL_IO_CHUNK;
        ssize_t got=read(fd,data+done,request);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) error=got<0 ? errno : EIO; else done+=(size_t)got;
    }
    if (fd>=0) close(fd);
    if (!error && stage) stage("wallpaper_file_read");
    struct desk_image image={0};
    if (!error && desk_image_decode(data,n,&image)) error=EINVAL;
    free(data);
    if (!error && stage) stage("wallpaper_decoded");
    if (!error && desk_wallpaper_render(pixels,r->width,r->height,r->top,r->bottom,r->path,r->mode,r->colour,&image,NULL)) error=ENOMEM;
    desk_image_free(&image);
    if (!error && stage) stage("wallpaper_scaled");
    /* Do not commit a frame under a key whose source changed during I/O. */
    struct stat after;
    if (!error && (stat(r->path,&after) || after.st_size!=s.st_size || after.st_mtime!=s.st_mtime)) error=EAGAIN;
    if (error) { free(pixels); return error; }
    *out=pixels;
    if (dir && !make_directory(dir) && !join_path(path,sizeof(path),dir,name) && !write_atomic(path,pixels,bytes)) cache_promote(dir,name);
    return 0;
}
static void *wall_worker(void *arg)
{
    struct desk_wall_worker *w=arg;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->pending && !w->stopping) pthread_cond_wait(&w->changed,&w->lock);
        if (w->stopping) break;
        struct desk_wall_request r=w->request; unsigned serial=w->serial;
        w->pending=0; w->busy=1; pthread_mutex_unlock(&w->lock);
        uint32_t *pixels; int hit, error=desk_wall_prepare(&r,w->cache_dir,&pixels,&hit,w->stage);
        pthread_mutex_lock(&w->lock); w->busy=0;
        uint32_t *old=w->pixels;
        w->pixels=NULL;
        if (!w->stopping && serial==w->serial) {
            w->pixels=pixels; pixels=NULL; w->ready=1; w->completed_serial=serial; w->error=error; w->cache_hit=hit;
        }
        pthread_mutex_unlock(&w->lock); free(old); free(pixels); pthread_mutex_lock(&w->lock);
    }
    pthread_mutex_unlock(&w->lock); return NULL;
}
int desk_wall_worker_start(struct desk_wall_worker *w, const char *dir, void (*stage)(const char *))
{
    memset(w,0,sizeof(*w)); w->cache_dir=dir; w->stage=stage;
    int e=pthread_mutex_init(&w->lock,NULL); if (e) return e;
    e=pthread_cond_init(&w->changed,NULL); if (e) { pthread_mutex_destroy(&w->lock); return e; }
    e=pthread_create(&w->thread,NULL,wall_worker,w);
    if (e) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); } return e;
}
unsigned desk_wall_worker_submit(struct desk_wall_worker *w, const struct desk_wall_request *r)
{
    if (pthread_mutex_trylock(&w->lock)) return 0;
    w->request=*r; if (!++w->serial) ++w->serial;
    unsigned serial=w->serial; w->pending=1; w->ready=0;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock); return serial;
}
int desk_wall_worker_take(struct desk_wall_worker *w, unsigned serial, uint32_t **pixels, int *error, int *hit)
{
    if (pthread_mutex_trylock(&w->lock)) return 0;
    int ready=w->ready && w->completed_serial==serial;
    if (ready) { *pixels=w->pixels; w->pixels=NULL; *error=w->error; *hit=w->cache_hit; w->ready=0; }
    pthread_mutex_unlock(&w->lock); return ready;
}
int desk_wall_worker_busy(struct desk_wall_worker *w)
{
    if (pthread_mutex_trylock(&w->lock)) return 1;
    int busy=w->busy || w->pending; pthread_mutex_unlock(&w->lock); return busy;
}
void desk_wall_worker_stop(struct desk_wall_worker *w)
{
    pthread_mutex_lock(&w->lock); w->stopping=1; pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL); free(w->pixels); w->pixels=NULL;
    pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock);
}
