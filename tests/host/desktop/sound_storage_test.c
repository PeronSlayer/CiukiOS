/* SPDX-License-Identifier: MIT */
#include <unistd.h>
#include "sound_storage.h"
#include <ciuki/devices.h>
#include <ciuki/abi.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
int ciuki_devices(struct ciuki_devices_info *s) { (void)s;errno=ENODEV;return -1; }
int main(int argc,char **argv)
{
    assert(argc==2);
    int32_t packed;unsigned left,right,mute;
    char cfg[512],legacy[512];snprintf(cfg,sizeof(cfg),"%s/scheme.cfg",argv[1]);
    snprintf(legacy,sizeof(legacy),"%s/legacy.cfg",argv[1]);
    FILE *f=fopen(cfg,"wb");assert(f);fputs("enabled=1\nui_volume=80\nerror=custom.wav\n",f);fclose(f);
    struct sound_scheme s,loaded;assert(!sound_load_at(&s,cfg,NULL,argv[1]));
    assert(s.volume==80 && !strcmp(s.file[SOUND_ERROR],"custom.wav"));
    char rootfile[512];snprintf(rootfile,sizeof(rootfile),"%s/Desktop.reg",argv[1]);assert(access(rootfile,F_OK));
    f=fopen(legacy,"wb");assert(f);fputs("enabled=0\nquestion=\n",f);fclose(f);
    assert(!sound_load_at(&s,cfg,legacy,argv[1]));assert(!s.enabled && !s.file[SOUND_QUESTION][0]);
    struct reg_root *r=reg_open_root_at("Desktop",argv[1]);assert(r);
    assert(!reg_set_string(r,"Desktop\\Other","Keep","unchanged"));uint64_t generation=reg_generation(r);
    s.enabled=1;s.volume=35;assert(!sound_save_at(&s,argv[1]));assert(reg_generation(r)==generation+1);
    char value[64];size_t size=sizeof(value);assert(!reg_get_string(r,"Desktop\\Other","Keep",value,&size));
    assert(!strcmp(value,"unchanged"));
    assert(!sound_load_at(&loaded,cfg,legacy,argv[1]));assert(!memcmp(&s,&loaded,sizeof(s)));
    assert(!reg_set_string(r,"Desktop\\Sounds\\error","File","../escape.wav"));
    assert(!reg_set_int32(r,"Desktop\\Sounds","Volume",101));
    assert(!sound_load_at(&loaded,cfg,legacy,argv[1]));assert(loaded.volume==80 && !strcmp(loaded.file[SOUND_ERROR],"custom.wav"));
    assert(!reg_set_string(r,"Desktop\\Sounds\\error","File","custom.wav\nenabled=0"));
    assert(!sound_load_at(&loaded,cfg,legacy,argv[1]));assert(loaded.enabled && !strcmp(loaded.file[SOUND_ERROR],"custom.wav"));
    strcpy(s.file[SOUND_ERROR],"../escape.wav");generation=reg_generation(r);
    assert(sound_save_at(&s,argv[1])==-1 && errno==EINVAL && reg_generation(r)==generation);
    reg_close_root(r);
    /* Almost the full value-count and text budgets, preserving unrelated
     * values across repeated transactions in the bounded master workspace. */
    char dense[512]; snprintf(dense,sizeof(dense),"%s/dense",argv[1]); assert(!mkdir(dense,0755));
    char dense_file[600]; snprintf(dense_file,sizeof(dense_file),"%s/System.reg",dense);
    f=fopen(dense_file,"wb"); assert(f); fputs("; ciuki-registry generation=7\n[System\\Dense]\n",f);
    for (unsigned i=0;i<REG_VALUES_MAX-1;i++) fprintf(f,"\"V%04u_%040u\" = %u\n",i,0u,i);
    assert(!fclose(f));
    for (unsigned i=0;i<3;i++) {
        int result=master_preference_save(dense,20+i,70-i,i%2);
        if (result) perror("dense master save");
        assert(!result);
    }
    r=reg_open_root_at("System",dense); assert(r && reg_generation(r)==10);
    assert(!reg_get_int32(r,CIUKI_MASTER_KEY,CIUKI_MASTER_VALUE,&packed) && packed==ciuki_master_pack(22,68,0));
    assert(!reg_get_int32(r,"System\\Dense","V4094_0000000000000000000000000000000000000000",&packed) && packed==4094);
    reg_close_root(r);
    r=reg_open_root_at("System",argv[1]);assert(r);
    assert(!reg_set_string(r,"System\\Identity","Name","CiukiOS"));generation=reg_generation(r);
    assert(!master_preference_save(argv[1],20,20,0)&&reg_generation(r)==generation+1);
    assert(!reg_get_int32(r,CIUKI_MASTER_KEY,CIUKI_MASTER_VALUE,&packed));
    assert(ciuki_master_unpack(packed,&left,&right,&mute)&&left==20&&right==20&&!mute);
    assert(!master_preference_save(argv[1],25,75,1));
    assert(!reg_get_int32(r,CIUKI_MASTER_KEY,CIUKI_MASTER_VALUE,&packed));
    assert(ciuki_master_unpack(packed,&left,&right,&mute)&&left==25&&right==75&&mute);
    size=sizeof(value);assert(!reg_get_string(r,"System\\Identity","Name",value,&size)&&!strcmp(value,"CiukiOS"));
    generation=reg_generation(r);assert(master_preference_save(argv[1],101,20,0)==-1&&errno==EINVAL&&reg_generation(r)==generation);
    assert(master_preference_save(argv[1],20,20,2)==-1&&errno==EINVAL);
    assert(!ciuki_master_unpack(-1,&left,&right,&mute)&&!ciuki_master_unpack(131072,&left,&right,&mute));
    struct ciuki_mixer_line line={.version=1,.size=sizeof(line),.line=CIUKI_MIXER_CD,.left=40,.right=65,.mute=1};
    assert(!line_preference_save(argv[1],&line));
    assert(!reg_get_int32(r,CIUKI_MASTER_KEY,"Source3",&packed) && packed==ciuki_master_pack(40,65,1));
    line.line=CIUKI_MIXER_LINE; line.recording=1; line.selected=1;
    assert(!line_preference_save(argv[1],&line));
    assert(!reg_get_int32(r,CIUKI_MASTER_KEY,"Recording",&packed) && packed==(ciuki_master_pack(40,65,1)|(4<<24)));
    line.left=101; generation=reg_generation(r); assert(line_preference_save(argv[1],&line)==-1 && errno==EINVAL && reg_generation(r)==generation);
    reg_close_root(r);
    char schemes[256]; snprintf(schemes,sizeof(schemes),"%s/schemes",argv[1]);
    char names[SOUND_SCHEMES_MAX][SOUND_SCHEME_NAME];
    assert(sound_scheme_list_at(schemes,names,SOUND_SCHEMES_MAX)==0);
    sound_defaults(&s); assert(!sound_assign(&s,SOUND_ERROR,"/Documents/My Sound.WAV"));
    assert(!sound_path(&s,SOUND_ERROR,cfg,sizeof(cfg)) && !strcmp(cfg,"/Documents/My Sound.WAV"));
    assert(!sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"Work",&s));
    assert(!sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"Arcade",&s));
    assert(sound_scheme_list_at(schemes,names,SOUND_SCHEMES_MAX)==2 && !strcmp(names[0],"Arcade") && !strcmp(names[1],"Work"));
    assert(sound_scheme_list_at(schemes,names,1)==-ENOSPC);
    sound_defaults(&loaded); assert(!sound_scheme_at(schemes,SOUND_SCHEME_LOAD,"Work",&loaded));
    assert(!memcmp(&s,&loaded,sizeof(s)));
    s.volume=42; assert(!sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"Work",&s));
    assert(!sound_scheme_at(schemes,SOUND_SCHEME_LOAD,"Work",&loaded) && loaded.volume==42);
    assert(sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"Default",&s)==-EINVAL);
    assert(sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"default",&s)==-EINVAL);
    assert(sound_scheme_at(schemes,SOUND_SCHEME_DELETE,"../escape",&s)==-EINVAL);
    assert(!sound_scheme_at(schemes,SOUND_SCHEME_DELETE,"Work",&s));
    assert(sound_scheme_at(schemes,SOUND_SCHEME_LOAD,"Work",&loaded)==-ENOENT);
    assert(!sound_scheme_at(schemes,SOUND_SCHEME_DELETE,"Arcade",&s));
    for (unsigned i=0;i<SOUND_SCHEMES_MAX;i++) {
        char name[32]; snprintf(name,sizeof(name),"Scheme%02u",i);
        assert(!sound_scheme_at(schemes,SOUND_SCHEME_SAVE,name,&s));
    }
    assert(sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"Overflow",&s)==-ENOSPC);
    assert(!sound_scheme_at(schemes,SOUND_SCHEME_SAVE,"Scheme00",&s)); /* overwrite at capacity */
    for (unsigned i=0;i<SOUND_SCHEMES_MAX;i++) {
        char name[32]; snprintf(name,sizeof(name),"Scheme%02u",i);
        assert(!sound_scheme_at(schemes,SOUND_SCHEME_DELETE,name,&s));
    }
    assert(!rmdir(schemes));
    char cwd[1024]; assert(getcwd(cwd,sizeof(cwd))); assert(!chdir(argv[1]));
    uint8_t wav[46]={0}; memcpy(wav,"RIFF",4); wav[4]=38; memcpy(wav+8,"WAVEfmt ",8);
    wav[16]=16; wav[20]=1; wav[22]=1; wav[24]=0x22; wav[25]=0x56;
    wav[28]=0x44; wav[29]=0xac; wav[32]=2; wav[34]=16; memcpy(wav+36,"data",4); wav[40]=2;
    f=fopen("sample.wav","wb"); assert(f && fwrite(wav,1,sizeof(wav),f)==sizeof(wav)); fclose(f);
    assert(!sound_validate_file("sample.wav"));
    wav[20]=3; f=fopen("sample.wav","wb"); assert(f && fwrite(wav,1,sizeof(wav),f)==sizeof(wav)); fclose(f);
    assert(sound_validate_file("sample.wav")==-EINVAL);
    assert(!unlink("sample.wav")); assert(!chdir(cwd));
    puts("PASS sound storage: shipped/legacy/registry precedence, no writes on load, one durable merge, unrelated keys, invalid paths/volume");
}
