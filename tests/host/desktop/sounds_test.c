/* SPDX-License-Identifier: MIT */
#include "sounds.h"
#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mode, calls; static int16_t output[50000]; static unsigned used;
static int sink(void *ctx, const int16_t *p, unsigned n)
{
    (void)ctx; calls++;
    if (mode && calls == 1) return -EAGAIN;
    if (mode && calls == 2) return -EINTR;
    if (mode && calls == 3) n = n > 3 ? 3 : n;
    memcpy(output+used,p,n*2); used += n; return (int)n;
}
static int fail(void *ctx, const int16_t *p, unsigned n) { (void)ctx;(void)p;(void)n;return -EIO; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    struct sound_scheme s, saved; sound_defaults(&s);
    char text[SOUND_TEXT_MAX], path[128]; size_t n = sound_format(&s,text,sizeof(text));
    assert(n); saved = s; memset(&s,0,sizeof(s)); assert(!sound_parse(&s,text,n)); assert(!memcmp(&saved,&s,sizeof(s)));
    const char custom[] = "# comment\r\n enabled = 0\r\n error = custom.wav\n question=\n future=abc\n";
    assert(!sound_parse(&s,custom,sizeof(custom)-1)); assert(!s.enabled); assert(!strcmp(s.file[SOUND_ERROR],"custom.wav"));
    assert(!sound_path(&s,SOUND_ERROR,path,sizeof(path))); assert(!strcmp(path,SOUND_DIR "/custom.wav"));
    assert(sound_path(&s,SOUND_QUESTION,path,sizeof(path)) == -ENOENT);
    assert(sound_path(&s,SOUND_EVENTS,path,sizeof(path)) == -EINVAL);
    assert(sound_path(&s,SOUND_ERROR,path,3) == -ENAMETOOLONG);
    assert(!sound_assign(&s,SOUND_ERROR,"/Documents/My Sound (1).wAv"));
    assert(!sound_path(&s,SOUND_ERROR,path,sizeof(path)) && !strcmp(path,"/Documents/My Sound (1).wAv"));
    static const char *bad[] = {"ui_volume=101", "ui_volume=-1", "ui_volume=", "ui_volume=abc", "enabled=2", "enabled=", "error=../bad.wav", "error=relative/bad.wav", "error=/bad\".wav", "error=a.ogg", "error=a..wav", "badline"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) { saved=s; assert(sound_parse(&s,bad[i],strlen(bad[i])) == -EINVAL); assert(!memcmp(&s,&saved,sizeof(s))); }
    assert(sound_parse(&s,"enabled=1\0error=a.wav",21) == -EINVAL);
    assert(sound_parse(&s,"",SOUND_TEXT_MAX+1) == -EINVAL);
    assert(!sound_format(&s,text,3));
    sound_defaults(&s);
    struct sound_throttle throttle = {0};
    assert(sound_coalesce(&throttle,SOUND_VOLUME,0));
    assert(!sound_coalesce(&throttle,SOUND_VOLUME,59999999));
    assert(sound_coalesce(&throttle,SOUND_VOLUME,60000000));
    assert(sound_coalesce(&throttle,SOUND_ERROR,60000001));
    assert(!sound_coalesce(&throttle,SOUND_VOLUME,119999999));
    assert(sound_coalesce(&throttle,SOUND_VOLUME,120000000));
    assert(sound_allowed(&s,0,0,0)); assert(!sound_allowed(&s,0,1,0)); assert(!sound_allowed(&s,1,0,1));
    s.enabled=0; assert(!sound_allowed(&s,0,0,0)); assert(sound_allowed(&s,1,0,0));
    s.volume=0;assert(!sound_allowed(&s,1,0,0));sound_defaults(&s);
    for (unsigned i=0;i<SOUND_EVENTS;i++) {
        snprintf(path,sizeof(path),"%s/%s",argv[1],s.file[i]); FILE *f=fopen(path,"rb"); assert(f);
        uint8_t data[SOUND_FILE_MAX+1]; n=fread(data,1,sizeof(data),f); fclose(f);
        struct sound_clip clip; assert(!sound_wav(data,n,&clip)); assert(clip.frames && clip.frames <= SOUND_RATE*2);
        for (size_t cut=0;cut<n;cut++) assert(sound_wav(data,cut,&clip) == -EINVAL);
        data[20]=3; assert(sound_wav(data,n,&clip) == -EINVAL); /* float */
    }
    /* Unknown odd-sized chunks with padding are valid before the fmt/data. */
    uint8_t wav[58] = {'R','I','F','F',50,0,0,0,'W','A','V','E','J','U','N','K',1,0,0,0,42,0,
        'f','m','t',' ',16,0,0,0,1,0,1,0,0x22,0x56,0,0,0x44,0xac,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,0x10,0x27,0xf0,0xd8};
    struct sound_clip clip; assert(!sound_wav(wav,sizeof(wav),&clip)); assert(clip.frames==2);
    uint8_t pcm[8] = {0x30,0x75,0xd0,0x8a,100,0,0x9c,0xff}; /* 30000,-30000,100,-100 */
    struct sound_player player; sound_reset(&player); clip=(struct sound_clip){pcm,4};
    assert(!sound_play(&player,clip)); assert(!sound_play(&player,clip)); assert(sound_play(&player,clip) == -EBUSY);
    mode=1; while (sound_active(&player)) assert(sound_pump(&player,sink,NULL)>=0);
    assert(used==4 && output[0]==32767 && output[1]==-32768 && output[2]==200 && output[3]==-200);
    sound_reset(&player); player.volume=25; used=0; calls=0;
    assert(!sound_play(&player,clip)); assert(!sound_play(&player,clip));
    while(sound_active(&player))assert(sound_pump(&player,sink,NULL)>=0);
    assert(used==4 && output[0]==15000 && output[1]==-15000 && output[2]==50 && output[3]==-50);
    assert(!sound_play(&player,clip)); assert(sound_pump(&player,fail,NULL)==-EIO); assert(sound_play(&player,clip)==-EIO);
    sound_reset(&player); assert(!sound_active(&player));
    int16_t tone[SOUND_RATE]; sound_tone(tone); assert(!tone[0] && !tone[SOUND_RATE-1]);
    int crossings=0; for (unsigned i=110;i<SOUND_RATE-110;i++) if (tone[i-1]<=0 && tone[i]>0) crossings++;
    assert(crossings>=989 && crossings<=991);
    used=0; calls=0; assert(!sound_play(&player,(struct sound_clip){(uint8_t *)tone,SOUND_RATE}));
    while(sound_active(&player)) assert(sound_pump(&player,sink,NULL)>=0);
    assert(used==SOUND_RATE && !memcmp(output,tone,sizeof(tone)));
    struct ciuki_message msg; struct desk_packet packet;
    desk_message(&msg,DESK_SETTINGS,24); msg.data[0]=DESK_SHELL_VERSION; desk_put32(msg.data+8,DESK_SETTINGS_SOUND_TEST); desk_put32(msg.data+12,SOUND_ERROR);
    assert(desk_parse(&msg,1,NULL,&packet)); desk_put32(msg.data+12,SOUND_EVENTS); assert(!desk_parse(&msg,1,NULL,&packet));
    desk_message(&msg,DESK_SOUND,8); msg.data[0]=DESK_SHELL_VERSION; desk_put32(msg.data+4,SOUND_QUESTION); assert(desk_parse(&msg,1,NULL,&packet));
    desk_put32(msg.data+4,SOUND_AUTOPLAY); assert(desk_parse(&msg,1,NULL,&packet));
    desk_put32(msg.data+4,SOUND_STARTUP); assert(!desk_parse(&msg,1,NULL,&packet));
    puts("PASS sounds: schemes, paths, WAV bounds/padding, two voices, saturation, partial/EAGAIN/EINTR writes, reset/failure, 1 kHz tone, protocol");
}
