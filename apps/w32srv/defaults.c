/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "registry.h"
#include <stdio.h>
#include <string.h>
static struct wr_key *ensure(struct wr_server *s,unsigned root,const char *path)
{
    /* Persisted deletion of a default key/value is an application change.
     * Only an entirely new hive is seeded; HKCC is synthesized every boot. */
    if(root<2&&s->hive[root].sequence)return s->roots[root];
    struct wr_key *k=s->roots[root];char part[W32R_NAME];
    while(*path){const char *e=strchr(path,'\\');size_t n=e?(size_t)(e-path):strlen(path);if(n>=sizeof(part))return NULL;
        memcpy(part,path,n);part[n]=0;struct wr_key *c=wr_child(s,k,part);if(!c)c=wr_add_key(s,k,part,root);if(!c)return NULL;k=c;if(!e)break;path=e+1;}
    return k;
}
static int string(struct wr_server *s,struct wr_key *k,const char *name,const char *value,int replace)
{
    if(!k)return 0;
    if(k->hive<2&&s->hive[k->hive].sequence)return 1;
    for(struct wr_value *v=k->values;v;v=v->next)if(wr_equal(v->name,name)&&!replace)return 1;
    uint8_t utf16[1024];size_t n=strlen(value)+1;if(n*2>sizeof(utf16))return 0;
    for(size_t i=0;i<n;i++){utf16[i*2]=(uint8_t)value[i];utf16[i*2+1]=0;}
    return !wr_put(s,k,name,1,utf16,(uint32_t)n*2);
}
int wr_defaults(struct wr_server *s,unsigned width,unsigned height,unsigned bpp)
{
    const char *cv="Software\\Microsoft\\Windows\\CurrentVersion";
    static const char *const settings[][2]={
        {"ProgramFilesDir","C:\\Program Files"},{"CommonFilesDir","C:\\Program Files\\Common Files"},
        {"Version","Windows 98"},{"VersionNumber","4.10.2222"},{"SubVersionNumber"," A "},
        {"ProductName","Microsoft Windows 98"},{"RegisteredOwner","Ciuki"},{"RegisteredOrganization","CiukiOS"},
        {"MediaPath","C:\\WINDOWS\\MEDIA"},{"SystemRoot","C:\\WINDOWS"}};
    struct wr_key *current=ensure(s,0,cv);if(!current)return 0;
    for(unsigned i=0;i<sizeof(settings)/sizeof(*settings);i++)if(!string(s,current,settings[i][0],settings[i][1],0))return 0;
    if(!ensure(s,1,".DEFAULT"))return 0;
    static const char *const containers[]={"App Paths","Uninstall","Run","RunOnce","SharedDLLs","Fonts","Explorer\\Shell Folders","Explorer\\User Shell Folders"};
    for(unsigned root=0;root<2;root++)for(unsigned i=0;i<sizeof(containers)/sizeof(*containers);i++){
        char path[256];snprintf(path,sizeof(path),"%s%s\\%s",root?".DEFAULT\\":"",cv,containers[i]);if(!ensure(s,root,path))return 0;
    }
    static const char *const folders[][2]={{"Desktop","C:\\WINDOWS\\Desktop"},{"Programs","C:\\WINDOWS\\Start Menu\\Programs"},
        {"Start Menu","C:\\WINDOWS\\Start Menu"},{"Startup","C:\\WINDOWS\\Start Menu\\Programs\\StartUp"},
        {"Personal","C:\\Documents"},{"Fonts","C:\\WINDOWS\\FONTS"},{"AppData","C:\\WINDOWS\\Application Data"},
        {"Recent","C:\\WINDOWS\\Recent"},{"SendTo","C:\\WINDOWS\\SendTo"},
        {"Common Desktop","C:\\WINDOWS\\Desktop"},{"Common Programs","C:\\WINDOWS\\Start Menu\\Programs"}};
    for(unsigned root=0;root<2;root++)for(unsigned user=0;user<2;user++){
        char path[256];snprintf(path,sizeof(path),"%s%s\\Explorer\\%sShell Folders",root?".DEFAULT\\":"",cv,user?"User ":"");
        struct wr_key *k=ensure(s,root,path);for(unsigned i=0;i<sizeof(folders)/sizeof(*folders);i++)if(!string(s,k,folders[i][0],folders[i][1],0))return 0;
    }
    struct wr_key *dx=ensure(s,0,"Software\\Microsoft\\DirectX");
    if(!string(s,dx,"Version","4.07.00.0716",0))return 0;
    const uint8_t installed[]={0,0,0,0,7,0,0,0};
    int exists=0;for(struct wr_value *v=dx->values;v;v=v->next)if(wr_equal(v->name,"InstalledVersion"))exists=1;
    if(!s->hive[0].sequence&&!exists&&wr_put(s,dx,"InstalledVersion",3,installed,sizeof(installed)))return 0;
    static const char *const classes[][2]={
        {".lnk","lnkfile"},{".exe","exefile"},{".txt","txtfile"},
        {"txtfile","Text Document"},
        {".md","txtfile"},{".log","txtfile"},{".cfg","txtfile"},{".ini","txtfile"},{".conf","txtfile"},
        {".csv","txtfile"},{".json","txtfile"},{".c","txtfile"},{".h","txtfile"},{".asm","txtfile"},
        {".bmp","CiukiOS.Image"},{".png","CiukiOS.Image"},{".jpg","CiukiOS.Image"},{".jpeg","CiukiOS.Image"},
        {".jpe","CiukiOS.Image"},{".gif","CiukiOS.Image"},{".xrgb","CiukiOS.Image"},
        {"CiukiOS.Image","Image"},{"CiukiOS.Image\\shell\\open\\command","\"/Programs/viewer\" \"%1\""},
        {"CiukiOS.Image\\shell\\edit\\command","\"/Programs/paint\" \"%1\""},
        {".wav","CiukiOS.Audio"},{".mp3","CiukiOS.Audio"},{".ogg","CiukiOS.Audio"},{".m3u","CiukiOS.Audio"},
        {"CiukiOS.Audio","Audio"},{"CiukiOS.Audio\\shell\\open\\command","\"/Programs/music\" \"%1\""},
        {"exefile\\shell\\open\\command","\"%1\" %*"},{"txtfile\\shell\\open\\command","\"/Programs/ciuknote\" \"%1\""},
        {"CLSID\\{D7B70EE0-4340-11CF-B063-0020AFC2CD35}\\InprocServer32","ddraw.dll"},
        {"CLSID\\{47D4D946-62E8-11CF-93BC-444553540000}\\InprocServer32","dsound.dll"},
        {"CLSID\\{25E609E0-B259-11CF-BFC7-444553540000}\\InprocServer32","dinput.dll"},
        {"CLSID\\{00021401-0000-0000-C000-000000000046}\\InprocServer32","shell32.dll"}};
    for(unsigned i=0;i<sizeof(classes)/sizeof(*classes);i++){
        char path[512];snprintf(path,sizeof(path),"Software\\Classes\\%s",classes[i][0]);if(!string(s,ensure(s,0,path),"",classes[i][1],0))return 0;
    }
    if(!ensure(s,0,"Software\\Classes\\lnkfile")||!ensure(s,0,"Software\\CiukiOS\\Win32"))return 0;
    if(!string(s,ensure(s,0,"System\\CurrentControlSet\\Control\\MediaResources"),"Description","CiukiOS audio",0)||
       !string(s,ensure(s,0,"System\\CurrentControlSet\\Services\\Class\\Display"),"Description","CiukiOS display",0))return 0;
    struct wr_key *display=ensure(s,2,"Display\\Settings");char resolution[64];snprintf(resolution,sizeof(resolution),"%u,%u",width,height);
    uint8_t depth[4]={(uint8_t)bpp,0,0,0};
    if(!string(s,display,"Resolution",resolution,1)||wr_put(s,display,"BitsPerPixel",4,depth,4))return 0;
    static const char *const user[][3]={{"Desktop","Wallpaper",""},{"Desktop","ScreenSaveActive","0"},
        {"International","Locale","00000409"},{"International","sCountry","United States"},{"International","sLanguage","ENU"},
        {"International","sShortDate","M/d/yy"},{"International","sTimeFormat","h:mm:ss tt"},
        {"Mouse","MouseSensitivity","10"},{"Mouse","DoubleClickSpeed","500"},{"Mouse","SwapMouseButtons","0"}};
    for(unsigned i=0;i<sizeof(user)/sizeof(*user);i++){
        char path[256];snprintf(path,sizeof(path),".DEFAULT\\Control Panel\\%s",user[i][0]);if(!string(s,ensure(s,1,path),user[i][1],user[i][2],0))return 0;
    }
    return 1;
}
