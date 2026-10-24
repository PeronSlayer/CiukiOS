/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/files/backup.h"
#include "../../../apps/files/cleanup.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char root[FM_PATH];
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while(0)
static void path(char out[FM_PATH],const char *name)
{ int n=snprintf(out,FM_PATH,"%s/%s",root,name); CHECK(n>0 && n<FM_PATH); }
static void put(const char *p,const char *s)
{ FILE *f=fopen(p,"wb"); CHECK(f); CHECK(fwrite(s,1,strlen(s),f)==strlen(s)); CHECK(!fclose(f)); }
static void expect(const char *p,const char *s)
{ char text[100]={0}; FILE *f=fopen(p,"rb"); CHECK(f); CHECK(fread(text,1,sizeof(text),f)==strlen(s)); CHECK(!strcmp(text,s)); CHECK(!fclose(f)); }
static int cancel(void *ctx) { return *(int *)ctx; }
static int fail_write(void *ctx,int fd,const void *data,unsigned bytes)
{ (void)ctx; (void)fd; (void)data; (void)bytes; return -EIO; }
static int no(void *ctx,const char *p) { (void)ctx; (void)p; return 0; }
int main(int argc,char **argv)
{
    CHECK(argc==2); snprintf(root,sizeof(root),"%s",argv[1]);
    static char source[FM_PATH],file[FM_PATH],empty[FM_PATH],full[FM_PATH],diff[FM_PATH],dest[FM_PATH],target[FM_PATH];
    path(source,"source"); CHECK(!mkdir(source,0700)); path(file,"source/Long name.txt"); put(file,"original bytes");
    path(empty,"source/empty"); CHECK(!mkdir(empty,0700)); path(full,"full.cbk"); path(diff,"diff.cbk"); path(dest,"restore"); CHECK(!mkdir(dest,0700));
    const struct fm_vfs *fs=fm_native_backup_vfs(); struct bk_result result; const char *sources[]={source,file};
    CHECK(!bk_create(fs,full,sources,2,NULL,NULL,&result)); CHECK(result.files==1 && result.folders==2);
    CHECK(!bk_verify(fs,full,NULL,&result) && result.files==1);
    struct fm_vfs faulty=*fs; faulty.write=fail_write;
    CHECK(bk_create(&faulty,full,sources,2,NULL,NULL,&result)==-EIO);
    CHECK(!bk_verify(fs,full,NULL,&result) && result.files==1);
    CHECK(bk_restore(&faulty,full,NULL,BK_ALWAYS,NULL,&result)==-EIO); expect(file,"original bytes");
    CHECK(bk_create(fs,full,NULL,0,full,NULL,&result)==-EINVAL);
    CHECK(!bk_restore(fs,full,NULL,BK_NEVER,NULL,&result) && result.skipped==1); expect(file,"original bytes");
    put(file,"changed bytes");
    static char added[FM_PATH]; path(added,"source/new.txt"); put(added,"not in the fixed catalogue");
    CHECK(!bk_create(fs,diff,NULL,0,full,NULL,&result) && result.files==1);
    static char chained[FM_PATH]; path(chained,"chained.cbk"); CHECK(bk_create(fs,chained,NULL,0,diff,NULL,&result)==-EINVAL);
    CHECK(!bk_restore(fs,full,NULL,BK_ALWAYS,NULL,&result)); expect(file,"original bytes");
    CHECK(!bk_restore(fs,diff,NULL,BK_ALWAYS,NULL,&result)); expect(file,"changed bytes");
    CHECK(!bk_restore(fs,full,dest,BK_ALWAYS,NULL,&result));
    int n=snprintf(target,sizeof(target),"%s%s",dest,file); CHECK(n>0 && n<FM_PATH); expect(target,"original bytes");
    struct bk_callbacks cb={.replace=no};
    CHECK(!bk_restore(fs,full,NULL,BK_ASK,&cb,&result) && result.skipped==1); expect(file,"changed bytes");
    CHECK(!bk_restore(fs,full,NULL,BK_OLDER,NULL,&result) && result.skipped==1);
    int cancelled=1; cb=(struct bk_callbacks){.ctx=&cancelled,.cancel=cancel};
    CHECK(bk_create(fs,full,sources,2,NULL,&cb,&result)==-ECANCELED); CHECK(!bk_verify(fs,full,NULL,&result));
    static char link[FM_PATH]; path(link,"link"); CHECK(!symlink(source,link)); const char *unsafe[]={link};
    CHECK(bk_create(fs,full,unsafe,1,NULL,NULL,&result)<0);
    unsafe[0]="/dev/null"; CHECK(bk_create(fs,full,unsafe,1,NULL,NULL,&result)==-EINVAL);
    CHECK(bk_restore(fs,full,"/dev",BK_ALWAYS,NULL,&result)==-EINVAL);
    CHECK(bk_restore(fs,full,"/tmp/../dev",BK_ALWAYS,NULL,&result)==-EINVAL);
    /* Corrupt data: Verify and Restore reject before replacing any file. */
    FILE *f=fopen(full,"r+b"); CHECK(f); CHECK(!fseek(f,-1,SEEK_END)); int c=fgetc(f); CHECK(c!=EOF);
    CHECK(!fseek(f,-1,SEEK_END)); CHECK(fputc(c^0x80,f)!=EOF); CHECK(!fclose(f));
    CHECK(bk_verify(fs,full,NULL,&result)<0); CHECK(bk_restore(fs,full,NULL,BK_ALWAYS,NULL,&result)<0); expect(file,"changed bytes");
    CHECK(!bk_create(fs,full,sources,2,NULL,NULL,&result));
    CHECK(!unlink(file)); CHECK(!bk_restore(fs,full,NULL,BK_NEVER,NULL,&result)); expect(file,"changed bytes");
    printf("PASS Backup: full/differential fixed selection, catalogue/checksums, verify, original/alternate restore, overwrite policies, cancellation and corruption/path/symlink refusals (%u checks)\n",checks);
    return 0;
}
