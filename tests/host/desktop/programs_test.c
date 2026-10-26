/* Production inventory/removal against a fixture registry, manifest and tree. */
#include "../../../apps/control/programs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
static struct cp_program_store store;
static struct desk_uninstall_entry entries[96];
static unsigned games=31,registered=1,keep_key,runs,logs,icons,save_failure,saves,key_checks;
static int check_key(void *ctx,const char *key){(void)ctx;assert(!strcmp(key,"HKCU/fixture"));key_checks++;return registered;}
static int persist(void *ctx){(void)ctx;saves++;return save_failure ? -EIO : 0;}
static char logged[256];
static int registry(void *ctx,struct desk_uninstall_entry *out,unsigned cap)
{
    (void)ctx;assert(cap>=2);
    if(!registered)return 0;
    out[0]=(struct desk_uninstall_entry){.name="Zulu Win32",.key="HKCU/fixture",.command="C:\\Programs\\Win\\remove.exe",.version="2.4",.publisher="Fixture",.estimated_kb=5};
    return 1;
}
static int run(void *ctx,const char *key)
{(void)ctx;assert(!strcmp(key,"HKCU/fixture"));runs++;if(!keep_key)registered=0;return 0;}
static void file(const char *,const void *,size_t);
static int exists(const char *);
static int game(void *ctx,unsigned id,int remove,uint32_t *bytes)
{
    (void)ctx;assert(id && id<=5);*bytes=8192;
    char pending[64];snprintf(pending,sizeof(pending),"/System/REMOVE%u.PND",id);
    if(remove){games&=~(1u<<(id-1));file(pending,"",0);return 1;}
    return exists(pending) ? 2 : !!(games&(1u<<(id-1)));
}
static void log_line(void *ctx,const char *text)
{(void)ctx;logs++;snprintf(logged,sizeof(logged),"%s",text);}
static void forget(void *ctx,const char *name)
{(void)ctx;assert(*name);icons++;}
static void file(const char *rel,const void *bytes,size_t count)
{char path[1024];snprintf(path,sizeof(path),"%s%s",store.root,rel);FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,count,f)==count);assert(!fclose(f));}
static void link_file(const char *rel,const char *name,const char *target,const char *args)
{
    struct desk_launch_record r={.show=1};strcpy(r.label,name);strcpy(r.target,target);strcpy(r.arguments,args);
    uint8_t b[4096];unsigned n=desk_link_encode(&r,b,sizeof(b));assert(n);file(rel,b,n);
}
static unsigned find(const char *name)
{int n=cp_program_list(&store,entries,96);assert(n>0);for(int i=0;i<n;i++)if(!strcmp(entries[i].name,name))return (unsigned)i;fprintf(stderr,"missing fixture entry: %s\n",name);assert(0);return 0;}
static int exists(const char *rel)
{char p[1024];snprintf(p,sizeof(p),"%s%s",store.root,rel);return !access(p,F_OK);}
int main(int argc,char **argv)
{
    assert(argc==2);store=(struct cp_program_store){.root=argv[1],.registry_list=registry,.registry_run=run,.registry_check=check_key,.game=game,.log=log_line,.forget_icon=forget,.persist=persist};
    /* External-only launch record: no internal record may mask discovery. */
    link_file("/Games/External.ciuki","External DOS launch","C:\\Games\\Test\\GAME.EXE","");
    link_file("/Windows/Start Menu/Programs/DosUser.lnk","User DOS","C:\\Programs\\DosUser\\RUN.BAT","");
    link_file("/Windows/Desktop/DOS.lnk","DOS shortcut","/Games/Test/GAME.EXE","");
    link_file("/Windows/Desktop/Other.lnk","Other","/Games/Tester/GAME.EXE","");
    link_file("/Windows/Start Menu/Programs/Win.lnk","Win","C:\\Programs\\Win\\APP.EXE","");
    link_file("/Windows/Desktop/Quake.lnk","Quake shortcut","/Programs/dosbox","--name=Quake \\QUAKE\\QUAKE.EXE");
    link_file("/Windows/Desktop/Native.lnk","Native shortcut","/Programs/UserNative","");
    link_file("/Documents/Desktop/Native.lnk","Native shortcut","/Programs/UserNative","");
    file("/Programs/UserNative","\177ELF",4);file("/Programs/control","\177ELF",4);file("/Programs/hwreport","\177ELF",4);
    struct desk_uninstall_entry limited[4];
    assert(cp_program_list(&store,limited,4)==4 && limited[0].inventory_more);
    for(unsigned j=0;j<4;j++)assert(limited[j].system_component);
    unsigned i=find("External DOS launch");assert(entries[i].kind==DESK_PROGRAM_DOS && entries[i].disk_bytes);
    int n=cp_program_list(&store,entries,96);assert(n>=10);
    for(int j=1;j<n;j++)assert(strcasecmp(entries[j-1].name,entries[j].name)<=0);
    i=find("hwreport");assert(entries[i].system_component && cp_program_remove(&store,&entries[i])==-EPERM && exists("/Programs/hwreport"));
    i=find("control");assert(entries[i].system_component && cp_program_remove(&store,&entries[i])==-EPERM && exists("/Programs/control"));
    i=find("DOOM (shareware)");assert(!strcmp(entries[i].version,"1.9") && !strcmp(entries[i].publisher,"id Software") && strstr(entries[i].licence,"/system/licenses/"));
    i=find("Zulu Win32");assert(!strcmp(entries[i].location,"/Programs/Win"));keep_key=1;assert(cp_program_remove(&store,&entries[i])==-EAGAIN && exists("/Windows/Start Menu/Programs/Win.lnk"));
    keep_key=0;assert(!cp_program_remove(&store,&entries[i]) && runs==2 && key_checks==2 && !exists("/Windows/Start Menu/Programs/Win.lnk"));
    i=find("External DOS launch");assert(!cp_program_remove(&store,&entries[i]));
    assert(!exists("/Games/Test") && !exists("/Windows/Desktop/DOS.lnk") && exists("/Windows/Desktop/Other.lnk"));
    assert(!exists("/Games/External.ciuki"));
    i=find("User DOS");assert(entries[i].kind==DESK_PROGRAM_DOS && !cp_program_remove(&store,&entries[i]) && !exists("/Programs/DosUser"));
    i=find("Native shortcut");assert(!cp_program_remove(&store,&entries[i]) && !exists("/Programs/UserNative") && !exists("/Documents/Desktop/Native.lnk"));
    i=find("UserDirectory");assert(!cp_program_remove(&store,&entries[i]) && !exists("/Programs/UserDirectory"));
    link_file("/Windows/Desktop/Doom.lnk","Doom shortcut","C:\\Programs\\dosbox","--name=Doom \\doom\\doom.exe");
    link_file("/Windows/Desktop/Quake2.lnk","Quake alternate","c:\\programs\\dosbox","\\quake\\quake.exe -optional");
    i=find("DOOM (shareware)");assert(!cp_program_remove(&store,&entries[i]) && !exists("/Windows/Desktop/Doom.lnk"));
    char blocked[1024];snprintf(blocked,sizeof(blocked),"%s/system/settings/dos-game-3.cfg",store.root);
    assert(!unlink(blocked) && !mkdir(blocked,0700));
    i=find("Quake (shareware)");assert(cp_program_remove(&store,&entries[i])==-EISDIR);
    assert(exists("/System/REMOVE3.PND") && !(games&4));
    assert(!rmdir(blocked));i=find("Quake (shareware)");
    save_failure=1;assert(cp_program_remove(&store,&entries[i])==-EIO);
    assert(exists("/System/REMOVE3.PND") && exists("/Windows/Desktop/Quake.lnk") && saves);
    save_failure=0;i=find("Quake (shareware)");
    assert(!cp_program_remove(&store,&entries[i]) && !(games&4));
    assert(!exists("/System/REMOVE3.PND") && !exists("/system/settings/dos-game-3.cfg") && !exists("/Windows/Desktop/Quake.lnk") && icons>=7);
    assert(strstr(logged,"[control] remove name=Quake (shareware) kind=bundled-dos result=0") && logs==12);
    assert(exists("/system/licenses/quake-shareware-UPSTREAM.json"));
    link_file("/Windows/Desktop/Duke.lnk","Duke shortcut","/Programs/dosbox","--name=Duke Nukem 3D \\DUKE3D\\DUKE3D.BAT");
    i=find("Duke Nukem 3D (shareware)");assert(entries[i].kind==DESK_PROGRAM_GAME && entries[i].game==5 && !strcmp(entries[i].location,"/DUKE3D") &&
        !strcmp(entries[i].version,"1.3d") && strstr(entries[i].publisher,"3D Realms") &&
        !strcmp(entries[i].licence,"/system/licenses/duke3d-shareware-LICENSE.TXT"));
    assert(!cp_program_remove(&store,&entries[i]) && !(games&16) && !exists("/System/REMOVE5.PND") && !exists("/Windows/Desktop/Duke.lnk"));
    assert(exists("/system/licenses/duke3d-shareware-UPSTREAM.json") && (games&1));
    /* Owner 2026-10-25: the Warcraft II demo is the fourth bundled DOS game (/WAR2, package war2-demo). */
    link_file("/Windows/Desktop/Warcraft.lnk","Warcraft shortcut","/Programs/dosbox","--name=Warcraft II \\WAR2\\WAR2.BAT");
    i=find("Warcraft II (demo)");assert(entries[i].kind==DESK_PROGRAM_GAME && entries[i].game==4 && !strcmp(entries[i].location,"/WAR2") &&
        !strcmp(entries[i].publisher,"Blizzard Entertainment") && !strcmp(entries[i].version,"1.20elc") &&
        !strcmp(entries[i].licence,"/system/licenses/war2-demo-VENDOR.TXT"));
    assert(!cp_program_remove(&store,&entries[i]) && !(games&8) && !exists("/System/REMOVE4.PND") && !exists("/Windows/Desktop/Warcraft.lnk"));
    assert(exists("/system/licenses/war2-demo-UPSTREAM.json") && (games&1));
    struct desk_uninstall_entry bad={.name="bad",.kind=DESK_PROGRAM_NATIVE,.location="/Programs/../System"};
    assert(cp_program_remove(&store,&bad)==-EPERM);
    strcpy(bad.location,"/Programs");assert(cp_program_remove(&store,&bad)==-EPERM);
    char dangerous[1024],outside[1024];
    snprintf(dangerous,sizeof(dangerous),"%s/Programs/SymlinkTest",store.root);assert(!mkdir(dangerous,0700));
    snprintf(dangerous,sizeof(dangerous),"%s/Programs/SymlinkTest/link",store.root);
    snprintf(outside,sizeof(outside),"%s/Programs/control",store.root);assert(!symlink(outside,dangerous));
    strcpy(bad.location,"/Programs/SymlinkTest");assert(cp_program_remove(&store,&bad)==-EINVAL && exists("/Programs/control"));assert(!unlink(dangerous));
    assert(cp_program_contains("/Games/Test","C:\\Games\\Test\\A.EXE"));
    assert(!cp_program_contains("/Games/Test","/Games/Tester/A.EXE") && !cp_program_contains("/Games/Test","/Games/Test/../Other"));
    assert(cp_program_installer("/mnt/d/SETUP.EXE") && cp_program_installer("/mnt/e/install.exe") && cp_program_installer("/mnt/d/RUN.BAT") && !cp_program_installer("/mnt/d/game.exe"));
    puts("PASS Add/Remove Programs: sorted unified inventory, provenance, protected components, all removal kinds, stale registry key, shortcut boundaries, settings, licences, log");
    fflush(stdout);return 0;
}
