/* Synthetic packages only. SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "apps/control/driver_pack.h"
#include <ciuki/sha256.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const struct ndp_device device={0,3,0,0x1234,0x5678,0x1111,0x2222,7};
static const char inf[]=
"; synthetic, no vendor payload\n[Version]\nSignature=\"$Windows NT$\"\nClass=Net\nProvider=%vendor%\nDriverVer=10/01/2001,1.2.3.4\n"
"[Manufacturer]\n%vendor%=Models,NTx86.5.0,NTx86.5.1,NTx86.5.2,NTamd64.5.2\n"
"[Models.NTx86.5.0]\n%label%=Old,PCI\\VEN_1234&DEV_5678\n"
"[Models.NTx86.5.1]\n%label%=Xp,PCI\\VEN_1234&DEV_5678\n"
"[Models.NTx86.5.2]\n%label%=Base,PCI\\VEN_1234&DEV_5678\n%label%=Card,PCI\\VEN_1234&DEV_5678&SUBSYS_22221111&REV_07\n"
"[Models.NTamd64.5.2]\n%label%=Wrong,PCI\\VEN_1234&DEV_5678&SUBSYS_22221111&REV_07\n"
"[Old.NTx86]\nNeeds=Card.NTx86\n[Old.NTx86.Services]\nNeeds=Card.NTx86.Services\n"
"[Xp.NTx86]\nNeeds=Card.NTx86\n[Xp.NTx86.Services]\nNeeds=Card.NTx86.Services\n"
"[Base.NTx86]\nNeeds=Card.NTx86\n[Base.NTx86.Services]\nNeeds=Card.NTx86.Services\n"
"[Card.NTx86]\nCharacteristics=4\nBusType=5\nCopyFiles=driver.files\nInclude=common.inf\nNeeds=shared.defaults\nAddReg=adapter.params\n"
"[Card.NTx86.Services]\nAddService=Synthetic,2,service\n"
"[service]\nServiceType=1\nServiceBinary=%12%\\mini.sys\nStartType=3\nErrorControl=1\nAddReg=service.params\n"
"[driver.files]\nmini.sys\nsupport.dat\n"
"[adapter.params]\nHKR,,Count,0x10001,42\nHKR,,Bytes,1,01,ff,7f\nHKR,,Label,0,\"quoted; label, one\"\n"
"HKR,,Expanded,0,%value%\nHKR,,Percent,0,100%%\nHKR,Ndi\\Params\\Speed,type,0,int\nHKR,Ndi\\Params\\Speed,min,0,1\nHKR,Ndi\\Params\\Speed,max,0,100\nHKR,Ndi\\Params\\Speed,default,0,10\n"
"HKR,Ndi\\Params\\Mode,type,0,enum\nHKR,Ndi\\Params\\Mode,default,0,auto\nHKR,Ndi\\Params\\Mode\\enum,auto,0,Automatic\n"
"[service.params]\nHKR,Parameters,Owner,0,service-view\n"
"[Strings.0409]\nvendor=Incorrect localized vendor\n[Strings]\nvendor=\"Synthetic Provider\"\nlabel=\"Synthetic Adapter\"\nvalue=\"quoted; token, two\"\n";
static const char common[]="[shared.defaults]\nAddReg=shared.params\n[shared.params]\nHKR,,Shared,0x10001,7\n";
static void path(char out[512],const char *a,const char *b) { assert(snprintf(out,512,"%s/%s",a,b)<512); }
static void put(const char *root,const char *name,const void *bytes,size_t n)
{ char p[512]; path(p,root,name); FILE *f=fopen(p,"wb"); assert(f); assert(fwrite(bytes,1,n,f)==n); assert(!fclose(f)); }
static int count(const char *p)
{ DIR *d=opendir(p); assert(d); int n=0; struct dirent *e; while((e=readdir(d))) if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")) n++; assert(!closedir(d)); return n; }
static int fetch(void *ctx,const char *name,uint8_t **out,size_t *n)
{
    (void)ctx; const char *text=!strcmp(name,"synthetic.inf") ? inf : !strcmp(name,"common.inf") ? common : NULL;
    if(!text) return -ENOENT;
    *n=strlen(text); *out=malloc(*n); assert(*out); memcpy(*out,text,*n); return 0;
}
static const char *changed;
static int fetch_changed(void *ctx,const char *name,uint8_t **out,size_t *n)
{ if(strcmp(name,"synthetic.inf")) return fetch(ctx,name,out,n); *n=strlen(changed); *out=malloc(*n); assert(*out); memcpy(*out,changed,*n); return 0; }
static char *replace(const char *old,const char *find,const char *with)
{
    const char *at=strstr(old,find); assert(at); size_t size=strlen(old)-strlen(find)+strlen(with)+1;
    char *s=malloc(size); assert(s); size_t prefix=(size_t)(at-old); memcpy(s,old,prefix); strcpy(s+prefix,with); strcpy(s+prefix+strlen(with),at+strlen(find)); return s;
}
static const struct ndp_parameter *param(const struct ndp_manifest *m,const char *key,const char *name)
{ for(unsigned i=0;i<m->parameter_count;i++) if(!strcmp(m->parameters[i].key,key) && !strcmp(m->parameters[i].name,name)) return &m->parameters[i]; assert(0); return NULL; }
static int failure_at,operations;
static int inject(const char *op,void *ctx) { (void)op;(void)ctx; return ++operations==failure_at; }
static int read_package(void *ctx,const char *name,uint32_t off,void *out,size_t cap,size_t *got)
{ char p[512]; path(p,ctx,name); int fd=open(p,O_RDONLY); if(fd<0) return -1; ssize_t n=pread(fd,out,cap,off); int r=n<0?-1:0; if(n>=0)*got=(size_t)n; assert(!close(fd)); return r; }
static void bad(struct dp_plan *plan,char reason[160],const char *needle,const char *replacement)
{
    char *s=replace(inf,needle,replacement); changed=s;
    assert(dp_parse(plan,"synthetic.inf",&device,2,fetch_changed,NULL,reason)); assert(*reason); free(s);
}
int main(int argc,char **argv)
{
    assert(argc==2); struct dp_plan *plan=calloc(1,sizeof(*plan)),*other=calloc(1,sizeof(*other)); assert(plan && other); char reason[160];
    assert(!dp_parse(plan,"synthetic.inf",&device,2,fetch,NULL,reason));
    assert(!strcmp(plan->manifest.install,"Card.ntx86")); assert(!strcmp(plan->manifest.models,"Models.NTx86.5.2"));
    assert(!strcmp(plan->manifest.provider,"Synthetic Provider")); assert(!strcmp(plan->manifest.name,"Synthetic Adapter"));
    assert(!plan->manifest.enabled && !strcmp(plan->manifest.reason,NDP_DISABLED));
    const struct ndp_parameter *v=param(&plan->manifest,"adapter/","Count"); assert(v->type==NDP_DWORD && v->bytes==4 && v->data[0]==42);
    v=param(&plan->manifest,"adapter/","Bytes"); assert(v->type==NDP_BINARY && v->bytes==3 && v->data[1]==255);
    v=param(&plan->manifest,"adapter/","Label"); assert(!strcmp((char *)v->data,"quoted; label, one"));
    assert(!strcmp((char *)param(&plan->manifest,"adapter/","Expanded")->data,"quoted; token, two"));
    assert(!strcmp((char *)param(&plan->manifest,"adapter/","Percent")->data,"100%"));
    v=param(&plan->manifest,"adapter/","Speed"); assert(v->type==NDP_DWORD && v->data[0]==10);
    assert(!strcmp((char *)param(&plan->manifest,"adapter/","Mode")->data,"auto"));
    assert(param(&plan->manifest,"service/Parameters","Owner")->type==NDP_STRING);
    assert(param(&plan->manifest,"adapter/","Shared")->data[0]==7);
    assert(!dp_parse(other,"synthetic.inf",&device,1,fetch,NULL,reason));
    assert(!strcmp(other->manifest.models,"Models.NTx86.5.1") && !strcmp(other->manifest.install,"Xp.ntx86"));
    assert(!dp_parse(other,"synthetic.inf",&device,0,fetch,NULL,reason)); assert(!strcmp(other->manifest.install,"Old.ntx86"));
    struct ndp_device revised=device; revised.revision=8;
    assert(!dp_parse(other,"synthetic.inf",&revised,2,fetch,NULL,reason)); assert(!strcmp(other->manifest.install,"Base.ntx86"));
    struct ndp_device unknown=device; unknown.vendor=0x9999; assert(dp_parse(other,"synthetic.inf",&unknown,2,fetch,NULL,reason));
    bad(other,reason,"Syntheti", "../Synthetic");
    bad(other,reason,"AddService=Synthetic,2,service","AddService=Synthetic,0,service");
    bad(other,reason,"Include=common.inf","Include=../common.inf");
    bad(other,reason,"Needs=shared.defaults","Needs=absent");
    bad(other,reason,"Needs=shared.defaults","Needs=Card.NTx86");
    bad(other,reason,"mini.sys\nsupport.dat","mini.sys\n../support.dat");
    bad(other,reason,"mini.sys\nsupport.dat","mini.sys\nsetup.exe");
    bad(other,reason,"mini.sys\nsupport.dat","mini.sys,mini.sy_\nsupport.dat");
    bad(other,reason,"HKR,,Count","HKLM,,Count");
    bad(other,reason,"Count,0x10001,42","Count,0x10001,4294967296");
    bad(other,reason,"Count,0x10001,42","Count,0x10001,2,3");
    bad(other,reason,"Count,0x10001,42","Count,0x10003,42");
    bad(other,reason,"Bytes,1,01,ff,7f","Bytes,1,zz");
    bad(other,reason,"default,0,10","default,0,101");
    bad(other,reason,"default,0,auto","default,0,missing");
    bad(other,reason,"Characteristics=4","Characteristics=0x400");
    bad(other,reason,"BusType=5","BusType=6");
    bad(other,reason,"AddReg=adapter.params","RunPreSetupCommands=execute");
    bad(other,reason,"mini.sys\nsupport.dat","mini.sys\nmini.sys,other.sys");
    bad(other,reason,"Models,NTx86.5.0,NTx86.5.1,NTx86.5.2,NTamd64.5.2","Models,NTx86.6.0,NTamd64.5.2");
    assert(!ndp_component("../x") && !ndp_component("x.") && !ndp_relative("x/../y") && !ndp_relative("/absolute") && !ndp_relative("x//y"));
    char media[512],store[512],installed[512]; path(media,argv[1],"media"); path(store,argv[1],"drivers"); assert(!mkdir(media,0700)); assert(!mkdir(store,0700));
    put(media,"synthetic.inf",inf,strlen(inf)); put(media,"common.inf",common,strlen(common)); put(media,"support.dat","support",7);
    uint8_t pe[256]={0}; pe[0]='M'; pe[1]='Z'; pe[60]=64; memcpy(pe+64,"PE\0\0",4); pe[68]=0x4c; pe[69]=1; pe[88]=0x0b; pe[89]=1; pe[156]=1; put(media,"mini.sys",pe,sizeof(pe));
    assert(!dp_preview(plan,media,"synthetic.inf",&device,reason)); assert(ndp_manifest_valid(&plan->manifest));
    char names[32][192]; assert(dp_scan(media,names,32)==2);
    int descriptors=count("/proc/self/fd");
    unsigned failures=0;
    for(failure_at=1;failure_at<200;failure_at++) {
        operations=0; dp_test_fault(inject,NULL); int r=dp_install(plan,media,store,reason); dp_test_fault(NULL,NULL);
        if(!r) { assert(!dp_uninstall(&device,store,reason)); break; }
        failures++; assert(count(store)==0); assert(dp_installed(&device,store)==-ENOENT); assert(count("/proc/self/fd")==descriptors);
    }
    assert(failures>20 && failure_at<200);
    put(media,"support.dat","changed",7); assert(dp_install(plan,media,store,reason)); assert(count(store)==0); put(media,"support.dat","support",7);
    assert(!dp_install(plan,media,store,reason)); assert(!dp_installed(&device,store)); path(installed,store,plan->manifest.package);
    assert(ndp_verify(&plan->manifest,&device,read_package,installed));
    assert(!ndp_verify(&plan->manifest,&unknown,read_package,installed));
    assert(dp_install(plan,media,store,reason)==-EEXIST); assert(count(store)==1);
    failure_at=1; operations=0; dp_test_fault(inject,NULL); assert(dp_uninstall(&device,store,reason)); dp_test_fault(NULL,NULL); assert(!dp_installed(&device,store));
    put(installed,"registry.ndp","bad",3); assert(!ndp_verify(&plan->manifest,&device,read_package,installed));
    assert(!dp_uninstall(&device,store,reason)); assert(!count(store));
    for(int point=2;point<=9;point++) {
        assert(!dp_install(plan,media,store,reason));
        failure_at=point; operations=0; dp_test_fault(inject,NULL); int r=dp_uninstall(&device,store,reason); dp_test_fault(NULL,NULL);
        if(r) assert(!dp_uninstall(&device,store,reason));
        assert(!count(store));
    }
    for(unsigned i=0;i<100;i++) { assert(!dp_install(plan,media,store,reason)); assert(!dp_uninstall(&device,store,reason)); assert(!count(store)); }
    assert(count("/proc/self/fd")==descriptors);
    char file[512],link[512]; path(file,media,"support.dat"); path(link,media,"linked.dat"); assert(!symlink(file,link));
    char *s=replace(inf,"support.dat","linked.dat"); put(media,"synthetic.inf",s,strlen(s)); free(s); assert(dp_preview(other,media,"synthetic.inf",&device,reason)); assert(!unlink(link));
    /* UTF-16 LE and BE, including BOM-less input, use the production decoder. */
    size_t len=strlen(inf); uint8_t *wide=calloc(len*2+2,1); assert(wide);
    wide[0]=255; wide[1]=254; for(size_t i=0;i<len;i++) wide[2+i*2]=(uint8_t)inf[i]; put(media,"synthetic.inf",wide,len*2+2);
    assert(!dp_preview(other,media,"synthetic.inf",&device,reason));
    wide[0]=254; wide[1]=255; for(size_t i=0;i<len;i++) { wide[2+i*2]=0; wide[3+i*2]=(uint8_t)inf[i]; } put(media,"synthetic.inf",wide,len*2+2);
    assert(!dp_preview(other,media,"synthetic.inf",&device,reason)); put(media,"synthetic.inf",wide+2,len*2); assert(!dp_preview(other,media,"synthetic.inf",&device,reason)); free(wide);
    /* Case-insensitive source lookup, with collisions rejected. */
    put(media,"synthetic.inf",inf,strlen(inf)); path(file,media,"mini.sys"); path(link,media,"MINI.SYS"); assert(!rename(file,link));
    assert(!dp_preview(other,media,"synthetic.inf",&device,reason)); put(media,"mini.sys",pe,sizeof(pe)); assert(dp_preview(other,media,"synthetic.inf",&device,reason)); assert(!unlink(link));
    free(plan); free(other);
    printf("PASS driver packs: exact ID/OS/service selection, Includes/Needs, typed defaults, hostile paths, %u rollback points, 100 install/remove cycles, hash and registry checks, UTF-16, stable descriptors\n",failures);
    fflush(stdout);
    return 0;
}
