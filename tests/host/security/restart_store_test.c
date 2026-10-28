/* Production store/quarantine with lost, torn, reordered and acknowledged
 * I/O outcomes at each write/patch/flush/rename/removal boundary.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/registry/restart_store.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define FILES 24
struct file {char name[112];uint8_t *live,*disk;size_t n,dn;int exists,dexists;};
struct disk {struct file files[FILES];unsigned event,cut,mode;};
static struct file *find(struct disk *d,const char *name,int create)
{for(unsigned i=0;i<FILES;i++)if(!strcmp(d->files[i].name,name))return d->files+i;
 if(create)for(unsigned i=0;i<FILES;i++)if(!d->files[i].name[0]){assert(strlen(name)<112);strcpy(d->files[i].name,name);return d->files+i;}return NULL;}
static void bytes(uint8_t **out,size_t *size,const void *v,size_t n){uint8_t *p=malloc(n?n:1);assert(p);if(n)memcpy(p,v,n);free(*out);*out=p;*size=n;}
static void durable(struct disk *d){for(unsigned i=0;i<FILES;i++){struct file *f=d->files+i;f->dexists=f->exists;if(f->exists)bytes(&f->disk,&f->dn,f->live,f->n);}}
static void crash(struct disk *d){for(unsigned i=0;i<FILES;i++){struct file *f=d->files+i;f->exists=f->dexists;if(f->exists)bytes(&f->live,&f->n,f->disk,f->dn);}d->cut=0;}
static void destroy(struct disk *d){for(unsigned i=0;i<FILES;i++){free(d->files[i].live);free(d->files[i].disk);}memset(d,0,sizeof(*d));}
static void clone(struct disk *out,const struct disk *in){memset(out,0,sizeof(*out));for(unsigned i=0;i<FILES;i++){
 const struct file *a=in->files+i;struct file *b=out->files+i;strcpy(b->name,a->name);b->exists=a->dexists;b->dexists=a->dexists;
 if(a->dexists){bytes(&b->live,&b->n,a->disk,a->dn);bytes(&b->disk,&b->dn,a->disk,a->dn);}}}
static int hit(struct disk *d){return ++d->event==d->cut;}
static int read_file(void *p,const char *name,uint8_t **out,size_t *n){struct file *f=find(p,name,0);if(!f||!f->exists)return -ENOENT;
 *out=malloc(f->n?f->n:1);assert(*out);memcpy(*out,f->live,f->n);*n=f->n;return 0;}
static int write_file(void *p,const char *name,const void *v,size_t n){struct disk *d=p;int cut=hit(d);if(cut&&!d->mode)return -EIO;
 struct file *f=find(d,name,1);assert(f);f->exists=1;bytes(&f->live,&f->n,v,cut?n/2:n);if(cut){durable(d);return -EIO;}return 0;}
static int patch_file(void *p,const char *name,size_t at,const void *v,size_t n){struct disk *d=p;int cut=hit(d);if(cut&&!d->mode)return -EIO;
 struct file *f=find(d,name,0);assert(f&&f->exists&&at<=f->n&&n<=f->n-at);memcpy(f->live+at,v,cut?n/2:n);if(cut){durable(d);return -EIO;}return 0;}
static int sync_file(void *p,const char *name){struct disk *d=p;struct file *f=find(d,name,0);assert(f&&f->exists);int cut=hit(d);if(cut&&!d->mode)return -EIO;durable(d);return cut?-EIO:0;}
static int move_file(void *p,const char *from,const char *to){struct disk *d=p;int cut=hit(d);if(cut&&!d->mode)return -EIO;
 struct file *a=find(d,from,0),*b=find(d,to,1);assert(b);if(!a||!a->exists)return -ENOENT;if(b->exists)return -EEXIST;
 bytes(&b->live,&b->n,a->live,a->n);b->exists=1;if(!cut||d->mode!=2)a->exists=0;if(cut){durable(d);return -EIO;}return 0;}
static int remove_file(void *p,const char *name){struct disk *d=p;int cut=hit(d);if(cut&&!d->mode)return -EIO;
 struct file *f=find(d,name,0);int present=f&&f->exists;if(present)f->exists=0;if(cut){durable(d);return -EIO;}return present?0:-ENOENT;}
static struct rj_io io(struct disk *d){return (struct rj_io){d,read_file,write_file,patch_file,sync_file,move_file,remove_file};}
static uint8_t *generation[2];static size_t lengths[2];static unsigned captured;
static int role(void *p){(void)p;return 0;}
static int retain(void *p,uintptr_t lease){(void)p;return lease==1?0:-ESRCH;}
static void release(void *p,uintptr_t lease){(void)p;assert(lease==1);}
static int identity(void *p,uintptr_t lease,struct rb_identity *c){(void)p;if(lease!=1)return -ESRCH;*c=(struct rb_identity){.uid=1000,.session=1,.volume=42,.machine_sid={1,2,3},.group_count=1,.groups={545}};return 0;}
static int absent(void *p,unsigned scope,uint32_t uid,uint8_t **out,size_t *n){(void)p;(void)scope;(void)uid;(void)out;(void)n;return -ENOENT;}
static int capture(void *p,unsigned scope,uint32_t uid,const void *v,size_t n){(void)p;assert(scope==1&&uid==1000&&captured<2);bytes(&generation[captured],&lengths[captured],v,n);captured++;return 0;}
static int account(void *p,uint32_t uid,struct rb_identity *c,int *enabled){int e=identity(p,1,c);assert(uid==1000);*enabled=1;return e;}
static int authorize(void *p,uintptr_t lease,const struct rb_identity *c,unsigned scope,uint8_t *job,int replay){(void)p;(void)lease;(void)c;(void)scope;(void)job;(void)replay;return 0;}
static int unused(void *p,uintptr_t lease,const uint8_t *job){(void)p;(void)lease;(void)job;return -EOPNOTSUPP;}
static void fixtures(void){struct rb_authority a={NULL,role,retain,release,identity};struct rj_backend b={.read=absent,.write=capture,.account=account,.authorize=authorize,.apply=unused,.launch=unused};
 struct rj_queue q;uint32_t sid[]={1,2,3};assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));
 struct rj_spec spec={.kind=RJ_PROGRAM,.mask=1,.source="",.target="",.executable="/a",.arguments="",.cwd="/"};uint64_t id;
 assert(!rj_admit(&q,1,&spec,&id));assert(!rj_admit(&q,1,&spec,&id));rj_close(&q);}
static const uint32_t sid[]={1,2,3};
static void store_cases(void){struct disk baseline={0};struct rj_io table=io(&baseline);struct rj_store store;assert(!rj_store_init(&store,&table,RJ_PERSONAL,1000,sid,42));
 assert(!rj_store_write(&store,1,1000,generation[0],lengths[0]));durable(&baseline);
 struct disk successful;clone(&successful,&baseline);table=io(&successful);assert(!rj_store_init(&store,&table,1,1000,sid,42));
 assert(!rj_store_write(&store,1,1000,generation[1],lengths[1]));unsigned events=successful.event;crash(&successful);
 uint8_t *v=NULL;size_t n=0;assert(!rj_store_read(&store,1,1000,&v,&n)&&n==lengths[1]&&!memcmp(v,generation[1],n));free(v);destroy(&successful);
 unsigned cuts=0;
 for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=events;cut++){
  struct disk d;clone(&d,&baseline);d.cut=cut;d.mode=mode;table=io(&d);assert(!rj_store_init(&store,&table,1,1000,sid,42));
  assert(rj_store_write(&store,1,1000,generation[1],lengths[1])==-EIO);crash(&d);
  int e=rj_store_read(&store,1,1000,&v,&n);
  if(e)fprintf(stderr,"recover failed mode=%u cut=%u events=%u errno=%d\n",mode,cut,events,e);
  assert(!e);assert((n==lengths[0]&&!memcmp(v,generation[0],n))||(n==lengths[1]&&!memcmp(v,generation[1],n)));free(v);cuts++;destroy(&d);
 }
 table=io(&baseline);assert(!rj_store_init(&store,&table,1,1000,sid,42));assert(rj_store_read(&store,1,1001,&v,&n)==-EACCES);
 uint32_t foreign[]={4,5,6};assert(!rj_store_init(&store,&table,1,1000,foreign,42));assert(rj_store_read(&store,1,1000,&v,&n)==-EIO);
 destroy(&baseline);printf("H07 durable restart store: %u cut/outcome cases; no acknowledged rollback, exact generation/SID/uid and zero adopted replacement paths PASS\n",cuts);}
static void posix_cases(void){char directory[256];const char *temporary=getenv("TMPDIR");assert(temporary);
 int length=snprintf(directory,sizeof(directory),"%s/posix-XXXXXX",temporary);assert(length>0&&(size_t)length<sizeof(directory)&&mkdtemp(directory));
 struct rj_posix posix;struct rj_io table;struct rj_store store;
 assert(!rj_posix_io(&posix,directory,&table)&&!rj_store_init(&store,&table,1,1000,sid,42));
 assert(table.write(table.context,"../escape",generation[0],lengths[0])==-EINVAL);
 assert(!rj_store_write(&store,1,1000,generation[0],lengths[0]));
 assert(!rj_store_write(&store,1,1000,generation[1],lengths[1]));uint8_t *v=NULL;size_t n=0;
 assert(!rj_store_read(&store,1,1000,&v,&n)&&n==lengths[1]&&!memcmp(v,generation[1],n));free(v);
 assert(!table.remove(table.context,"personal.v2")&&!rmdir(directory));
 puts("H07 restart POSIX: real file/directory fsync, exact durable replacement and generated leaf confinement PASS");}
static const char original[]="CIUKIRST 1\nphase=1\nflags=2\nline=0\nhanded=0\n";
static int source(void *p,unsigned index,uint8_t **out,size_t *n){(void)p;if(index)return -ENOENT;*n=sizeof(original)-1;*out=malloc(*n);assert(*out);memcpy(*out,original,*n);return 0;}
static void quarantine_cases(void){struct disk good={0};struct rj_quarantine q={.evidence=io(&good),.source=source,.sid={1,2,3},.volume=42};assert(!rj_quarantine_run(&q));unsigned events=good.event;destroy(&good);unsigned cuts=0;
 for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=events;cut++){
  struct disk d={.cut=cut,.mode=mode};q.evidence=io(&d);assert(rj_quarantine_run(&q)==-EIO);crash(&d);assert(!rj_quarantine_run(&q));
  unsigned data=0,markers=0;for(unsigned i=0;i<FILES;i++)if(d.files[i].exists){struct file *f=d.files+i;
   if(strstr(f->name,".bin")){assert(f->n==sizeof(original)-1&&!memcmp(f->live,original,f->n));data++;}
   if(strstr(f->name,".consumed")){assert(f->n==96&&rb_get32(f->live+92)==0x32514343&&rb_get32(f->live+88)==wr_crc(f->live,88));markers++;}}
  assert(data==1&&markers==1);unsigned before=d.event;assert(!rj_quarantine_run(&q)&&d.event==before+2);cuts++;destroy(&d);
 }
 /* Retry without a process restart after a failed flush. A full live file or
  * patched marker is not yet a durable acknowledgement. */
 for(unsigned cut=1;cut<=events;cut++){
  struct disk d={.cut=cut};q.evidence=io(&d);assert(rj_quarantine_run(&q)==-EIO);d.cut=0;
  assert(!rj_quarantine_run(&q));crash(&d);unsigned found=0;
  for(unsigned i=0;i<FILES;i++)if(d.files[i].exists){struct file *f=d.files+i;
   if(strstr(f->name,".bin"))assert(f->n==sizeof(original)-1&&!memcmp(f->live,original,f->n));
   else {assert(f->n==96&&rb_get32(f->live+92)==0x32514343);found++;}}
  assert(found==1);cuts++;destroy(&d);
 }
 printf("H07 quarantine: %u before/after/torn evidence/marker and live-retry cuts, exact original bytes, idempotent consumed marker, no replay PASS\n",cuts);}
int main(void){setvbuf(stdout,NULL,_IONBF,0);fixtures();store_cases();posix_cases();quarantine_cases();free(generation[0]);free(generation[1]);puts("H07 restart storage production: PASS");return 0;}
