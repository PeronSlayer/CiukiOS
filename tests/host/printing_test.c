/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../apps/libprint/print.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static char root[128],out[256];
static void makepath(char *p,size_t cap,unsigned id,const char *leaf){snprintf(p,cap,"%s/j%03u/%s",root,id,leaf);}
static size_t slurp(const char *path,char *outbuf,size_t cap)
{FILE *f=fopen(path,"rb");assert(f);size_t n=fread(outbuf,1,cap-1,f);outbuf[n]=0;fclose(f);return n;}
static void drain(void){for(unsigned i=0;i<2000;i++){int r=cp_service_step(root);assert(r>=0);if(!r)return;}assert(!"queue stuck");}
static void raw_and_controls(void)
{
    struct cp_config c;assert(!cp_config_load(root,&c));assert(c.default_printer==0 && c.printers[0].driver==CP_TEXT);
    assert(!cp_add_printer(root,"PostScript",out,CP_POSTSCRIPT,NULL));assert(!cp_set_default(root,1));assert(!cp_config_load(root,&c) && c.default_printer==1);
    assert(cp_set_default(root,CP_PRINTERS)==-1);
    int id=cp_submit_raw(root,CP_PRINTERS,"Raw","abc",3,NULL,NULL);assert(id>0);
    struct cp_job j;assert(!cp_job_read(root,(unsigned)id,&j) && j.bytes==3 && j.printer==1);
    assert(!cp_job_control(root,id,CP_PAUSE));assert(cp_service_step(root)==0);assert(!cp_job_read(root,id,&j) && j.state==CP_PAUSED);
    assert(!cp_job_control(root,id,CP_RESUME));drain();assert(cp_job_read(root,id,&j)==-1);
    char b[128];assert(slurp(out,b,sizeof(b))==3 && !memcmp(b,"abc",3));
    id=cp_submit_raw(root,1,"cancel","discard",7,NULL,NULL);assert(id>0);assert(!cp_job_control(root,id,CP_CANCEL));drain();assert(cp_job_read(root,id,&j)==-1);
    assert(!cp_printer_pause(root,1,1));id=cp_submit_raw(root,1,"purge","discard",7,NULL,NULL);assert(id>0);assert(cp_service_step(root)==0);assert(!cp_purge(root,1));drain();assert(cp_job_read(root,id,&j)==-1);assert(!cp_printer_pause(root,1,0));
}
static void offline_and_restart(void)
{
    char offline[256];snprintf(offline,sizeof(offline),"%s/missing/output",root);
    char text[2500];memset(text,'X',sizeof(text));int id=cp_submit_raw(root,1,"Offline",text,sizeof(text),NULL,offline);assert(id>0);assert(cp_service_step(root)==0);
    struct cp_job j;assert(!cp_job_read(root,id,&j) && j.state==CP_OFFLINE && !j.offset);
    char dir[256];snprintf(dir,sizeof(dir),"%s/missing",root);assert(!mkdir(dir,0700));assert(cp_service_step(root)==1);assert(!cp_job_read(root,id,&j) && j.offset==1024);
    /* A fresh service call restores offset from disk, without retained buffers. */
    assert(cp_service_step(root)==1);assert(!cp_job_read(root,id,&j) && j.offset==2048);drain();assert(cp_job_read(root,id,&j)==-1);
    char b[2600];assert(slurp(offline,b,sizeof(b))==sizeof(text) && !memcmp(b,text,sizeof(text)));unlink(offline);rmdir(dir);
}
static void rendering(void)
{
    struct cp_page p;cp_page_default(&p);p.copies=2;
    int id=cp_submit_text(root,1,"Escapes","a(b)\\\nend",10,&p,NULL);assert(id>0);
    char path[256],b[32000];makepath(path,sizeof(path),id,"data");slurp(path,b,sizeof(b));assert(strstr(b,"%!PS-Adobe") && strstr(b,"a\\(b\\)\\\\") && strstr(b,"showpage"));drain();size_t n=slurp(out,b,sizeof(b));assert(n>100);
    unsigned pcl;assert(!cp_add_printer(root,"PCL",out,CP_PCL,&pcl));
    uint32_t pixels[]={0,0xffffff,0xff0000,0};id=cp_submit_image(root,pcl,"Image",pixels,2,2,NULL,NULL);assert(id>0);makepath(path,sizeof(path),id,"data");n=slurp(path,b,sizeof(b));assert(n>100 && strstr(b,"\033*r1A") && strstr(b,"\033*b0M"));drain();
    id=cp_submit_image(root,1,"Colour",pixels,2,2,NULL,NULL);assert(id>0);makepath(path,sizeof(path),id,"data");slurp(path,b,sizeof(b));assert(strstr(b,"colorimage") && strstr(b,"000000ffffff"));drain();
    assert(cp_submit_image(root,0,"Text only",pixels,2,2,NULL,NULL)==-1 && errno==EINVAL);
    cp_page_default(&p);p.height=1008;
    id=cp_submit_text(root,pcl,"Legal","legal",5,&p,NULL);assert(id>0);
    makepath(path,sizeof(path),id,"data");slurp(path,b,sizeof(b));assert(strstr(b,"\033&l3A"));drain();
    p.width=600;assert(cp_submit_text(root,pcl,"Custom","x",1,&p,NULL)==-1 && errno==EINVAL);
    cp_page_default(&p);p.height=144;p.top=12;p.bottom=12;p.first=2;p.last=2;
    char lines[300];size_t at=0;for(unsigned i=0;i<20;i++)at+=(size_t)snprintf(lines+at,sizeof(lines)-at,"line%u\n",i);
    id=cp_submit_text(root,1,"Range",lines,at,&p,NULL);assert(id>0);makepath(path,sizeof(path),id,"data");slurp(path,b,sizeof(b));assert(!strstr(b,"(line0)") && strstr(b,"(line10)"));drain();
    id=cp_submit_text(root,0,"Text","a\nb",3,NULL,out);assert(id>0);makepath(path,sizeof(path),id,"data");slurp(path,b,sizeof(b));assert(strstr(b,"a\r\n") && strchr(b,'\f'));drain();
    assert(cp_submit_raw(root,1,"Oversize","x",CP_MAX_BYTES+1,NULL,NULL)==-1);
    cp_page_default(&p);p.copies=0;assert(cp_submit_raw(root,1,"Invalid","x",1,&p,NULL)==-1);
}
static void full_and_corrupt(void)
{
    for(unsigned i=1;i<=CP_JOBS;i++){int id=cp_submit_raw(root,1,"Full","x",1,NULL,NULL);assert(id==(int)i);}
    assert(cp_submit_raw(root,1,"Full","x",1,NULL,NULL)==-1 && errno==ENOSPC);assert(!cp_purge(root,1));drain();
    char p[256],bak[256],tmp[256];snprintf(p,sizeof(p),"%s/printers",root);snprintf(bak,sizeof(bak),"%s/printers.bak",root);snprintf(tmp,sizeof(tmp),"%s/printers.tmp",root);
    assert(!rename(p,bak));struct cp_config c;assert(!cp_config_load(root,&c) && c.default_printer==1);assert(!rename(bak,p));
    int id=cp_submit_raw(root,1,"Recovery","x",1,NULL,NULL);assert(id>0);makepath(p,sizeof(p),id,"job");makepath(bak,sizeof(bak),id,"job.bak");assert(!rename(p,bak));struct cp_job j;assert(!cp_job_read(root,id,&j));drain();
    /* An unpublished directory and partial metadata cannot become output. */
    makepath(p,sizeof(p),1,"");assert(!mkdir(p,0700));makepath(tmp,sizeof(tmp),1,"job.tmp");FILE *f=fopen(tmp,"wb");assert(f);fputs("bad",f);fclose(f);assert(cp_job_read(root,1,&j)==-1);assert(cp_service_step(root)==0);unlink(tmp);makepath(p,sizeof(p),1,"");rmdir(p);
}
static void producers_and_lock(void)
{
    pid_t child=fork();assert(child>=0);
    if(!child){assert(!cp_service_lock(root));_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!cp_service_lock(root));assert(cp_service_lock(root)==-1 && errno==EBUSY);cp_service_unlock(root);
    pid_t producers[4];
    for(unsigned i=0;i<4;i++){
        producers[i]=fork();assert(producers[i]>=0);
        if(!producers[i]){char text[32];snprintf(text,sizeof(text),"producer%u",i);int id=cp_submit_raw(root,1,text,text,strlen(text),NULL,NULL);_exit(id>0?0:1);}
    }
    for(unsigned i=0;i<4;i++)assert(waitpid(producers[i],&status,0)==producers[i] && WIFEXITED(status) && !WEXITSTATUS(status));
    unsigned seen=0;for(unsigned id=1;id<=4;id++){struct cp_job j;assert(!cp_job_read(root,id,&j));assert(!strncmp(j.title,"producer",8));unsigned n=(unsigned)(j.title[8]-'0');assert(n<4 && !(seen&(1u<<n)));seen|=1u<<n;}assert(seen==15);
    assert(!cp_purge(root,1));drain();
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    char tmp[]="/tmp/ciuki-print-XXXXXX";char *p=mkdtemp(tmp);assert(p);snprintf(root,sizeof(root),"%s",p);snprintf(out,sizeof(out),"%s/output.prn",root);
    assert(sizeof(struct cp_job)==424 && sizeof(struct cp_page)==40 && sizeof(struct cp_config)==2988);assert(offsetof(struct cp_job,page)==32 && offsetof(struct cp_job,title)==72 && offsetof(struct cp_job,target)==168);assert(!cp_init(root));assert(!cp_service_lock(root));assert(cp_service_lock(root)==-1 && errno==EBUSY);cp_service_unlock(root);raw_and_controls();offline_and_restart();rendering();full_and_corrupt();producers_and_lock();
    char path[256];snprintf(path,sizeof(path),"%s/printers",root);unlink(path);unlink(out);assert(!rmdir(root));puts("printing: PASS (queue, controls, recovery, drivers, offline, copies, range, full)");return 0;
}
