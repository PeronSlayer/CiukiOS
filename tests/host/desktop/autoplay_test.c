/* Production desktop AutoPlay, fake mounted media, real libreg persistence.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#define fake_reset fake_tree_reset
#include "fake_vfs.h"
#undef fake_reset
#include "../../../apps/libdeskui/deskui.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/registry.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/devices.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int ciuki_devices(struct ciuki_devices_info *out) { (void)out; errno=ENODEV; return -1; }
static struct desktop d;
static struct ciuki_media_volume volumes[26];
static const char *registry_dir;
static unsigned launches, connected, disconnected;
static int fail_launch;
static char program[64], argument[FM_PATH];
static int launch(struct desktop *desk,const struct desk_program *p)
{
    launches++; snprintf(program,sizeof(program),"%s",p->name);
    snprintf(argument,sizeof(argument),"%s",desk->launch_arg ? desk->launch_arg : desk->launch_argv[0] ? desk->launch_argv[0] : "");
    if (fail_launch) { errno=ENOENT; return -1; }
    return 100;
}
static int preference(struct desktop *desk,const char *id,const char *label,int action)
{ (void)desk; return action<0 ? ap_preference_get(registry_dir,id) : ap_preference_set(registry_dir,id,label,action); }
static void sound(struct desktop *desk,int present) { (void)desk; if (present) connected++; else disconnected++; }
static void mounted(unsigned drive,unsigned generation,const char *id,const char *label,unsigned flags)
{
    volumes[drive]=(struct ciuki_media_volume){.version=1,.size=96,.drive=drive,.flags=flags,.generation=generation};
    snprintf(volumes[drive].id,40,"%s",id); snprintf(volumes[drive].label,33,"%s",label);
    desk_autoplay_update(&d,volumes); desk_autoplay_service(&d);
}
static void eject(unsigned drive)
{ memset(&volumes[drive],0,sizeof(volumes[drive])); desk_autoplay_update(&d,volumes); desk_autoplay_service(&d); }
static void file(const char *path,const char *text)
{
    int i=fake_lookup(path); if (i<0) i=fake_file(path,(unsigned)strlen(text),0); assert(i>=0);
    fake_nodes[i].meta.size=strlen(text);
    memcpy(fake_nodes[i].data,text,strlen(text));
}
static int window(void)
{ for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal==DESK_INTERNAL_AUTOPLAY) return i; return -1; }
static void key(int k) { assert(desk_shell_key(&d,k,1)); desk_shell_key(&d,k,0); }
static void select_action(int action)
{
    int w=window(); assert(w>=0); struct desk_box content=desk_content(&d.clients[w]);
    assert(desk_internal_click(&d,w,content.x+30,content.y+58+action*24+5));
    assert(d.sh.autoplay.selected==action);
}
static void render(void)
{
    int w=window(); assert(w>=0); struct desk_box full={0,0,640,480};
    desk_internal_paint(&d,w,desk_content(&d.clients[w]),full);
}
int main(int argc,char **argv)
{
    assert(argc==2); registry_dir=argv[1];
    fake_reset(); fake_tree_reset(); desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    d.pixels=calloc(640*480,4); assert(d.pixels);
    d.autoplay_fs=&fake_vfs; d.autoplay_preference=preference; d.media_sound=sound; d.launch=launch;
    const char *programs[]={"files","music","viewer","dosbox"};
    for (unsigned i=0;i<4;i++) { snprintf(d.programs[i].name,64,"%s",programs[i]); snprintf(d.programs[i].path,DESK_ARG_MAX+1,"/Programs/%s",programs[i]); }
    d.program_count=4;
    assert(fake_create("/mnt",FM_DIR)>=0);
    assert(fake_create("/mnt/e",FM_DIR)>=0); assert(fake_create("/mnt/e/Music",FM_DIR)>=0);
    file("/mnt/e/Music/tune.MP3","audio");
    mounted(4,10,"fat-12345678-1234","MIXTAPE",CIUKI_MEDIA_REMOVABLE);
    assert(d.sh.autoplay.open && !strcmp(d.sh.autoplay.content.music,"/mnt/e/Music"));
    assert(desk_autoplay_enabled(&d,AP_MUSIC) && !desk_autoplay_enabled(&d,AP_PICTURES)); render();
    desk_autoplay_update(&d,volumes); desk_autoplay_service(&d); assert(connected==1 && !launches);
    select_action(AP_MUSIC); key(DU_KEY_SPACE); key(DU_KEY_ENTER); desk_autoplay_service(&d);
    assert(!d.sh.autoplay.open && launches==1 && !strcmp(program,"music") && !strcmp(argument,"/mnt/e/Music"));
    assert(ap_preference_get(registry_dir,"fat-12345678-1234")==AP_MUSIC);
    eject(4); mounted(4,11,"fat-12345678-1234","MIXTAPE",CIUKI_MEDIA_REMOVABLE);
    assert(!d.sh.autoplay.open && launches==2); eject(4);
    /* Same volume gets a new letter; preferences use identity, not letter/generation. */
    assert(fake_create("/mnt/f",FM_DIR)>=0); file("/mnt/f/song.wav","audio");
    mounted(5,12,"fat-12345678-1234","MIXTAPE",CIUKI_MEDIA_REMOVABLE);
    assert(!d.sh.autoplay.open && launches==3 && !strcmp(argument,"/mnt/f")); eject(5);
    assert(fake_create("/mnt/d",FM_DIR)>=0); assert(fake_create("/mnt/d/SETUP",FM_DIR)>=0);
    file("/mnt/d/AUTORUN.INF","; CD\r\n[AuToRuN]\r\nlabel=Ciuki Games\r\nicon=disc.ico\r\nopen=SETUP\\INSTALL.COM /S\r\n[other]\r\nopen=bad.exe\r\n");
    file("/mnt/d/SETUP/INSTALL.COM","dos");
    mounted(3,20,"iso-1234567890123456","GAMES",CIUKI_MEDIA_OPTICAL);
    assert(d.sh.autoplay.open && d.sh.autoplay.content.run_kind==1);
    struct ap_inf quoted;
    const char *quoted_text="[autorun]\nopen=\"SETUP\\INSTALL.COM\" /S\nlabel=\"Ciuki Games\"\n";
    assert(!ap_parse(&quoted,quoted_text,strlen(quoted_text)) && !strcmp(quoted.open,"\"SETUP\\INSTALL.COM\" /S") && !strcmp(quoted.label,"Ciuki Games"));
    assert(!strcmp(d.sh.autoplay.content.inf.label,"Ciuki Games") && !strcmp(d.sh.autoplay.content.inf.icon,"disc.ico"));
    char label[33]; assert(!ap_drive_label(&fake_vfs,3,label) && !strcmp(label,"Ciuki Games")); render();
    select_action(AP_RUN); key(DU_KEY_ENTER); desk_autoplay_service(&d);
    assert(launches==4 && !strcmp(program,"dosbox") && !strcmp(argument,"D:\\SETUP\\INSTALL.COM /S")); eject(3);
    assert(fake_create("/mnt/g",FM_DIR)>=0); assert(fake_create("/mnt/g/Pictures",FM_DIR)>=0);
    file("/mnt/g/Pictures/ciuki.PNG","picture");
    mounted(6,30,"fat-87654321-1234","PHOTOS",CIUKI_MEDIA_REMOVABLE);
    assert(desk_autoplay_enabled(&d,AP_PICTURES)); select_action(AP_PICTURES); key(DU_KEY_ENTER); desk_autoplay_service(&d);
    assert(launches==5 && !strcmp(program,"viewer") && !strcmp(argument,"/mnt/g/Pictures/ciuki.PNG")); eject(6);
    /* Eject cancels a pending confirmation without launching or saving it. */
    mounted(6,31,"fat-87654321-1234","PHOTOS",CIUKI_MEDIA_REMOVABLE);
    key(DU_KEY_SPACE); key(DU_KEY_ENTER); eject(6);
    assert(window()<0 && !d.sh.autoplay.open && launches==5 && ap_preference_get(registry_dir,"fat-87654321-1234")==-1);
    /* Replacement media on the same letter dismisses the previous dialog. */
    mounted(6,32,"fat-87654321-1234","PHOTOS",CIUKI_MEDIA_REMOVABLE);
    mounted(6,33,"fat-87654322-1234","NEW PHOTOS",CIUKI_MEDIA_REMOVABLE);
    assert(d.sh.autoplay.active.generation==33 && d.sh.autoplay.open); key(DU_KEY_ESC); assert(!d.sh.autoplay.open); eject(6);
    /* PE Windows entry is visible, disabled, and cannot be remembered or launched. */
    int exe=fake_file("/mnt/d/SETUP/WIN.EXE",128,0); assert(exe>=0); memset(fake_nodes[exe].data,0,128);
    memcpy(fake_nodes[exe].data,"MZ",2); fake_nodes[exe].data[60]=64; memcpy(fake_nodes[exe].data+64,"PE\0\0",4);
    file("/mnt/d/AUTORUN.INF","[autorun]\nopen=SETUP\\WIN.EXE\n");
    mounted(3,21,"iso-2345678901234567","WINDOWS",CIUKI_MEDIA_OPTICAL);
    assert(d.sh.autoplay.content.run_kind==2 && !desk_autoplay_enabled(&d,AP_RUN)); render();
    char run[100]; assert(strstr(desk_autoplay_label(&d,AP_RUN,run,sizeof(run)),"F5"));
    int w=window(); struct desk_box content=desk_content(&d.clients[w]);
    desk_internal_click(&d,w,content.x+30,content.y+58+AP_RUN*24+5); assert(d.sh.autoplay.selected==AP_FOLDER);
    select_action(AP_NOTHING); key(DU_KEY_SPACE); key(DU_KEY_ENTER); desk_autoplay_service(&d);
    assert(launches==5 && ap_preference_get(registry_dir,"iso-2345678901234567")==AP_NOTHING); eject(3);
    mounted(3,22,"iso-2345678901234567","WINDOWS",CIUKI_MEDIA_OPTICAL); assert(!d.sh.autoplay.open && launches==5); eject(3);
    /* Unavailable remembered action falls back to asking. Failed launches never save. */
    assert(!ap_preference_set(registry_dir,"fat-00000001-1234","Empty stick",AP_MUSIC));
    mounted(3,23,"fat-00000001-1234","EMPTY",CIUKI_MEDIA_REMOVABLE); assert(d.sh.autoplay.open);
    fail_launch=1; key(DU_KEY_SPACE); key(DU_KEY_ENTER); desk_autoplay_service(&d); fail_launch=0;
    assert(ap_preference_get(registry_dir,"fat-00000001-1234")==AP_MUSIC); eject(3);
    /* Simultaneous arrivals are queued, and an eject removes a queued item. */
    volumes[4]=(struct ciuki_media_volume){.drive=4,.flags=1,.generation=40};
    volumes[6]=(struct ciuki_media_volume){.drive=6,.flags=1,.generation=40};
    desk_autoplay_update(&d,volumes); desk_autoplay_service(&d); assert(d.sh.autoplay.active.drive==4);
    eject(6); key(DU_KEY_ESC); desk_autoplay_service(&d); assert(!d.sh.autoplay.open); eject(4);
    struct ap_inf inf;
    const char *invalid="[autorun]\nopen=../escape.com\n";
    file("/mnt/d/AUTORUN.INF",invalid); struct ap_content probe;
    assert(!ap_probe(&fake_vfs,3,&probe) && probe.run_kind==3 && !probe.command[0]);
    int tiny=fake_file("/mnt/d/SETUP/TINY.EXE",32,0); memset(fake_nodes[tiny].data,0,32); memcpy(fake_nodes[tiny].data,"MZ",2);
    file("/mnt/d/AUTORUN.INF","[autorun]\nopen=SETUP\\TINY.EXE\n");
    assert(!ap_probe(&fake_vfs,3,&probe) && probe.run_kind==1);
    file("/mnt/d/SETUP/WITH SPACE.COM","dos");
    file("/mnt/d/AUTORUN.INF","[autorun]\nopen=\"SETUP\\WITH SPACE.COM\" /S\n");
    assert(!ap_probe(&fake_vfs,3,&probe) && probe.run_kind==3 && !probe.command[0]);
    fake_countdown=1; mounted(3,24,"fat-00000002-1234","UNREADABLE",CIUKI_MEDIA_REMOVABLE);
    assert(d.sh.autoplay.open && desk_autoplay_enabled(&d,AP_FOLDER)); key(DU_KEY_ESC); eject(3);
    assert(ap_parse(&inf,"[autorun]\nopen=ok\0x",20)<0);
    char big[AP_INF_MAX+1]; memset(big,'x',sizeof(big)); assert(ap_parse(&inf,big,sizeof(big))<0);
    struct ap_preference choices[AP_PREFS]; assert(ap_preference_list(registry_dir,choices,AP_PREFS)==3);
    assert(!ap_preference_forget(registry_dir,"fat-12345678-1234")); assert(ap_preference_get(registry_dir,"fat-12345678-1234")==-1);
    assert(ap_preference_set(registry_dir,"bad\\id","Bad",0)<0);
    assert(connected>5 && disconnected>5);
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal) desk_drop_client(&d,i);
    free(d.pixels); d.pixels=NULL; desk_render_release(&d); assert(!fake_live()); fake_assert_clean();
    puts("PASS AutoPlay: mount music/CD/pictures, typed DOS launch, disabled Windows, registry across letters, cancellation/eject/replacement/queue, sounds seam, bounded parser");
    return 0;
}
