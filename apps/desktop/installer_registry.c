/* Independent, read-only installer manifests through w32srv. SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
#include "installer_fixture.h"
#include "../../src/probes/win32_installers/controller.h"
#include <string.h>
static uint32_t request(unsigned op,uint32_t key,const char *path,const char *name,struct w32r_reply *reply,uint8_t *bytes,unsigned cap)
{
    uint8_t in[768]={0},out[1024];uint32_t used=0;unsigned p=(unsigned)strlen(path)+1,n=(unsigned)strlen(name)+1;
    if(sizeof(struct w32r_request)+p+n>sizeof(in))return 87;
    struct w32r_request r={.version=1,.op=op,.key=key,.access=0x20019,.path_bytes=p,.name_bytes=n};
    memcpy(in,&r,sizeof(r));memcpy(in+sizeof(r),path,p);memcpy(in+sizeof(r)+p,name,n);
    uint32_t e=desk_w32_exchange(in,sizeof(r)+p+n,out,sizeof(out),&used);
    if(e)return e;if(used<sizeof(*reply))return 13;memcpy(reply,out,sizeof(*reply));
    if(reply->version!=1 || reply->name_bytes || used!=sizeof(*reply)+reply->data_bytes || reply->data_bytes>cap)return 13;
    if(reply->data_bytes && bytes)memcpy(bytes,out+sizeof(*reply),reply->data_bytes);
    return reply->error;
}
static unsigned value(const char *path,const char *name,const char *expected)
{
    struct w32r_reply r;uint8_t bytes[768];uint32_t key=0,e=request(W32R_OPEN,W32R_HKLM,path,"",&r,bytes,sizeof(bytes));
    if(e==2 && !expected)return 0;if(e)return 1;key=r.key;
    e=request(W32R_QUERY,key,"",name,&r,bytes,sizeof(bytes));
    unsigned errors=expected ? e || r.type!=1 || r.data_bytes!=(strlen(expected)+1)*2 : e!=2;
    if(expected && !errors)for(unsigned i=0;i<r.data_bytes/2;i++)if(bytes[i*2]!=(uint8_t)expected[i] || bytes[i*2+1]){errors++;break;}
    errors+=request(W32R_CLOSE,key,"","",&r,bytes,sizeof(bytes))!=0;return errors;
}
unsigned desk_installer_registry(unsigned stage)
{
    const char *names[]={"DisplayName","UninstallString","DisplayVersion","Publisher","InstallLocation"};
    const char *values[]={"Ciuki Installer Fixture","\"" INSTALLER_DIR "\\uninstall.exe\"","1.0","CiukiOS",INSTALLER_DIR};
    unsigned errors=0;int present=stage>=2 && stage<=5;
    if(present){for(unsigned i=0;i<5;i++)errors+=value(INSTALLER_UNINSTALL,names[i],values[i]);}
    else{
        struct w32r_reply r;uint8_t bytes[4];uint32_t e=request(W32R_OPEN,W32R_HKLM,INSTALLER_UNINSTALL,"",&r,bytes,sizeof(bytes));
        if(!e){uint32_t key=r.key;request(W32R_CLOSE,key,"","",&r,bytes,sizeof(bytes));errors++;}
        else if(e!=2)errors++;
    }
    errors+=value(INSTALLER_RUNONCE,"CiukiInstallerFixture",stage==3?"\"" INSTALLER_DIR "\\child.exe\" /restart-child":NULL);
    return errors;
}
int desk_installer_ready(void)
{
    struct w32r_reply r;uint8_t bytes[4];if(request(W32R_OPEN,W32R_HKLM,"Software","",&r,bytes,sizeof(bytes)))return 0;
    uint32_t key=r.key;return !request(W32R_CLOSE,key,"","",&r,bytes,sizeof(bytes));
}
