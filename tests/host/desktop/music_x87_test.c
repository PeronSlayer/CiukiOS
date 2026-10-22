/* SPDX-License-Identifier: MIT
 * f4-37: the production MP3 decoder as built for the i686 baseline (x87, the
 * SDK's code generation flags, test_music.py) and as a host reference build.
 *   lerp                     the division-free interpolation against the
 *                            64-bit formula it replaced, and the integer PCM
 *                            conversion against minimp3's int16 rounding
 *   decode <out> <mp3>...    per vector: upstream minimp3's int16 PCM
 *                            (reference, music_x87_ref.c) and its denormal
 *                            flags, the production
 *                            frame driver's PCM, flags and a full state scan
 *                            after every frame, the adapter at 48 and 44.1 kHz
 *                            against the replaced 64-bit interpolation, a
 *                            positive control and garbage independence
 *   trace <mp3>              the production read loop between two SIGSTOPs
 *                            (cycle model, docs/validation/f4-37-*.md)
 *   cost <mp3> <rate>        per-stage cycles of the production read loop
 * Stack poisoning makes the uninitialised minimp3 scratch hold a chosen bit
 * pattern, as stack garbage does on the target. */
#define _GNU_SOURCE
#include "../../../apps/music/decoder.c"
#include <assert.h>
#include <signal.h>
#include <unistd.h>
void ref_mp3dec_init(mp3dec_t *);
int ref_mp3dec_decode_frame(mp3dec_t *, const uint8_t *, int, int16_t *, mp3dec_frame_info_t *);
#if defined(__i386__) && !defined(__SSE_MATH__)
#define ARCH "i686-x87"
#else
#define ARCH "reference"
#endif
__attribute__((noinline)) static void poison_stack(uint32_t pattern)
{
    volatile uint32_t area[16384];
    for (unsigned i=0;i<16384;i++) area[i]=pattern;
}
static uint8_t *load(const char *path, long *size)
{
    FILE *f=fopen(path,"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); *size=ftell(f); assert(*size>0 && !fseek(f,0,SEEK_SET));
    uint8_t *data=malloc((size_t)*size); assert(data && fread(data,1,(size_t)*size,f)==(size_t)*size);
    fclose(f); return data;
}
static uint64_t fnv(uint64_t h, const int16_t *pcm, size_t n)
{
    for (size_t i=0;i<n;i++) { h^=(uint16_t)pcm[i]; h*=1099511628211ull; }
    return h;
}
static void lerp_check(void)
{
    static const unsigned pairs[][2]={{44100,48000},{22050,48000},{11025,48000},{32000,48000},{8000,48000},
        {48000,44100},{22050,44100},{6000,48000},{96000,48000},{44056,48000},{12345,48000},{47999,48000},{1,32768},{1,32767}};
    uint64_t checks=0; uint32_t seed=1;
    for (unsigned k=0;k<sizeof(pairs)/sizeof(*pairs);k++) {
        static struct music_decoder d; memset(&d,0,sizeof(d)); d.rate=pairs[k][0]; d.output_rate=pairs[k][1]; ratio_setup(&d);
        unsigned g=gcd(d.rate,d.output_rate);
        assert(d.step*g==d.rate && d.period*g==d.output_rate);
        int every=d.period<=1024;
        for (unsigned p=1;p<d.period;p++) {
            if (!every && p>64 && p<d.period-64 && (seed=seed*1103515245u+12345u)>>24) continue;
            d.phase=p;
            for (int diff=-65535;diff<=65535;diff++) {
                /* The replaced expression: phase in output-rate units. */
                int64_t want=(int64_t)diff*(int64_t)(p*g)/(int64_t)d.output_rate;
                int got=lerp(d.magic,d.shift,d.period,p,diff);
                if (got!=(int)want) {
                    fprintf(stderr,"lerp %u->%u p=%u diff=%d got=%d want=%lld\n",d.rate,d.output_rate,p,diff,got,(long long)want);
                    abort();
                }
                checks++;
            }
        }
    }
    printf("lerp arch=%s checks=%llu exact\n",ARCH,(unsigned long long)checks);
}
/* minimp3's mp3d_scale_pcm (minimp3.h:1430-1442) on sample=32768*f, exactly:
 * in double, sample and sample+0.5 are exact, as on x87. */
static int16_t scale_model(float f)
{
    double sample=(double)f*32768.0;
    if (sample>=32766.5) return 32767;
    if (sample<=-32767.5) return -32768;
    int s=(int)(sample+0.5);
    return (int16_t)(s-(s<0));
}
static void pcm_check(void)
{
    uint64_t checks=0; uint32_t seed=7;
    for (uint32_t exponent=0;exponent<256;exponent++) {
        int every=exponent>=100 && exponent<=150;
        for (uint32_t mantissa=0;mantissa<(1u<<23);mantissa++) {
            if (!every && mantissa>4096 && mantissa<(1u<<23)-4096 && (seed=seed*1103515245u+12345u)>>20) continue;
            for (uint32_t sign=0;sign<2;sign++) {
                uint32_t bits=sign<<31|exponent<<23|mantissa; float f; __builtin_memcpy(&f,&bits,4);
                if (exponent==255 && mantissa) continue; /* NaN: undefined in minimp3 */
                if (pcm_s16(f)!=scale_model(f)) {
                    fprintf(stderr,"pcm %08x got=%d want=%d\n",bits,pcm_s16(f),scale_model(f)); abort();
                }
                checks++;
            }
        }
    }
    printf("pcm arch=%s checks=%llu exact\n",ARCH,(unsigned long long)checks);
}
static void write_pcm(const char *dir, const char *name, const char *kind, const int16_t *pcm, size_t samples)
{
    char path[1024]; snprintf(path,sizeof(path),"%s/%s.%s.%s.pcm",dir,name,ARCH,kind);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(pcm,2,samples,f)==samples); fclose(f);
}
/* Upstream minimp3, frame by frame, with the stack poisoned before each call. */
static size_t upstream(const uint8_t *data, long size, uint32_t pattern, int16_t *out, size_t room,
                       unsigned *channels, unsigned *hz, unsigned *de, unsigned *ue, unsigned *frames)
{
    static mp3dec_t dec; ref_mp3dec_init(&dec); size_t n=0; long pos=0; int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    *de=*ue=*frames=0; (void)fp_flags_take();
    while (pos<size) {
        mp3dec_frame_info_t info={0}; poison_stack(pattern); (void)fp_flags_take();
        int s=ref_mp3dec_decode_frame(&dec,data+pos,(int)(size-pos),pcm,&info);
        unsigned flags=fp_flags_take();
        if (info.frame_bytes<=0) break;
        pos+=info.frame_bytes;
        if (!s) continue;
        (*frames)++; *de+=!!(flags&2u); *ue+=!!(flags&16u);
        *channels=(unsigned)info.channels; *hz=(unsigned)info.hz;
        assert(n+(size_t)s*info.channels<=room);
        memcpy(out+n,pcm,(size_t)s*info.channels*2); n+=(size_t)s*info.channels;
    }
    return n;
}
/* The production frame driver, one MPEG frame per mp3_block. */
struct production { size_t samples; unsigned frames, tiny_full; struct music_cost cost; };
static struct production produce(const char *path, unsigned rate, uint32_t pattern, int16_t *out, size_t room, int inject)
{
    struct production r={0}; struct music_decoder *d=NULL;
    poison_stack(pattern); (void)fp_flags_take();
    assert(!music_decoder_open(&d,path,rate,NULL,NULL) && d->kind==2);
    for (;;) {
        assert(r.samples+(size_t)d->count*d->channels<=room);
        memcpy(out+r.samples,d->pcm,(size_t)d->count*d->channels*2); r.samples+=(size_t)d->count*d->channels;
        r.frames++;
        r.tiny_full+=tiny_floats(d->mp3.qmf_state,15*64)+tiny_floats(d->mp3.mdct_overlap[0],2*9*32);
        if (inject && r.frames==2) {
            /* Positive control: a denormal the next IMDCT overlap-add reads. */
            uint32_t bits=1; __builtin_memcpy(&d->mp3.mdct_overlap[0][0],&bits,4);
        }
        poison_stack(pattern);
        int n=mp3_block(d); assert(n>=0);
        if (!n) break;
    }
    music_decoder_cost(d,&r.cost); music_decoder_close(d); return r;
}
/* The replaced adapter: 64-bit interpolation over the production source PCM. */
static size_t reference_adapter(const int16_t *src, size_t frames, unsigned channels, unsigned rate, unsigned output,
                                uint64_t length, int16_t *out)
{
    uint64_t position=0, index=0; unsigned phase=0; size_t n=0, next=0; int16_t left[2], right[2];
    if (!frames) return 0;
#define SOURCE(dst) do { (dst)[0]=src[next*channels]; (dst)[1]=src[next*channels+channels-1]; next++; } while (0)
    SOURCE(left);
    if (next<frames) SOURCE(right); else memcpy(right,left,sizeof(right));
    while (position<length) {
        for (unsigned c=0;c<2;c++) out[2*n+c]=(int16_t)(left[c]+(int64_t)(right[c]-left[c])*phase/output);
        n++; position++;
        uint64_t p=(uint64_t)phase+rate; unsigned advance=(unsigned)(p/output); phase=(unsigned)(p%output);
        while (advance-- && index+1<frames) {
            memcpy(left,right,sizeof(left)); index++;
            if (next<frames) SOURCE(right); else memcpy(right,left,sizeof(right));
        }
    }
#undef SOURCE
    return n;
}
static void decode(const char *dir, const char *path)
{
    const char *name=strrchr(path,'/'); name=name ? name+1 : path;
    long size; uint8_t *data=load(path,&size);
    size_t room=(size_t)size*64+1152*8; int16_t *ref=malloc(room*2), *prod=malloc(room*2), *other=malloc(room*2);
    assert(ref && prod && other);
    unsigned channels=0, hz=0, up_de, up_ue, up_frames, de_tmp, ue_tmp, fr_tmp;
    /* Reference PCM; the upstream flags with a denormal-patterned stack. */
    size_t ref_n=upstream(data,size,1,ref,room,&channels,&hz,&up_de,&up_ue,&up_frames);
    write_pcm(dir,name,"ref",ref,ref_n);
    /* Production driver at the source rate, a denormal-patterned stack. */
    struct production p=produce(path,hz,1,prod,room,0);
    write_pcm(dir,name,"prod",prod,p.samples);
    uint64_t hash=fnv(1469598103934665603ull,prod,p.samples);
    /* Garbage independence: NaN, normal and zero patterns, production and upstream. */
    static const uint32_t patterns[]={0x7fc00000u,0x3f800000u,0};
    unsigned independent=1;
    for (unsigned i=0;i<3;i++) {
        struct production q=produce(path,hz,patterns[i],other,room,0);
        independent&=q.samples==p.samples && fnv(1469598103934665603ull,other,q.samples)==hash;
        size_t m=upstream(data,size,patterns[i],other,room,&channels,&hz,&de_tmp,&ue_tmp,&fr_tmp);
        independent&=m==ref_n && !memcmp(other,ref,m*2);
    }
    /* Positive control: an injected denormal must be seen. */
    struct production control=produce(path,hz,0,other,room,1);
    /* Adapter at 48 and 44.1 kHz against the replaced interpolation. */
    unsigned mismatch=0; size_t outs[2]={0};
    for (unsigned k=0;k<2;k++) {
        unsigned output=k ? 44100 : 48000; struct music_decoder *d=NULL;
        assert(!music_decoder_open(&d,path,output,NULL,NULL));
        /* Varied request sizes move the block-run and cancellation-check boundaries. */
        static const unsigned chunks[]={4096,1,7,137,1023,1024,1025,2048};
        size_t n=0; int r; unsigned call=0;
        while ((r=music_decoder_read(d,other+2*n,chunks[call++%8]))>0) { n+=(size_t)r; assert(2*n+8192<=room); }
        assert(!r); uint64_t length=music_decoder_length(d); music_decoder_close(d);
        int16_t *want=malloc((n+16)*4); assert(want);
        size_t m=reference_adapter(prod,p.samples/channels,channels,hz,output,length ? length : n,want);
        if (m!=n || memcmp(want,other,n*4)) mismatch++;
        outs[k]=n; free(want);
    }
    printf("vector=%s arch=%s frames=%u channels=%u rate=%u up_frames=%u up_de=%u up_ue=%u prod_frames=%u prod_de=%u prod_ue=%u "
           "tiny_full=%u tiny_sampled=%u scans=%u control_de=%u garbage_independent=%u adapter_mismatch=%u out48=%zu out441=%zu\n",
           name,ARCH,p.cost.frames,channels,hz,up_frames,up_de,up_ue,p.cost.frames,p.cost.de,p.cost.ue,
           p.tiny_full,p.cost.tiny,p.cost.scans,control.cost.de,independent,mismatch,outs[0],outs[1]);
    free(ref); free(prod); free(other); free(data);
}
static void cost(const char *path, unsigned rate, int trace)
{
    static int16_t pcm[4096*2]; struct music_decoder *d=NULL;
    assert(!music_decoder_open(&d,path,rate,NULL,NULL));
    struct music_cost total={0}, c; music_decoder_cost(d,&c); /* the open's first frame */
    if (trace) raise(SIGSTOP);
    int n; while ((n=music_decoder_read(d,pcm,4096))>0) {}
    if (trace) raise(SIGSTOP);
    assert(!n); music_decoder_cost(d,&total); music_decoder_close(d);
    if (trace) return;
    double f=total.frames ? total.frames : 1;
    printf("cost arch=%s file=%s rate=%u frames=%u out=%u cycles_per_frame decode=%.0f imdct=%.0f synth=%.0f pcm=%.0f adapter=%.0f io=%.0f best=%llu\n",
           ARCH,path,rate,total.frames,total.out,total.decode/f,total.imdct/f,total.synth/f,total.pcm/f,total.adapter/f,total.io/f,
           (unsigned long long)total.best);
}
int main(int argc, char **argv)
{
    assert(argc>=2);
    if (!strcmp(argv[1],"lerp")) { lerp_check(); pcm_check(); }
    else if (!strcmp(argv[1],"decode")) { assert(argc>=4); for (int i=3;i<argc;i++) decode(argv[2],argv[i]); }
    else if (!strcmp(argv[1],"trace") && argc==3) cost(argv[2],48000,1);
    else if (!strcmp(argv[1],"cost") && argc==4) cost(argv[2],(unsigned)atoi(argv[3]),0);
    else return 2;
    return 0;
}
