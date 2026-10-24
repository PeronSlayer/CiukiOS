/* G18 bounded worker-side shortcut persistence and classic ICO decoding.
 * SPDX-License-Identifier: MIT */
#ifndef DESK_SHORTCUT_IO_H
#define DESK_SHORTCUT_IO_H
#include "shell_link.h"
#include "settings_io.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#define DESK_SHORTCUT_PIXELS (16u*16u+32u*32u)
#define DESK_SHORTCUT_ICO_BYTES (1024u*1024u)
static inline unsigned desk_ico16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static inline int desk_ico_dib(const uint8_t *b,unsigned n,uint32_t *pixels,unsigned size)
{
    if(n<40 || desk_get32(b)!=40 || desk_ico16(b+12)!=1 || desk_get32(b+16))return 0;
    unsigned w=desk_get32(b+4),h=desk_get32(b+8),depth=desk_ico16(b+14);
    if(!w || w>256 || h!=2*w || (depth!=1 && depth!=4 && depth!=8 && depth!=24 && depth!=32))return 0;
    unsigned colors=depth<=8 ? (desk_get32(b+32) ? desk_get32(b+32) : 1u<<depth) : 0;
    if(colors>256 || (depth<=8 && colors>(1u<<depth)))return 0;
    unsigned pitch=((w*depth+31)/32)*4,mask_pitch=((w+31)/32)*4,offset=40+colors*4;
    if((uint64_t)offset+(uint64_t)(pitch+mask_pitch)*w>n)return 0;
    int alpha=0;
    if(depth==32)for(unsigned y=0;y<w && !alpha;y++)for(unsigned x=0;x<w;x++)
        if(b[offset+y*pitch+x*4+3]){alpha=1;break;}
    for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++){
        unsigned sx=x*w/size,sy=w-1-y*w/size;
        const uint8_t *row=b+offset+sy*pitch;
        uint32_t color;
        if(depth==32)color=desk_get32(row+sx*4);
        else if(depth==24)color=row[sx*3]|(uint32_t)row[sx*3+1]<<8|(uint32_t)row[sx*3+2]<<16;
        else {
            unsigned index=depth==8 ? row[sx] : depth==4 ? (row[sx/2]>>(sx&1 ? 0 : 4))&15 : (row[sx/8]>>(7-(sx&7)))&1;
            if(index>=colors)return 0;
            color=desk_get32(b+40+index*4);
        }
        int mask=!!(b[offset+pitch*w+sy*mask_pitch+sx/8]&(128u>>(sx&7)));
        pixels[y*size+x]=mask ? 0 : (color&0xffffffu)|(alpha ? color&0xff000000u : 0xff000000u);
    }
    return 1;
}
static inline int desk_ico_decode(const uint8_t *b,unsigned n,uint32_t *pixels,unsigned size)
{
    if(!b || !pixels || (size!=16 && size!=32) || n<6 || desk_ico16(b) || desk_ico16(b+2)!=1)return 0;
    unsigned count=desk_ico16(b+4);
    if(!count || count>256 || count>(n-6)/16)return 0;
    /* Try compatible DIBs in score order; malformed preferred images cannot
     * hide a valid alternate. Smaller images rank after images we downscale. */
    unsigned previous=0;
    for(unsigned pass=0;pass<count;pass++){
        unsigned best=~0u,chosen=count;
        for(unsigned i=0;i<count;i++){
            const uint8_t *e=b+6+i*16;unsigned w=e[0] ? e[0] : 256,h=e[1] ? e[1] : 256;
            if(w!=h)continue;
            unsigned score=(w>=size ? w-size : 512+size-w)*65536u+(32-(desk_ico16(e+6)>32 ? 32 : desk_ico16(e+6)))*512u+i+1;
            if(score>previous && score<best){best=score;chosen=i;}
        }
        if(chosen==count)break;
        previous=best;const uint8_t *e=b+6+chosen*16;
        unsigned bytes=desk_get32(e+8),at=desk_get32(e+12);
        if(at>=6+count*16 && at<=n && bytes<=n-at && desk_ico_dib(b+at,bytes,pixels,size))return 1;
    }
    return 0;
}
static inline int desk_shortcut_native_path(const char *path,char out[260])
{
    unsigned skip=0;
    if((path[0]=='C' || path[0]=='c') && path[1]==':' && (path[2]=='/' || path[2]=='\\'))skip=2;
    else if(path[0]!='/')return 0;
    size_t n=strlen(path+skip);if(n>=260)return 0;
    for(size_t i=0;i<=n;i++)out[i]=path[i+skip]=='\\' ? '/' : path[i+skip];
    return 1;
}
static inline int desk_shortcut_icon_load(const struct desk_launch_record *r,uint32_t *pixels)
{
    char path[260];size_t len=strlen(r->icon);
    if(len<4 || strcasecmp(r->icon+len-4,".ico") || r->icon_index || !desk_shortcut_native_path(r->icon,path))return 0;
    FILE *f=fopen(path,"rb");if(!f)return 0;
    uint8_t *bytes=malloc(DESK_SHORTCUT_ICO_BYTES+1);if(!bytes){fclose(f);return 0;}
    size_t n=fread(bytes,1,DESK_SHORTCUT_ICO_BYTES+1,f);int bad=ferror(f);if(fclose(f))bad=1;
    int ok=!bad && n<=DESK_SHORTCUT_ICO_BYTES && desk_ico_decode(bytes,(unsigned)n,pixels,16) && desk_ico_decode(bytes,(unsigned)n,pixels+16*16,32);
    free(bytes);return ok;
}
static inline int desk_shortcut_write(const char *path,const struct desk_launch_record *r)
{
    if(!path || !r || !desk_launch_valid(r))return -EINVAL;
    size_t len=strlen(path);
    if(len<4 || path[0]!='/' || (strcasecmp(path+len-4,".lnk") && (len<6 || strcasecmp(path+len-6,".ciuki"))))return -EINVAL;
    char temporary[300];int n=snprintf(temporary,sizeof(temporary),"%s.g18-tmp",path);
    if(n<0 || (unsigned)n>=sizeof(temporary))return -ENAMETOOLONG;
    uint8_t *bytes=malloc(DESK_LINK_BYTES);if(!bytes)return -ENOMEM;
    unsigned used=desk_link_encode(r,bytes,DESK_LINK_BYTES);
    int result=used ? -cfg_write_at(path,temporary,(const char *)bytes,used) : -EINVAL;
    free(bytes);return result;
}
#endif
