/* SPDX-License-Identifier: MIT
 * Bounded streaming adapters; upstream scalar decoders retain their licences. */
#include "decoder.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
/* The reader yields according to occupancy. Cached adapter reads must not
 * unconditionally surrender its quantum while the reservoir is empty. */
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
/* f4-37: float output, converted by pcm_s16 below without the FLDCW pair
 * that a C float-to-int16 conversion costs on x87. */
#define MINIMP3_FLOAT_OUTPUT
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
#define READ_AHEAD (256u*1024u)
struct music_decoder {
    FILE *file; stb_vorbis *ogg; char *vorbis_memory; mp3dec_t mp3;
    unsigned kind, rate, channels, bits, output_rate;
    uint64_t frames, position, source_index, mp3_decoded;
    /* f4-37: rate conversion in units of g=gcd(rate, output_rate): the phase
     * runs 0..period-1 in steps of step (rate/g, output_rate/g). */
    unsigned phase, step, period, shift; uint32_t magic;
    long data; uint32_t bytes;
    uint8_t raw[4096];
    char read_ahead[READ_AHEAD];
    uint8_t input[MP3_INPUT]; unsigned input_pos, input_count;
    unsigned sync_bytes;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; unsigned count, offset;
    float mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t left[2], right[2]; int primed, error;
    music_cancel cancel; void *ctx;
    struct music_cost cost;
};
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return u16(p) | (uint32_t)u16(p+2) << 16; }
static int cancelled(struct music_decoder *d) { return d->cancel && d->cancel(d->ctx); }
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void mp3_length(struct music_decoder *d, const uint8_t *h, unsigned bytes, unsigned samples)
{
    if (d->mp3_decoded || bytes<4) return;
    unsigned side=(h[1]&8) ? ((h[3]>>6)==3 ? 17 : 32) : ((h[3]>>6)==3 ? 9 : 17);
    unsigned off=4+(!(h[1]&1) ? 2 : 0)+side;
    if (bytes>=off+12 && (!memcmp(h+off,"Xing",4) || !memcmp(h+off,"Info",4)) && (be32(h+off+4)&1)) {
        uint32_t n=be32(h+off+8);
        /* Preserve the existing raw decoder timeline, including the tag frame.
         * Encoder delay/padding trimming is not part of this adapter. */
        if (n) d->frames=((uint64_t)n+1)*samples;
    }
}
static int mp3_start(struct music_decoder *d)
{
    uint8_t h[10];
    if (cancelled(d)) return -ECANCELED;
    size_t n=fread(h,1,sizeof(h),d->file);
    if (ferror(d->file)) return -(errno ? errno : EIO);
    long start=0;
    if (n==10 && !memcmp(h,"ID3",3)) {
        if (h[3]<2 || h[3]>4 || h[4]==255 || (h[6]|h[7]|h[8]|h[9])&128) return -EINVAL;
        start=10+((long)h[6]<<21)+((long)h[7]<<14)+((long)h[8]<<7)+h[9];
        if (h[3]==4 && (h[5]&16)) start+=10;
        if (fseek(d->file,0,SEEK_END)) return -EIO;
        long end=ftell(d->file); if (end<start) return -EINVAL;
    }
    d->data=start;
    return fseek(d->file,start,SEEK_SET) ? -EIO : 0;
}
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
/* f4-37: sticky denormal-operand (DE, bit 1) and underflow (UE, bit 4) flags
 * of the unit doing the float math, read and cleared once per MPEG frame: the
 * x87 status word on the i686 baseline (Intel SDM Vol. 1, 8.5.2 and 8.5.5),
 * MXCSR where the compiler uses SSE for float math (host builds). */
static unsigned fp_flags_take(void)
{
#if defined(__i386__) && !defined(__SSE_MATH__)
    uint16_t sw;
    __asm__ volatile("fnstsw %0\n\tfnclex" : "=m"(sw) :: "memory");
    return sw&0x12u;
#elif defined(__x86_64__) || defined(__SSE_MATH__)
    unsigned csr=__builtin_ia32_stmxcsr();
    __builtin_ia32_ldmxcsr(csr&~0x3fu);
    return csr&0x12u;
#else
    return 0;
#endif
}
/* Values below FLT_MIN (zero exponent, nonzero fraction), by bit pattern:
 * the scan itself never loads a float. */
static unsigned tiny_floats(const float *f, unsigned n)
{
    unsigned count=0;
    for (unsigned i=0;i<n;i++) {
        uint32_t bits; __builtin_memcpy(&bits,f+i,sizeof(bits));
        count+=!(bits&0x7f800000u) && (bits&0x007fffffu);
    }
    return count;
}
/* f4-37: minimp3's int16 conversion, mp3d_scale_pcm (minimp3.h:1430-1442),
 * applied to its float output f = sample/32768 (line 1445), in integer
 * arithmetic on the float's bits: the clamps at 32766.5 and -32767.5,
 * trunc(sample+0.5) and the step down below zero. On x87 the C conversion
 * changes the rounding mode with two FLDCW per sample, "a synchronizing
 * instruction [that] will cause a significant slowdown ... on all IA-based
 * processors" (Intel Architecture Optimization Reference Manual, 245127-001,
 * section 2, Floating-point to Integer Conversion). Exact for every float;
 * music_x87_test.c compares the PCM with the int16 reference decoder. */
static inline int16_t pcm_s16(float f)
{
    uint32_t bits; __builtin_memcpy(&bits,&f,sizeof(bits));
    /* |sample| = mantissa*2^-shift; floor(|sample|+0.5) by one add and one
     * shift. shift>31 covers |sample|<0.5, zero and subnormals (0); shift<1
     * means |sample|>=2^23 (clamped). Selects, no branch on the data. */
    int shift=135-(int)((bits>>23)&255u);
    unsigned s=shift<1 ? 1u : shift>31 ? 31u : (unsigned)shift;
    uint32_t up=(((bits&0x7fffffu)|0x800000u)+(1u<<(s-1u)))>>s;
    uint32_t m=shift<1 || up>32768u ? 32768u : up;
    int32_t positive=(int32_t)(m-(m>>15));                /* at most 32767 */
    int32_t negative=-(int32_t)(m&(0u-(uint32_t)(m>1u))); /* 0 below 1.5 */
    int32_t sign=(int32_t)bits>>31;
    return (int16_t)((positive&~sign)|(negative&sign));
}
__attribute__((noinline))
static void pcm_convert(int16_t *out, const float *in, unsigned n)
{ for (unsigned i=0;i<n;i++) out[i]=pcm_s16(in[i]); }
/* f4-37: mp3dec_decode_frame (minimp3.h ea99364, lines 1712-1777) for
 * MINIMP3_ONLY_MP3, with L3_decode (lines 1238-1272) split after its stereo
 * step: the same upstream calls in the same order, timed per stage, then
 * pcm_convert. music_x87_test.c compares the PCM with upstream minimp3's own
 * int16 output (music_x87_ref.c): at most 1 LSB apart, where rounding the x87
 * sum to the float output crosses a half. One addition: the last synthesis
 * row is cleared before each granule.
 * mp3d_synth (lines 1476-1615) never writes lanes 2 and 3 of the
 * last row; mp3d_synth_granule copies that row into qmf_state, and in the
 * mono branch (lines 1640-1645) the copy runs through x87 loads. Without the
 * clear, stack garbage whose bit pattern is denormal made the i686 build raise
 * the denormal-operand flag (a P6 microcode assist) in every mono granule. The
 * PCM does not depend on those lanes: the test decodes with the scratch
 * poisoned by denormal, NaN and normal patterns and compares the hashes. */
#if defined(__clang__)
/* The 17 KiB minimp3 scratch, as in mp3dec_decode_frame (see the include). */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-larger-than"
#endif
__attribute__((noinline))
static int mp3_frame(struct music_decoder *d, const uint8_t *mp3, int mp3_bytes, mp3dec_frame_info_t *info)
{
    mp3dec_t *dec=&d->mp3; mp3d_sample_t *pcm=d->mp3_pcm; int16_t *out=d->pcm;
    int i=0, igr, frame_size=0, success=1;
    const uint8_t *hdr; bs_t bs_frame[1]; mp3dec_scratch_t scratch;
    uint64_t start=music_cycles(), t0=start, t1, t2, t3, decode=0, imdct=0, synth=0, convert=0;
    if (mp3_bytes>4 && dec->header[0]==0xff && hdr_compare(dec->header,mp3)) {
        frame_size=hdr_frame_bytes(mp3,dec->free_format_bytes)+hdr_padding(mp3);
        if (frame_size!=mp3_bytes && (frame_size+HDR_SIZE>mp3_bytes || !hdr_compare(mp3,mp3+frame_size)))
            frame_size=0;
    }
    if (!frame_size) {
        memset(dec,0,sizeof(mp3dec_t));
        i=mp3d_find_frame(mp3,mp3_bytes,&dec->free_format_bytes,&frame_size);
        if (!frame_size || i+frame_size>mp3_bytes) {
            info->frame_bytes=i; d->cost.decode+=music_cycles()-start; return 0;
        }
    }
    hdr=mp3+i; memcpy(dec->header,hdr,HDR_SIZE);
    info->frame_bytes=i+frame_size; info->frame_offset=i;
    info->channels=HDR_IS_MONO(hdr) ? 1 : 2; info->hz=hdr_sample_rate_hz(hdr);
    info->layer=4-HDR_GET_LAYER(hdr); info->bitrate_kbps=hdr_bitrate_kbps(hdr);
    bs_init(bs_frame,hdr+HDR_SIZE,frame_size-HDR_SIZE);
    if (HDR_IS_CRC(hdr)) get_bits(bs_frame,16);
    if (info->layer!=3) { d->cost.decode+=music_cycles()-start; return 0; }
    int main_data_begin=L3_read_side_info(bs_frame,scratch.gr_info,hdr);
    if (main_data_begin<0 || bs_frame->pos>bs_frame->limit) {
        mp3dec_init(dec); d->cost.decode+=music_cycles()-start; return 0;
    }
    success=L3_restore_reservoir(dec,bs_frame,&scratch,main_data_begin);
    if (success) {
        int nch=info->channels;
        for (igr=0;igr<(HDR_TEST_MPEG1(hdr) ? 2 : 1);igr++,pcm+=576*nch,out+=576*nch) {
            L3_gr_info_t *gr_info=scratch.gr_info+igr*nch; int ch;
            memset(scratch.grbuf[0],0,576*2*sizeof(float));
            for (ch=0;ch<nch;ch++) {
                int layer3gr_limit=scratch.bs.pos+gr_info[ch].part_23_length;
                L3_decode_scalefactors(dec->header,scratch.ist_pos[ch],&scratch.bs,gr_info+ch,scratch.scf,ch);
                L3_huffman(scratch.grbuf[ch],&scratch.bs,gr_info+ch,scratch.scf,layer3gr_limit);
            }
            if (HDR_TEST_I_STEREO(dec->header)) L3_intensity_stereo(scratch.grbuf[0],scratch.ist_pos[1],gr_info,dec->header);
            else if (HDR_IS_MS_STEREO(dec->header)) L3_midside_stereo(scratch.grbuf[0],576);
            t1=music_cycles(); decode+=t1-t0;
            for (ch=0;ch<nch;ch++,gr_info++) {
                int aa_bands=31;
                int n_long_bands=(gr_info->mixed_block_flag ? 2 : 0) << (int)(HDR_GET_MY_SAMPLE_RATE(dec->header)==2);
                if (gr_info->n_short_sfb) {
                    aa_bands=n_long_bands-1;
                    L3_reorder(scratch.grbuf[ch]+n_long_bands*18,scratch.syn[0],gr_info->sfbtab+gr_info->n_long_sfb);
                }
                L3_antialias(scratch.grbuf[ch],aa_bands);
                L3_imdct_gr(scratch.grbuf[ch],dec->mdct_overlap[ch],gr_info->block_type,n_long_bands);
                L3_change_sign(scratch.grbuf[ch]);
            }
            t2=music_cycles(); imdct+=t2-t1;
            memset(scratch.syn[32],0,sizeof(scratch.syn[32]));
            mp3d_synth_granule(dec->qmf_state,scratch.grbuf[0],18,nch,pcm,scratch.syn[0]);
            t3=music_cycles(); synth+=t3-t2;
            pcm_convert(out,pcm,576u*(unsigned)nch);
            t0=music_cycles(); convert+=t0-t3;
        }
    }
    L3_save_reservoir(dec,&scratch);
    uint64_t end=music_cycles(); decode+=end-t0;
    struct music_cost *c=&d->cost;
    c->decode+=decode; c->imdct+=imdct; c->synth+=synth; c->pcm+=convert;
    if (success) {
        c->frames++;
        if (!c->best || end-start<c->best) c->best=end-start;
        unsigned flags=fp_flags_take();
        c->de+=!!(flags&2u); c->ue+=!!(flags&16u);
        if (!(c->frames&7u)) {
            c->tiny+=tiny_floats(dec->qmf_state,15*64)+tiny_floats(dec->mdct_overlap[0],2*9*32);
            c->scans++;
        }
    }
    return success*hdr_frame_samples(dec->header);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
static size_t timed_read(struct music_decoder *d, void *buffer, size_t size, size_t count)
{
    uint64_t begin=music_cycles(); size_t n=fread(buffer,size,count,d->file);
    d->cost.io+=music_cycles()-begin; return n;
}
static int mp3_block(struct music_decoder *d)
{
    while (!cancelled(d)) {
        /* Refill in batches, not a tiny storage read for every MPEG frame.
         * Accumulate short reads without discarding an incomplete frame.
         * f4-37: frames are consumed by offset; the remainder moves to the
         * front only before a refill, not after every frame. */
        if (d->input_count<MP3_INPUT/2 && !feof(d->file)) {
            if (d->input_pos) { memmove(d->input,d->input+d->input_pos,d->input_count); d->input_pos=0; }
            while (d->input_count<MP3_INPUT && !feof(d->file)) {
                if (cancelled(d)) return -ECANCELED;
                size_t n=timed_read(d,d->input+d->input_count,1,MP3_INPUT-d->input_count);
                d->input_count+=(unsigned)n;
                if (ferror(d->file)) return -(errno ? errno : EIO);
                if (!d->mp3_decoded && (d->sync_bytes+=(unsigned)n)>256*1024) return -EINVAL;
                if (!n && !feof(d->file)) return -EIO;
            }
        }
        if (!d->input_count) { d->frames=d->mp3_decoded; return 0; }
        const uint8_t *input=d->input+d->input_pos;
        mp3dec_frame_info_t info={0};
        int frames=mp3_frame(d,input,(int)d->input_count,&info);
        if (info.frame_bytes<=0) {
            if (feof(d->file)) { d->frames=d->mp3_decoded; return 0; }
            /* A full window with no sync: retain a possible split header. */
            unsigned keep=d->input_count<3 ? d->input_count : 3;
            d->input_pos+=d->input_count-keep; d->input_count=keep;
            continue;
        }
        if (frames) mp3_length(d,input+info.frame_offset,(unsigned)(info.frame_bytes-info.frame_offset),(unsigned)frames);
        d->input_count-=(unsigned)info.frame_bytes; d->input_pos+=(unsigned)info.frame_bytes;
        if (!frames) continue;
        if (info.layer!=3 || (info.channels!=1 && info.channels!=2) || info.hz<=0) return -EINVAL;
        if (d->rate && (d->rate!=(unsigned)info.hz || d->channels!=(unsigned)info.channels)) return -ENOTSUP;
        d->rate=(unsigned)info.hz; d->channels=(unsigned)info.channels;
        d->mp3_decoded+=(unsigned)frames;
        d->offset=0; d->count=(unsigned)frames; return frames;
    }
    return -ECANCELED;
}
/* Refill the source block. Cancellation is checked per block (f4-37: no
 * longer per source frame; mp3_block also checks it per MPEG frame). */
static int refill(struct music_decoder *d)
{
    if (cancelled(d)) return -ECANCELED;
    d->offset=d->count=0;
    if (d->kind==1) {
        long p=ftell(d->file); if (p<0) return -EIO;
        uint64_t used=(uint64_t)(p-d->data);
        if (used>=d->bytes) return 0;
        unsigned frame_bytes=d->channels*d->bits/8;
        unsigned n=(unsigned)((d->bytes-used)/frame_bytes); if (n>1024) n=1024;
        if (timed_read(d,d->raw,frame_bytes,n)!=n) return -EIO;
        uint64_t begin=music_cycles();
        for (unsigned i=0;i<n*d->channels;i++) {
            unsigned v=d->bits==8 ? d->raw[i] : u16(d->raw+2*i);
            d->pcm[i]=(int16_t)(d->bits==8 ? ((int)v-128)*256 : v<32768 ? (int)v : (int)v-65536);
        }
        d->cost.decode+=music_cycles()-begin; d->cost.frames++;
        d->count=n;
    } else if (d->kind==2) {
        int n=mp3_block(d); if (n<=0) return n;
    } else {
        uint64_t begin=music_cycles();
        int n=stb_vorbis_get_samples_short_interleaved(d->ogg,(int)d->channels,d->pcm,2048);
        d->cost.decode+=music_cycles()-begin; d->cost.frames++;
        if (!n) return ferror(d->file) ? -(errno ? errno : EIO) : stb_vorbis_get_error(d->ogg) ? -EIO : 0;
        d->count=(unsigned)n;
    }
    return 1;
}
static inline __attribute__((always_inline)) int source(struct music_decoder *d, int16_t out[2])
{
    if (d->offset==d->count) { int r=refill(d); if (r<=0) return r; }
    const int16_t *frame=d->pcm+d->offset*d->channels;
    out[0]=frame[0]; out[1]=frame[d->channels-1];
    d->offset++; return 1;
}
static int rewind_source(struct music_decoder *d, uint64_t frame)
{
    d->count=d->offset=d->input_pos=d->input_count=0; d->primed=d->error=0;
    if (d->kind==1) return fseek(d->file,d->data+(long)(frame*d->channels*d->bits/8),SEEK_SET) ? -EIO : 0;
    if (d->kind==3) return stb_vorbis_seek(d->ogg,(unsigned)frame) ? 0 : -EIO;
    if (fseek(d->file,d->data,SEEK_SET)) return -EIO;
    clearerr(d->file); mp3dec_init(&d->mp3); d->mp3_decoded=0; d->sync_bytes=0;
    int16_t discard[2];
    for (uint64_t i=0;i<frame;i++) { int r=source(d,discard); if (r<=0) return r; }
    return 0;
}
/* f4-37: the linear interpolation left+(right-left)*phase/output_rate used a
 * 64-bit signed division per channel and two 64-bit divisions for the phase
 * step per output frame (compiler-rt helpers on i686, each built on DIV r32,
 * 39 cycles on the P6). With g=gcd(rate, output_rate) the phase is a multiple
 * of g, so the same quotient is trunc(diff*p/period) with p=phase/g: below
 * 2^31 for |diff|<=65535 when period<=32768, and divided exactly by a
 * multiply-high with magic=ceil(2^(32+shift)/period), 2^shift<period<=
 * 2^(shift+1): the error u*(magic*period-2^(32+shift))/2^(32+shift) stays
 * below 2^-(shift+1) <= 1/period for u<2^31 (the round-up method of Granlund
 * and Montgomery, "Division by invariant integers using multiplication",
 * PLDI 1994). Larger periods keep the 64-bit division. Output is
 * bit-identical; music_x87_test.c checks every diff and phase for the common
 * rate pairs and the adapter against the replaced code on every vector. */
static unsigned gcd(unsigned a, unsigned b) { while (b) { unsigned t=a%b; a=b; b=t; } return a; }
static void ratio_setup(struct music_decoder *d)
{
    unsigned g=gcd(d->rate,d->output_rate);
    d->step=d->rate/g; d->period=d->output_rate/g; d->shift=0; d->magic=0;
    if (d->period>1 && d->period<=32768u) {
        while ((2u<<d->shift)<d->period) d->shift++;
        d->magic=(uint32_t)(((1ull<<(32+d->shift))+d->period-1)/d->period);
    }
}
static inline int lerp(uint32_t magic, unsigned shift, unsigned period, unsigned phase, int diff)
{
    if (!magic) return (int)((int64_t)diff*phase/period);
    uint32_t u=(uint32_t)(diff<0 ? -diff : diff)*phase;
    uint32_t q=(uint32_t)(((uint64_t)u*magic)>>32)>>shift;
    return diff<0 ? -(int)q : (int)q;
}
int music_decoder_open(struct music_decoder **out, const char *path, unsigned rate, music_cancel cancel, void *ctx)
{
    *out=NULL; if (!path || !rate) return -EINVAL;
    struct music_decoder *d=calloc(1,sizeof(*d)); if (!d) return -ENOMEM;
    d->cancel=cancel; d->ctx=ctx; d->output_rate=rate;
    const char *ext=strrchr(path,'.'); int err=-EINVAL;
    if (!ext) goto done;
    d->file=fopen(path,"rb"); if (!d->file) { err=-errno; goto done; }
    if (setvbuf(d->file,d->read_ahead,_IOFBF,sizeof(d->read_ahead))) { err=-EIO; goto done; }
    if (!strcasecmp(ext,".wav")) { d->kind=1; err=wav_open(d); }
    else if (!strcasecmp(ext,".mp3")) {
        d->kind=2; d->frames=UINT64_MAX; mp3dec_init(&d->mp3);
        err=mp3_start(d);
        if (!err) { int r=mp3_block(d); err=r>0 ? 0 : r<0 ? r : -EINVAL; }
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
    if (ferror(d->file)) err=-(errno ? errno : EIO);
    if (!err) ratio_setup(d);
    if (!err && cancelled(d)) err=-ECANCELED;
done:
    if (err) music_decoder_close(d); else *out=d;
    return err;
}
uint64_t music_decoder_length(const struct music_decoder *d)
{ return d && d->rate && d->frames!=UINT64_MAX ? (d->frames*d->output_rate+d->rate-1)/d->rate : 0; }
/* The output frame the read stops at: unknown length reads to EOF. */
static uint64_t music_decoder_limit(const struct music_decoder *d)
{ return d->frames==UINT64_MAX ? UINT64_MAX : music_decoder_length(d); }
unsigned music_decoder_rate(const struct music_decoder *d) { return d ? d->rate : 0; }
unsigned music_decoder_channels(const struct music_decoder *d) { return d ? d->channels : 0; }
int music_decoder_seek(struct music_decoder *d, uint64_t frame)
{
    uint64_t length=music_decoder_length(d); if (length && frame>length) frame=length;
    uint64_t target=frame*d->rate/d->output_rate; if (target>=d->frames) target=d->frames-1;
    int r=rewind_source(d,target); if (r) return r;
    /* Replaying an unknown-length stream may discover EOF before the target. */
    length=music_decoder_length(d);
    if (length && frame>length) { frame=length; target=d->frames-1; }
    d->position=frame; d->source_index=target; d->phase=(unsigned)(frame*d->step%d->period);
    return 0;
}
int music_decoder_read(struct music_decoder *d, int16_t *out, unsigned frames)
{
    if (d->error) return d->error;
    struct music_cost *c=&d->cost;
    uint64_t begin=music_cycles(), inner=c->decode+c->imdct+c->synth+c->pcm+c->io;
    unsigned n=0; int error=0;
    if (!d->primed && frames && d->position<music_decoder_limit(d)) {
        if (cancelled(d)) return -ECANCELED;
        int r=source(d,d->left); if (r<=0) return d->error=r;
        r=source(d,d->right); if (r<0) return d->error=r;
        if (!r) memcpy(d->right,d->left,sizeof(d->right));
        d->primed=1;
    }
    /* f4-37: the per-frame state lives in locals: under -fno-strict-aliasing
     * every store to out may alias a field of d. d is current again before a
     * refill and on return. */
    const unsigned step=d->step, period=d->period, shift=d->shift, last=d->channels-1;
    const uint32_t magic=d->magic; const int16_t *pcm=d->pcm;
    unsigned phase=d->phase, offset=d->offset, count=d->count;
    uint64_t position=d->position, index=d->source_index, known=d->frames, limit=music_decoder_limit(d);
    int l0=d->left[0], l1=d->left[1], r0=d->right[0], r1=d->right[1];
    const unsigned stride=d->channels;
    while (!error && n<frames && position<limit) {
        if (!(n&1023u) && cancelled(d)) { error=-ECANCELED; break; }
        /* A run of output frames whose sources all lie in the decoded block:
         * no refill, frame-count or cancellation check inside, the same
         * arithmetic as the frame-by-frame step below. k frames advance
         * floor((phase+k*step)/period) source frames, at most `sources`. */
        unsigned todo=frames-n, gap=1024u-(n&1023u), sources=count-offset;
        if (todo>gap) todo=gap;
        if (limit-position<todo) todo=(unsigned)(limit-position);
        if (known!=UINT64_MAX && known-1-index<sources) sources=(unsigned)(known-1-index);
        unsigned most=((sources+1u)*period-1u-phase)/step, k=most<todo ? most : todo;
        if (k) {
            int16_t *o=out+2*n; const int16_t *next=pcm+offset*stride, *first=next;
            if (step==1u && period==1u) {
                for (unsigned j=0;j<k;j++,o+=2,next+=stride) {
                    o[0]=(int16_t)l0; o[1]=(int16_t)l1; l0=r0; l1=r1; r0=next[0]; r1=next[last];
                }
            } else {
                for (unsigned j=0;j<k;j++,o+=2) {
                    int a=l0, b=l1;
                    if (phase) { a+=lerp(magic,shift,period,phase,r0-l0); b+=lerp(magic,shift,period,phase,r1-l1); }
                    o[0]=(int16_t)a; o[1]=(int16_t)b;
                    for (phase+=step;phase>=period;phase-=period,next+=stride) { l0=r0; l1=r1; r0=next[0]; r1=next[last]; }
                }
            }
            unsigned used=(unsigned)(next-first)/stride;
            offset+=used; index+=used; n+=k; position+=k;
            continue;
        }
        int a=l0, b=l1;
        if (phase) { a+=lerp(magic,shift,period,phase,r0-l0); b+=lerp(magic,shift,period,phase,r1-l1); }
        out[2*n]=(int16_t)a; out[2*n+1]=(int16_t)b;
        n++; position++;
        unsigned advance=0; phase+=step;
        while (phase>=period) { phase-=period; advance++; }
        while (advance-- && index+1<known) {
            l0=r0; l1=r1; index++;
            if (offset==count) {
                d->offset=offset;
                int r=refill(d);
                offset=d->offset; count=d->count;
                if (d->frames!=known) { known=d->frames; limit=music_decoder_limit(d); }
                if (r<0) { error=r; break; }
                if (!r) { r0=l0; r1=l1; continue; }
            }
            const int16_t *frame=pcm+offset*d->channels; r0=frame[0]; r1=frame[last]; offset++;
        }
    }
    d->phase=phase; d->offset=offset; d->position=position; d->source_index=index;
    d->left[0]=(int16_t)l0; d->left[1]=(int16_t)l1; d->right[0]=(int16_t)r0; d->right[1]=(int16_t)r1;
    c->out+=n;
    uint64_t total=music_cycles()-begin, stages=c->decode+c->imdct+c->synth+c->pcm+c->io-inner;
    c->adapter+=total>stages ? total-stages : 0;
    if (error==-ECANCELED) return error;
    if (error) d->error=error;
    return n ? (int)n : d->error;
}
void music_decoder_cost(struct music_decoder *d, struct music_cost *out)
{ *out=d->cost; memset(&d->cost,0,sizeof(d->cost)); }
void music_decoder_close(struct music_decoder *d)
{ if (d) { if (d->ogg) stb_vorbis_close(d->ogg); if (d->file) fclose(d->file); free(d->vorbis_memory); free(d); } }
