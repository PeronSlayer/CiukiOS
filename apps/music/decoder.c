/* SPDX-License-Identifier: MIT
 * Bounded streaming adapters; upstream scalar decoders retain their licences. */
#include "decoder.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#if defined(__clang__)
/* minimp3 uses a 17 KiB frame; the worker has the SDK default 1 MiB stack. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-larger-than"
#endif
#include "../../third_party/minimp3/minimp3.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/stb/stb_vorbis.c"
#define MP3_INPUT 16384
struct music_decoder {
    FILE *file; stb_vorbis *ogg; char *vorbis_memory; mp3dec_t mp3;
    unsigned kind, rate, channels, bits, output_rate;
    uint64_t frames, position, source_index; unsigned phase;
    long data; uint32_t bytes;
    uint8_t raw[4096];
    uint8_t input[MP3_INPUT]; unsigned input_count;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; unsigned count, offset;
    int16_t left[2], right[2]; int primed, error;
    music_cancel cancel; void *ctx;
};
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return u16(p) | (uint32_t)u16(p+2) << 16; }
static int cancelled(struct music_decoder *d) { return d->cancel && d->cancel(d->ctx); }
static int wav_open(struct music_decoder *d)
{
    unsigned char h[16];
    if (fread(h,1,12,d->file)!=12 || memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4)) return -EINVAL;
    uint64_t end=(uint64_t)u32(h+4)+8, pos=12; int format=0, data=0;
    if (end > LONG_MAX || fseek(d->file,0,SEEK_END)) return -EFBIG;
    long file_end=ftell(d->file);
    if (file_end < 0 || end > (uint64_t)file_end) return -EINVAL;
    while (pos+8<=end) {
        if (cancelled(d)) return -ECANCELED;
        if (fseek(d->file,(long)pos,SEEK_SET) || fread(h,1,8,d->file)!=8) return -EIO;
        uint32_t n=u32(h+4); pos+=8;
        if (n>end-pos) return -EINVAL;
        if (!memcmp(h,"fmt ",4)) {
            if (n<16 || fread(h,1,16,d->file)!=16) return -EINVAL;
            d->channels=u16(h+2); d->rate=u32(h+4); d->bits=u16(h+14);
            if (u16(h)!=1 || (d->channels!=1 && d->channels!=2) || !d->rate ||
                (d->bits!=8 && d->bits!=16) || u16(h+12)!=d->channels*d->bits/8 ||
                (uint64_t)d->rate*d->channels*d->bits/8!=u32(h+8)) return -EINVAL;
            format=1;
        } else if (!memcmp(h,"data",4) && !data) { d->data=(long)pos; d->bytes=n; data=1; }
        pos+=(uint64_t)n+(n&1);
        if (pos>end) return -EINVAL;
    }
    if (!format || !data || !d->bytes || d->bytes%(d->channels*d->bits/8)) return -EINVAL;
    d->frames=d->bytes/(d->channels*d->bits/8);
    return fseek(d->file,d->data,SEEK_SET) ? -EIO : 0;
}
static int mp3_block(struct music_decoder *d)
{
    while (!cancelled(d)) {
        size_t n=fread(d->input+d->input_count,1,MP3_INPUT-d->input_count,d->file);
        d->input_count+=(unsigned)n;
        if (ferror(d->file)) return -EIO;
        if (!d->input_count) return 0;
        mp3dec_frame_info_t info={0};
        int frames=mp3dec_decode_frame(&d->mp3,d->input,(int)d->input_count,d->pcm,&info);
        if (info.frame_bytes<=0) { d->input_count=0; if (feof(d->file)) return 0; continue; }
        d->input_count-=(unsigned)info.frame_bytes;
        memmove(d->input,d->input+info.frame_bytes,d->input_count);
        if (!frames) continue;
        if (info.layer!=3 || (info.channels!=1 && info.channels!=2) || info.hz<=0) return -EINVAL;
        if (d->rate && (d->rate!=(unsigned)info.hz || d->channels!=(unsigned)info.channels)) return -ENOTSUP;
        d->rate=(unsigned)info.hz; d->channels=(unsigned)info.channels;
        d->offset=0; d->count=(unsigned)frames; return frames;
    }
    return -ECANCELED;
}
static int source(struct music_decoder *d, int16_t out[2])
{
    if (cancelled(d)) return -ECANCELED;
    if (d->offset==d->count) {
        d->offset=d->count=0;
        if (d->kind==1) {
            long p=ftell(d->file); if (p<0) return -EIO;
            uint64_t used=(uint64_t)(p-d->data);
            if (used>=d->bytes) return 0;
            unsigned frame_bytes=d->channels*d->bits/8;
            unsigned n=(unsigned)((d->bytes-used)/frame_bytes); if (n>1024) n=1024;
            if (fread(d->raw,frame_bytes,n,d->file)!=n) return -EIO;
            for (unsigned i=0;i<n*d->channels;i++) {
                unsigned v=d->bits==8 ? d->raw[i] : u16(d->raw+2*i);
                d->pcm[i]=(int16_t)(d->bits==8 ? ((int)v-128)*256 : v<32768 ? (int)v : (int)v-65536);
            }
            d->count=n;
        } else if (d->kind==2) {
            int n=mp3_block(d); if (n<=0) return n;
        } else {
            int n=stb_vorbis_get_samples_short_interleaved(d->ogg,(int)d->channels,d->pcm,2048);
            if (!n) return stb_vorbis_get_error(d->ogg) ? -EIO : 0;
            d->count=(unsigned)n;
        }
    }
    out[0]=d->pcm[d->offset*d->channels]; out[1]=d->pcm[d->offset*d->channels+d->channels-1];
    d->offset++; return 1;
}
static int rewind_source(struct music_decoder *d, uint64_t frame)
{
    d->count=d->offset=d->input_count=0; d->primed=d->error=0;
    if (d->kind==1) return fseek(d->file,d->data+(long)(frame*d->channels*d->bits/8),SEEK_SET) ? -EIO : 0;
    if (d->kind==3) return stb_vorbis_seek(d->ogg,(unsigned)frame) ? 0 : -EIO;
    if (fseek(d->file,0,SEEK_SET)) return -EIO;
    clearerr(d->file); mp3dec_init(&d->mp3);
    int16_t discard[2];
    for (uint64_t i=0;i<frame;i++) { int r=source(d,discard); if (r<=0) return r ? r : -EIO; }
    return 0;
}
int music_decoder_open(struct music_decoder **out, const char *path, unsigned rate, music_cancel cancel, void *ctx)
{
    *out=NULL; if (!path || !rate) return -EINVAL;
    struct music_decoder *d=calloc(1,sizeof(*d)); if (!d) return -ENOMEM;
    d->cancel=cancel; d->ctx=ctx; d->output_rate=rate;
    const char *ext=strrchr(path,'.'); int err=-EINVAL;
    if (!ext) goto done;
    d->file=fopen(path,"rb"); if (!d->file) { err=-errno; goto done; }
    if (!strcasecmp(ext,".wav")) { d->kind=1; err=wav_open(d); }
    else if (!strcasecmp(ext,".mp3")) {
        d->kind=2; mp3dec_init(&d->mp3); int r;
        while ((r=mp3_block(d))>0) d->frames+=(unsigned)r;
        err=r<0 ? r : !d->frames ? -EINVAL : rewind_source(d,0);
    } else if (!strcasecmp(ext,".ogg")) {
        d->kind=3; int e;
        d->vorbis_memory=malloc(1u<<20);
        if (!d->vorbis_memory) { err=-ENOMEM; goto done; }
        stb_vorbis_alloc alloc={d->vorbis_memory,1<<20};
        d->ogg=stb_vorbis_open_file(d->file,0,&e,&alloc);
        if (d->ogg) {
            stb_vorbis_info info=stb_vorbis_get_info(d->ogg);
            d->rate=info.sample_rate; d->channels=(unsigned)info.channels;
            d->frames=stb_vorbis_stream_length_in_samples(d->ogg);
            err=(!d->rate || (d->channels!=1 && d->channels!=2) || !d->frames) ? -EINVAL : 0;
        }
    }
    if (!err && cancelled(d)) err=-ECANCELED;
done:
    if (err) music_decoder_close(d); else *out=d;
    return err;
}
uint64_t music_decoder_length(const struct music_decoder *d)
{ return d ? (d->frames*d->output_rate+d->rate-1)/d->rate : 0; }
unsigned music_decoder_rate(const struct music_decoder *d) { return d ? d->rate : 0; }
int music_decoder_seek(struct music_decoder *d, uint64_t frame)
{
    uint64_t length=music_decoder_length(d); if (frame>length) frame=length;
    uint64_t target=frame*d->rate/d->output_rate; if (target>=d->frames) target=d->frames-1;
    int r=rewind_source(d,target); if (r) return r;
    d->position=frame; d->source_index=target; d->phase=(unsigned)(frame*d->rate%d->output_rate);
    return 0;
}
int music_decoder_read(struct music_decoder *d, int16_t *out, unsigned frames)
{
    if (d->error) return d->error;
    unsigned n=0;
    while (n<frames && d->position<music_decoder_length(d)) {
        if (cancelled(d)) return -ECANCELED;
        if (!d->primed) {
            int r=source(d,d->left); if (r<=0) { d->error=r ? r : -EIO; break; }
            r=source(d,d->right); if (r<0) { d->error=r; break; }
            if (!r) memcpy(d->right,d->left,sizeof(d->right));
            d->primed=1;
        }
        for (unsigned c=0;c<2;c++) out[2*n+c]=(int16_t)(d->left[c]+(int64_t)(d->right[c]-d->left[c])*d->phase/d->output_rate);
        n++; d->position++;
        uint64_t phase=(uint64_t)d->phase+d->rate;
        unsigned advance=(unsigned)(phase/d->output_rate); d->phase=(unsigned)(phase%d->output_rate);
        while (advance-- && d->source_index+1<d->frames) {
            memcpy(d->left,d->right,sizeof(d->left)); d->source_index++;
            int r=source(d,d->right); if (r<0) { d->error=r; break; }
            if (!r) memcpy(d->right,d->left,sizeof(d->right));
        }
        if (d->error) break;
    }
    return n ? (int)n : d->error;
}
void music_decoder_close(struct music_decoder *d)
{ if (d) { if (d->ogg) stb_vorbis_close(d->ogg); if (d->file) fclose(d->file); free(d->vorbis_memory); free(d); } }
