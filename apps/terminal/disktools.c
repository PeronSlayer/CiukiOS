/* G29c Terminal front end. All disk writes require explicit YES and the
 * kernel's exclusive nonboot lease. SPDX-License-Identifier: MIT */
#include "terminal.h"
#include "../files/backup.h"
#include "../files/cleanup.h"
#include "../../sdk/libdisk/partition_tool.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int cancel(void *ctx)
{ struct terminal *t=ctx; if(t->host.pump)t->host.pump(t->host.ctx); return t->interrupted || t->quit; }
static void progress(void *ctx,const char *path,const struct bk_result *r)
{ struct terminal *t=ctx; term_printf(t,"%u file(s), %llu bytes: %s\n",r->files,(unsigned long long)r->bytes,path); if(t->host.pump)t->host.pump(t->host.ctx); }
static int number(const char *s,uint32_t *out)
{ char *end; errno=0; unsigned long n=strtoul(s,&end,10); if(!*s || *end || errno || n>UINT32_MAX || *s=='-')return -EINVAL; *out=(uint32_t)n; return 0; }
static int backup(struct terminal *t,int n,char **a)
{
    struct bk_callbacks cb={.ctx=t,.cancel=cancel,.progress=progress}; struct bk_result result;
    if(n<3) { term_write(t,"BACKUP FULL archive file-or-folder [...]\nBACKUP DIFF full-archive archive\nBACKUP VERIFY archive\nRESTORE archive ORIGINAL|folder NEVER|OLDER|ALWAYS\n"); return 0; }
    char archive[FM_PATH],base[FM_PATH]; int e=0;
    if(!strcmp(a[0],"RESTORE") || fm_same_path(a[0],"restore")) {
        if(n!=4)return -EINVAL;
        if((e=term_path(t,a[1],archive)))return e;
        enum bk_replace policy=fm_same_path(a[3],"NEVER") ? BK_NEVER : fm_same_path(a[3],"OLDER") ? BK_OLDER : BK_ALWAYS;
        if(policy==BK_ALWAYS && !fm_same_path(a[3],"ALWAYS"))return -EINVAL;
        const char *root=NULL;
        if(!fm_same_path(a[2],"ORIGINAL")) { if((e=term_path(t,a[2],base)))return e; root=base; }
        e=bk_restore(fm_native_backup_vfs(),archive,root,policy,&cb,&result);
    } else if(fm_same_path(a[1],"VERIFY")) {
        if(n!=3)return -EINVAL; if((e=term_path(t,a[2],archive)))return e;
        e=bk_verify(fm_native_backup_vfs(),archive,&cb,&result);
    } else if(fm_same_path(a[1],"DIFF")) {
        if(n!=4)return -EINVAL;
        if((e=term_path(t,a[2],base)) || (e=term_path(t,a[3],archive)))return e;
        e=bk_create(fm_native_backup_vfs(),archive,NULL,0,base,&cb,&result);
    } else if(fm_same_path(a[1],"FULL")) {
        if(n<4)return -EINVAL; if((e=term_path(t,a[2],archive)))return e;
        char (*paths)[FM_PATH]=calloc((unsigned)n-3,FM_PATH); const char **sources=calloc((unsigned)n-3,sizeof(*sources));
        if(!paths || !sources) { free(paths); free(sources); return -ENOMEM; }
        for(int i=3;!e && i<n;i++) { e=term_path(t,a[i],paths[i-3]); sources[i-3]=paths[i-3]; }
        if(!e)e=bk_create(fm_native_backup_vfs(),archive,sources,(unsigned)n-3,NULL,&cb,&result);
        free(paths); free(sources);
    } else return -EINVAL;
    if(!e)term_printf(t,"Completed: %u file(s), %u folder(s), %u skipped, %llu bytes.\n",result.files,result.folders,result.skipped,(unsigned long long)result.bytes);
    return e;
}
static uint32_t read32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void information(struct terminal *t,const struct pt_table *p)
{
    term_printf(t,"Current fixed disk: %u. Capacity: %llu sectors. Usable LBA ceiling: %llu.\n",p->disk,(unsigned long long)p->id.sectors,(unsigned long long)(p->id.sectors<DISK_LBA28 ? p->id.sectors : DISK_LBA28));
    term_write(t,"Number  Active  Type  Start LBA  Sectors\n");
    for(unsigned i=0;i<p->count;i++)for(unsigned k=0;k<(i ? 1u : 4u);k++) {
        const uint8_t *q=p->sector[i]+446+k*16; if(!q[4])continue;
        term_printf(t,"%u       %c      %02X    %u    %u\n",i ? i+4 : k+1,q[0]==0x80 ? '*' : ' ',q[4],p->lba[i]+read32(q+8),read32(q+12));
    }
}
static int fdisk(struct terminal *t,int n,char **a)
{
    if(n<2) {
        term_write(t,"FDISK disk [Y|N [PRIMARY|EXTENDED|LOGICAL start-LBA sectors YES]]\nFDISK disk Y|N DELETE|ACTIVE number YES\nFDISK disk displays information. Disk numbers: 0..3.\nPartition changes destroy access to affected data. Back up first.\n"); return 0;
    }
    uint32_t disk; int e=number(a[1],&disk); if(e || disk>=DISK_SLOTS)return -EINVAL;
    struct pt_table *p=malloc(sizeof(*p)); if(!p)return -ENOMEM;
    e=pt_read(disk,p);
    if(!e) {
        information(t,p);
        if(n<3) {
            if(p->id.sectors>1048576u)term_write(t,"Do you wish to enable large disk support (Y/N)?\nRepeat FDISK with Y (FAT32; older DOS cannot read it) or N (FAT16, up to 2 GiB).\n");
        } else if(!fm_same_path(a[2],"Y") && !fm_same_path(a[2],"N"))e=-EINVAL;
        else if(n>3) {
            p->large=fm_same_path(a[2],"Y");
            if(n<6 || strcmp(a[n-1],"YES")) { term_write(t,"No changes written. Add YES to confirm partition changes.\n"); e=-ECANCELED; }
            else if(fm_same_path(a[3],"DELETE") || fm_same_path(a[3],"ACTIVE")) {
                uint32_t which; e=n!=6 ? -EINVAL : number(a[4],&which);
                if(!e)e=fm_same_path(a[3],"DELETE") ? pt_delete(p,which) : pt_active(p,which);
            } else {
                uint32_t start=0,count=0;
                enum pt_kind kind=fm_same_path(a[3],"PRIMARY") ? PT_PRIMARY : fm_same_path(a[3],"EXTENDED") ? PT_EXTENDED : PT_LOGICAL;
                if(n!=7 || (kind==PT_LOGICAL && !fm_same_path(a[3],"LOGICAL")))e=-EINVAL;
                if(!e)e=number(a[4],&start); if(!e)e=number(a[5],&count);
                if(!e)e=pt_create(p,kind,start,count);
            }
            if(!e && cancel(t))e=-ECANCELED;
            if(!e)e=pt_commit(p);
            if(!e) { information(t,p); term_write(t,"Partition table saved and verified. Restart before using the changed disk. New partitions still require Format.\n"); }
        }
    }
    if(e && e!=-EINVAL && e!=-ECANCELED) {
        const struct disk_error_info *last=disk_last_error();
        term_printf(t,"Partition operation: %s (code %d, errno %d).\n",last->what,last->code,last->err);
        e=last->err ? -last->err : -EIO;
    }
    free(p); return e;
}
int term_disktool(struct terminal *t,int n,char **a)
{ return fm_same_path(a[0],"FDISK") ? fdisk(t,n,a) : backup(t,n,a); }
