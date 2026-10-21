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
static unsigned games=7,registered=1,keep_key,runs,logs,icons;
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
static int game(void *ctx,unsigned id,int remove,uint32_t *bytes)
{(void)ctx;assert(id && id<=3);*bytes=8192;if(remove)games&=~(1u<<(id-1));return !!(games&(1u<<(id-1)));}
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
    assert(argc==2);store=(struct cp_program_store){.root=argv[1],.registry_list=registry,.registry_run=run,.game=game,.log=log_line,.forget_icon=forget};
    link_file("/Games/Test/.ciuki","Alpha DOS","C:\\Games\\Test\\GAME.EXE","");
    link_file("/Games/External.ciuki","External DOS launch","C:\\Games\\Test\\GAME.EXE","");
    link_file("/Windows/Start Menu/Programs/DosUser.lnk","User DOS","C:\\Programs\\DosUser\\RUN.BAT","");
    link_file("/Windows/Desktop/DOS.lnk","DOS shortcut","/Games/Test/GAME.EXE","");
    link_file("/Windows/Desktop/Other.lnk","Other","/Games/Tester/GAME.EXE","");
    link_file("/Windows/Start Menu/Programs/Win.lnk","Win","C:\\Programs\\Win\\APP.EXE","");
    link_file("/Windows/Desktop/Quake.lnk","Quake shortcut","/Programs/dosbox","--name=Quake \\QUAKE\\QUAKE.EXE");
    link_file("/Windows/Desktop/Native.lnk","Native shortcut","/Programs/UserNative","");
    link_file("/Documents/Desktop/Native.lnk","Native shortcut","/Programs/UserNative","");
    file("/Programs/UserNative","\177ELF",4);file("/Programs/control","\177ELF",4);
    unsigned i=find("Alpha DOS");assert(entries[i].kind==DESK_PROGRAM_DOS && entries[i].disk_bytes);
    int n=cp_program_list(&store,entries,96);assert(n>=10);
    for(int j=1;j<n;j++)assert(strcasecmp(entries[j-1].name,entries[j].name)<=0);
    i=find("control");assert(entries[i].system_component && cp_program_remove(&store,&entries[i])==-EPERM && exists("/Programs/control"));
    i=find("DOOM (shareware)");assert(!strcmp(entries[i].version,"1.9") && !strcmp(entries[i].publisher,"id Software") && strstr(entries[i].licence,"/system/licenses/"));
    i=find("Zulu Win32");assert(!strcmp(entries[i].location,"/Programs/Win"));keep_key=1;assert(cp_program_remove(&store,&entries[i])==-EAGAIN && exists("/Windows/Start Menu/Programs/Win.lnk"));
    keep_key=0;assert(!cp_program_remove(&store,&entries[i]) && runs==2 && !exists("/Windows/Start Menu/Programs/Win.lnk"));
    i=find("Alpha DOS");assert(!cp_program_remove(&store,&entries[i]));
    assert(!exists("/Games/Test") && !exists("/Windows/Desktop/DOS.lnk") && exists("/Windows/Desktop/Other.lnk"));
    assert(!exists("/Games/External.ciuki"));
    i=find("User DOS");assert(entries[i].kind==DESK_PROGRAM_DOS && !cp_program_remove(&store,&entries[i]) && !exists("/Programs/DosUser"));
    i=find("Native shortcut");assert(!cp_program_remove(&store,&entries[i]) && !exists("/Programs/UserNative") && !exists("/Documents/Desktop/Native.lnk"));
    i=find("UserDirectory");assert(!cp_program_remove(&store,&entries[i]) && !exists("/Programs/UserDirectory"));
    i=find("Quake (shareware)");assert(!cp_program_remove(&store,&entries[i]) && !(games&4));
    assert(!exists("/system/settings/dos-game-3.cfg") && !exists("/Windows/Desktop/Quake.lnk") && icons>=7);
    assert(strstr(logged,"[control] remove name=Quake (shareware) kind=bundled-dos result=0") && logs==8);
    assert(exists("/system/licenses/quake-shareware-UPSTREAM.json"));
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
