/* SPDX-License-Identifier: MIT
 * Rescue never opens an ATA destination for write. The host boundary also
 * verifies USB FAT provenance on each write, including SETUP.STS. */
#include "setup.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
int st_usb_volume(const struct st_app *a)
{
    int found=-1;
    for (int i=0;i<a->volume_count;i++) {
        const struct st_volume *v=&a->volumes[i];
        if (!v->usb || (strcmp(v->fs,"FAT") && strcmp(v->fs,"FAT12") && strcmp(v->fs,"FAT16") && strcmp(v->fs,"FAT32"))) continue;
        if (found<0 || v->letter<a->volumes[found].letter) found=i;
    }
    return found;
}
static int target(struct st_app *a, char *result, size_t cap)
{
    if (!a->file) { snprintf(result,cap,"Rescue stopped: not enough memory for the copy buffer."); return -ENOMEM; }
    int i=st_usb_volume(a);
    if (i<0 || !a->volumes[i].writable || !a->h.write_file) {
        snprintf(result,cap,"Insert a writable USB FAT volume, then try again."); return -1;
    }
    return i;
}
/* Saves HARDWARE.TXT (Setup's own report: disks, partitions, verdict) and,
 * when the rescue page received the f9-07 report, CIUKIREP.TXT next to it, the
 * file name the f9-07 program and its documentation use. Both are kept
 * because they answer different questions; neither replaces the other. */
int st_save_report(struct st_app *a, char *result, size_t cap)
{
    int i=target(a,result,cap); if(i<0)return -1;
    char path[32]; snprintf(path,sizeof(path),"%c:\\HARDWARE.TXT",a->volumes[i].letter);
    int r=a->h.write_file(a->h.ctx,path,a->report,a->report_length);
    int second=0;
    if(!r && a->link_text && a->link_length){
        snprintf(path,sizeof(path),"%c:\\CIUKIREP.TXT",a->volumes[i].letter);
        r=a->h.write_file(a->h.ctx,path,a->link_text,a->link_length);
        second=1;
    }
    st_record(a,"[rescue] action=hardware_export result=%s files=%d",r?"failed":"ok",a->link_text&&a->link_length?2:1);
    if(r) snprintf(result,cap,second?"HARDWARE.TXT was saved but CIUKIREP.TXT could not be (error %d).":"Cannot save the hardware report (error %d).",-r);
    else if(a->link_text&&a->link_length) snprintf(result,cap,"Hardware report saved to %c:\\CIUKIREP.TXT and %c:\\HARDWARE.TXT.",a->volumes[i].letter,a->volumes[i].letter);
    else snprintf(result,cap,"Hardware report saved to %c:\\HARDWARE.TXT.",a->volumes[i].letter);
    return r;
}
static int rescue_copy(struct st_app *a, char *result, size_t cap)
{
    int i=target(a,result,cap); if(i<0)return -1;
    char usb=a->volumes[i].letter, source=0;
    time_t seconds=(time_t)(a->h.realtime?a->h.realtime(a->h.ctx):0);
    struct tm *date=gmtime(&seconds);char stamp[16],directory[64];
    if(!date || !strftime(stamp,sizeof(stamp),"%Y%m%d",date) || !a->h.make_dir){
        snprintf(result,cap,"Cannot create the dated USB rescue directory.");return -ENOTSUP;
    }
    snprintf(directory,sizeof(directory),"%c:\\CIUKIOS-RESCUE-%s",usb,stamp);
    int made=a->h.make_dir(a->h.ctx,directory);
    if(made){snprintf(result,cap,made==-EEXIST?"The dated rescue directory already exists; keep that export or use another USB volume.":"Cannot create the USB rescue directory (error %d).",-made);return made;}
    for(int d=0;d<a->disk_count;d++) if(a->disks[d].present && !a->disks[d].slot && !a->disks[d].unit)
        for(unsigned p=0;p<a->disks[d].part_count && p<ST_PARTS;p++)
            if(a->disks[d].parts[p].letter) {source=a->disks[d].parts[p].letter;break;}
    static const char *const files[]={"SETUP.LOG","HARDWARE.TXT","SETUP.STATE","BOOT.LOG"};
    unsigned copied=0, missing=0;
    for(unsigned k=0;k<sizeof(files)/sizeof(files[0]);k++) {
        char from[64],to[128]; size_t length=0; int r=-ENOENT;
        snprintf(to,sizeof(to),"%s\\%s",directory,files[k]);
        if(source) {
            snprintf(from,sizeof(from),"%c:\\SYSTEM\\%s%s",source,k==3?"LOGS\\":"",files[k]);
            if (a->h.copy_file) {
                r=a->h.copy_file(a->h.ctx,from,to,a->file,ST_FILE_BYTES);
                if(!r){copied++;continue;}
                if(r!=-ENOENT){snprintf(result,cap,"Cannot copy %s (error %d).",files[k],-r);return r;}
            } else r=a->h.read_file(a->h.ctx,from,a->file,ST_FILE_BYTES,&length);
        }
        /* Production streams large disk logs through this same fixed
         * buffer. Read-only test hosts may supply the bounded read fallback. */
        if(r==-ENOENT && k==0){length=setup_log_copy(&a->log,a->file,ST_FILE_BYTES);r=0;}
        if(r==-ENOENT && k==1){memcpy(a->file,a->report,a->report_length);length=a->report_length;r=0;}
        if(r==-ENOENT){missing++;continue;}
        if(r){snprintf(result,cap,"Cannot read %s (error %d). %u logs saved.",files[k],-r,copied);return r;}
        snprintf(to,sizeof(to),"%s\\%s",directory,files[k]);
        r=a->h.write_file(a->h.ctx,to,a->file,length);
        if(r){snprintf(result,cap,"Cannot copy %s to USB (error %d).",files[k],-r);return r;}
        copied++;
    }
    snprintf(result,cap,"%u logs copied to %s; %u unavailable. ATA disks were read only.",copied,directory,missing);
    return 0;
}

int st_rescue_copy(struct st_app *a,char *result,size_t cap)
{
    int r=rescue_copy(a,result,cap);
    st_record(a,"[rescue] action=logs_export result=%s",r?"failed":"ok");
    return r;
}
