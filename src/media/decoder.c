/* One bounded-chunk decoder front end for the four supported audio formats. */
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define STB_VORBIS_IMPLEMENTATION
#include "../../third_party/audio/dr_libs/dr_wav.h"
#include "../../third_party/audio/dr_libs/dr_mp3.h"
#include "../../third_party/audio/dr_libs/dr_flac.h"
#include "../../third_party/audio/stb/stb_vorbis.c"
#include "decoder.h"
#include <string.h>

enum { MEDIA_NONE, MEDIA_WAV, MEDIA_MP3, MEDIA_FLAC, MEDIA_OGG };
static drwav wav;
static drmp3 mp3;
static drflac *flac;
static stb_vorbis *ogg;
static int kind;
static uint32_t source_rate, channels, output_phase;
static uint64_t source_total;
static int16_t native_pcm[MEDIA_INPUT_FRAMES * 8];

static const char *extension(const char *path)
{
    const char *p=path,*dot=0;
    while(*p){if(*p=='.')dot=p+1;++p;}
    return dot;
}
static int extension_is(const char *ext,const char *name)
{
    while(*ext&&*name){char a=*ext++,b=*name++;if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b)return 0;}
    return !*ext&&!*name;
}

int media_decoder_open(const char *path,uint32_t *rate,uint32_t *total)
{
    const char *ext=extension(path);uint64_t out;
    media_decoder_close(); if(!ext)return 0;
    if(extension_is(ext,"wav")){
        if(!drwav_init_file(&wav,path,0))return 0;
        kind=MEDIA_WAV;source_rate=wav.sampleRate;channels=wav.channels;source_total=wav.totalPCMFrameCount;
    }else if(extension_is(ext,"mp3")){
        if(!drmp3_init_file(&mp3,path,0))return 0;
        kind=MEDIA_MP3;source_rate=mp3.sampleRate;channels=mp3.channels;
        source_total=mp3.totalPCMFrameCount==UINT64_MAX?drmp3_get_pcm_frame_count(&mp3):mp3.totalPCMFrameCount;
    }else if(extension_is(ext,"flac")){
        flac=drflac_open_file(path,0);if(!flac)return 0;
        kind=MEDIA_FLAC;source_rate=flac->sampleRate;channels=flac->channels;source_total=flac->totalPCMFrameCount;
    }else if(extension_is(ext,"ogg")){
        int error=0;stb_vorbis_info info;
        ogg=stb_vorbis_open_filename(path,&error,0);if(!ogg)return 0;
        info=stb_vorbis_get_info(ogg);kind=MEDIA_OGG;source_rate=info.sample_rate;
        channels=(uint32_t)info.channels;source_total=stb_vorbis_stream_length_in_samples(ogg);
    }else return 0;
    if(source_rate<8000||source_rate>192000||!channels||channels>8){media_decoder_close();return 0;}
    output_phase=0;*rate=MEDIA_PCM_RATE;
    out=source_total==UINT64_MAX?0:(source_total*MEDIA_PCM_RATE)/source_rate;
    *total=out>0xFFFFFFFFULL?0xFFFFFFFFUL:(uint32_t)out;
    return 1;
}

static uint32_t read_native(uint32_t requested)
{
    switch(kind){
    case MEDIA_WAV:return (uint32_t)drwav_read_pcm_frames_s16(&wav,requested,native_pcm);
    case MEDIA_MP3:return (uint32_t)drmp3_read_pcm_frames_s16(&mp3,requested,native_pcm);
    case MEDIA_FLAC:return (uint32_t)drflac_read_pcm_frames_s16(flac,requested,native_pcm);
    case MEDIA_OGG:return (uint32_t)stb_vorbis_get_samples_short_interleaved(ogg,(int)channels,native_pcm,(int)(requested*channels));
    default:return 0;
    }
}

int media_decoder_read(int16_t *stereo,uint32_t capacity,uint32_t *frames_read)
{
    uint32_t i,j,n,out=0;int16_t l,r;
    if(!kind||capacity<MEDIA_OUTPUT_FRAMES)return 0;
    n=read_native(MEDIA_INPUT_FRAMES);*frames_read=0;
    for(i=0;i<n;++i){
        l=native_pcm[i*channels];r=channels>1?native_pcm[i*channels+1]:l;
        output_phase+=MEDIA_PCM_RATE;
        while(output_phase>=source_rate){
            if(out>=capacity)return 0;
            stereo[out*2]=l;stereo[out*2+1]=r;++out;
            output_phase-=source_rate;
        }
    }
    *frames_read=out;return 1;
}

int media_decoder_seek(uint32_t output_frame)
{
    uint64_t native=(uint64_t)output_frame*source_rate/MEDIA_PCM_RATE;
    int ok=0;
    switch(kind){
    case MEDIA_WAV:ok=drwav_seek_to_pcm_frame(&wav,native);break;
    case MEDIA_MP3:ok=drmp3_seek_to_pcm_frame(&mp3,native);break;
    case MEDIA_FLAC:ok=drflac_seek_to_pcm_frame(flac,native);break;
    case MEDIA_OGG:ok=stb_vorbis_seek(ogg,(unsigned int)native);break;
    }
    output_phase=0;return ok;
}

void media_decoder_close(void)
{
    if(kind==MEDIA_WAV)drwav_uninit(&wav);
    else if(kind==MEDIA_MP3)drmp3_uninit(&mp3);
    else if(kind==MEDIA_FLAC&&flac)drflac_close(flac);
    else if(kind==MEDIA_OGG&&ogg)stb_vorbis_close(ogg);
    kind=MEDIA_NONE;flac=0;ogg=0;source_rate=channels=output_phase=0;source_total=0;
}
