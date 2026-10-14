/* SPDX-License-Identifier: MIT */
#include <unistd.h>
#include "sound_storage.h"
#include <ciuki/devices.h>
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
    reg_close_root(r);
    puts("PASS sound storage: shipped/legacy/registry precedence, no writes on load, one durable merge, unrelated keys, invalid paths/volume");
}
