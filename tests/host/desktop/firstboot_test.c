/* SPDX-License-Identifier: MIT -- case-insensitive FAT record model with
 * separately committed bytes. The production native adapter is checked by
 * the companion target compilation and native fixtures. */
#include "../../../apps/desktop/firstboot.h"
#include "../../../apps/desktop/catalog.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/devices.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/registry.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static struct {
    char bytes[FB_STATE_BYTES], durable[FB_STATE_BYTES];
    int absent, commit_error, hang_sound, hang_network, hang_entropy;
    unsigned commits, calls[FB_STEPS], records, actions[2], games, accel_state;
    uint64_t started[FB_STEPS], ended[FB_STEPS];
    char log[4096];
} fat;
static int rd(void *ctx,char *out,size_t cap,size_t *n)
{(void)ctx;if(fat.absent)return -ENOENT;*n=strlen(fat.bytes);assert(*n<cap);memcpy(out,fat.bytes,*n);return 0;}
static int append(void *ctx,const char *text,size_t n)
{
    (void)ctx;assert(strlen(fat.bytes)+n<sizeof(fat.bytes));
    strncat(fat.bytes,text,n);if(fat.commit_error)return -EIO;
    strcpy(fat.durable,fat.bytes);fat.commits++;return 0;
}
static int step(void *ctx,unsigned s,int start,char *text,size_t cap)
{
    (void)ctx;fat.calls[s]++;(void)start;
    if(s==FB_ACCEL)return fb_accel_outcome(fat.accel_state==0,0,fat.accel_state==2,text,cap);
    if((s==FB_SOUND&&fat.hang_sound)||(s==FB_NETWORK&&fat.hang_network)||(s==FB_ENTROPY&&fat.hang_entropy)){
        snprintf(text,cap,"Waiting for service; HTTPS needs ready entropy and a valid clock.");return -EAGAIN;
    }
    if(s==FB_GAMES)fat.games|=7;
    snprintf(text,cap,"%s complete",fb_names[s]);return 0;
}
static void record(void *ctx,const char *line)
{(void)ctx;fat.records++;assert(strstr(fat.durable,"step="));strcat(fat.log,line);strcat(fat.log,"\n");}
static void action(void *ctx,unsigned a){(void)ctx;assert(a<2);fat.actions[a]++;}
static struct fb_host host={NULL,rd,append,step,NULL,record,action};
static void reset(const char *fixture){memset(&fat,0,sizeof(fat));if(fixture)strcpy(fat.bytes,fixture);else fat.absent=1;strcpy(fat.durable,fat.bytes);}
static const char pending[]="phase=installed\nrelease=fixture\nfirstboot=pending\n";
static void run(struct firstboot *f)
{
    for(uint64_t ms=0;ms<40000&&!f->done&&!f->error;ms+=20){
        unsigned s=f->step;
        if(s<FB_STEPS&&!fat.started[s])fat.started[s]=ms+1;
        if(s==FB_SUMMARY)fb_finish(f);
        fb_tick(f,ms);
        if(s<FB_STEPS&&f->result[s]&&!fat.ended[s])fat.ended[s]=ms+1;
    }
    assert(f->done);assert(strstr(fat.durable,"firstboot=done\n"));
}
static unsigned occurrences(const char *s,const char *needle)
{unsigned n=0;for(; (s=strstr(s,needle));s+=strlen(needle))n++;return n;}
static void engine_tests(void)
{
    struct firstboot f;
    reset(NULL);assert(fb_begin(&f,&host)==0&&!fat.commits);
    reset("phase=installed\nfirstboot=done\n");assert(!fb_begin(&f,&host)&&!fat.commits);
    reset("phase=copy\nfirstboot=pending\n");assert(!fb_begin(&f,&host));
    reset(pending);assert(fb_begin(&f,&host)==1&&f.step==0);
    strcat(fat.bytes,"step=accel result=ok\nstep=sound result=skipped\nstep=network result=failed\n");
    assert(fb_begin(&f,&host)==1&&f.step==3);
    strcat(fat.bytes,"step=games started\nstep=games result=");
    assert(fb_begin(&f,&host)==1&&f.step==3);run(&f);
    assert(fat.calls[0]==0&&fat.calls[1]==0&&fat.calls[2]==0);
    assert(occurrences(fat.bytes,"step=games result=ok\n")==1);
    assert(!fb_begin(&f,&host));
    reset(pending);assert(fb_begin(&f,&host)==1);run(&f);
    assert(fat.records==FB_STEPS&&fat.commits==FB_STEPS+1&&fat.games==7);
    assert(strstr(fat.log,"[firstboot] step=summary result=ok ms="));
    for(unsigned i=0;i<FB_STEPS;i++){char token[64];snprintf(token,sizeof(token),"step=%s result=ok\n",fb_names[i]);assert(occurrences(fat.bytes,token)==1);}
    /* Explicit reinstall/run fixtures: every result is once in each run. */
    reset(pending);assert(fb_begin(&f,&host)==1);run(&f);assert(fat.games==7);
    reset(pending);fat.hang_sound=fat.hang_network=fat.hang_entropy=1;
    assert(fb_begin(&f,&host)==1);run(&f);
    assert(f.result[FB_SOUND]==FB_FAILED&&f.result[FB_NETWORK]==FB_SKIPPED&&f.result[FB_ENTROPY]==FB_FAILED);
    assert(fat.ended[FB_SOUND]-fat.started[FB_SOUND]<=10000);
    assert(fat.ended[FB_NETWORK]-fat.started[FB_NETWORK]<=10000);
    assert(fat.ended[FB_ENTROPY]-fat.started[FB_ENTROPY]<=5000);
    /* A dead time source cannot turn a never-answering service into a spin. */
    reset(pending);fat.hang_sound=1;assert(fb_begin(&f,&host)==1);
    for(unsigned i=0;i<600;i++)fb_tick(&f,0);
    assert(f.result[FB_SOUND]==FB_FAILED);
    reset(pending);assert(fb_begin(&f,&host)==1);fat.commit_error=1;fb_tick(&f,0);
    assert(f.error==-EIO&&!fat.records&&!strstr(fat.durable,"step="));
    strcpy(fat.bytes,fat.durable);fat.commit_error=0;assert(fb_begin(&f,&host)==1&&f.step==0);
    reset(pending);assert(fb_begin(&f,&host)==1);fb_action(&f,FB_DISPLAY);fb_action(&f,FB_CHANGE_CLOCK);
    assert(fat.actions[0]==1&&fat.actions[1]==1);
    /* Already completed summary, interrupted before done: only final commit. */
    for(unsigned i=0;i<FB_STEPS;i++)f.result[i]=FB_OK;
    fb_tick(&f,0);assert(f.done&&fat.commits==1);
    puts("PASS firstboot triggers, crash/resume, durability, idempotence, time bounds, actions");
}
static unsigned present=31,queries;
static int game(void *ctx,unsigned id,int remove,uint32_t *bytes)
{(void)ctx;assert(!remove&&id>=1&&id<=5);queries++;*bytes=4096;return !!(present&(1u<<(id-1)));}
int ciuki_devices(struct ciuki_devices_info *v){(void)v;errno=ENOSYS;return -1;}
static void games_test(const char *directory)
{
    char text[192];assert(!fb_register_games(directory,game,NULL,text,sizeof(text)));
    struct reg_root *r=reg_open_root_at("Programs",directory);assert(r);
    char before[96];snprintf(before,sizeof(before),"%llu",(unsigned long long)reg_generation(r));
    assert(!fb_register_games(directory,game,NULL,text,sizeof(text)));
    char after[96];snprintf(after,sizeof(after),"%llu",(unsigned long long)reg_generation(r));assert(!strcmp(before,after));
    unsigned keys=0;char key[257];while(!reg_enum_keys(r,"Programs\\BundledGames",keys,key,sizeof(key)))keys++;
    assert(keys==DESK_GAME_COUNT&&queries==2*DESK_GAME_COUNT);reg_close_root(r);
    present=0;assert(fb_register_games(directory,game,NULL,text,sizeof(text))==1);
    puts("PASS catalogue registrations: exact identities, unchanged generation on second run, no resurrection");
}
static void frames(const char *directory,const uint32_t *portrait)
{
    struct firstboot f;reset(pending);assert(fb_begin(&f,&host)==1);
    uint32_t *pixels=malloc(640*480*4);assert(pixels);
    struct du_canvas c={pixels,640,480,{0,0,640,480}};
    const char *names[]={"pending","accelerated","failed-attempt","previous-hang","sound-timeout","summary","commit-error"};
    for(unsigned k=0;k<7;k++){
        memset(f.detail,0,sizeof(f.detail));
        if(k>=1&&k<=3){fb_accel_outcome(k==1,0,k==3,f.detail[0],sizeof(f.detail[0]));f.result[0]=k==1?FB_OK:FB_SKIPPED;f.step=1;}
        if(k==4){f.step=FB_SOUND;f.result[1]=FB_FAILED;strcpy(f.detail[1],"Audio check timed out; setup will continue.");}
        if(k==5){for(unsigned s=0;s<FB_SUMMARY;s++)f.result[s]=FB_OK;f.step=FB_SUMMARY;fb_tick(&f,0);}
        if(k==6)f.error=-EIO;
        fb_draw(&f,&c,portrait);
        char path[1024];snprintf(path,sizeof(path),"%s/%s.frame",directory,names[k]);
        FILE *out=fopen(path,"wb");assert(out);assert(fwrite(pixels,4,640*480,out)==640*480);assert(!fclose(out));
    }
    free(pixels);
}
int main(int argc,char **argv)
{
    assert(argc==3);engine_tests();games_test(argv[1]);
    uint32_t *portrait=malloc(256*256*4);assert(portrait);FILE *p=fopen(argv[2],"rb");assert(p);assert(fread(portrait,4,256*256,p)==256*256);fclose(p);
    frames(argv[1],portrait);free(portrait);return 0;
}
