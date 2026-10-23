/* Device Manager local pack workflow. SPDX-License-Identifier: GPL-2.0-only */
#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static const struct ciuki_device_record *selected(struct cp_app *a)
{
    if(!a->devices_ok) return NULL;
    for(unsigned i=0;i<a->devices.count;i++) if(a->devices.devices[i].id==a->selected_device_id) {
        const struct ciuki_device_record *d=&a->devices.devices[i];
        return d->class_id==CIUKI_DEVICE_NETWORK && d->bus_type==CIUKI_BUS_PCI &&
            d->state!=CIUKI_DEVICE_QUARANTINED && d->state!=CIUKI_DEVICE_ABSENT ? d : NULL;
    }
    return NULL;
}
static struct ndp_device identity(const struct ciuki_device_record *d)
{ return (struct ndp_device){d->bus,d->slot,d->function,d->vendor,d->device,d->sub_vendor,d->sub_device,d->revision}; }
void cp_pack_buttons(struct cp_app *a)
{
    struct du_widget *install=du_get(a->ui,CP_PACK_INSTALL),*remove=du_get(a->ui,CP_PACK_REMOVE);
    int eligible=selected(a)!=NULL;
    if(install) install->disabled=!eligible || !a->host.pack_scan || !a->host.pack_preview || !a->host.pack_install;
    if(remove) remove->disabled=!eligible || !a->host.pack_remove;
}
static void message(struct cp_app *a,const char *text)
{ du_message(a->ui,CP_PACK_PREVIEW,"Install driver from pack",text,DU_DLG_ICON_INFO,NULL,0,0,0); a->pack_valid=0; a->pack_index=a->pack_count; }
static void files(struct cp_app *a)
{
    char body[DU_DIALOG_TEXT]; size_t at=0;
    struct ndp_manifest *m=&a->pack->manifest;
    unsigned first=a->pack_file_page*12,end=first+12;
    if(end>m->file_count) end=m->file_count;
    for(unsigned i=first;i<end;i++) {
        int n=snprintf(body+at,sizeof(body)-at,"%s\n",m->files[i].name);
        if(n<0 || (size_t)n>=sizeof(body)-at) break;
        at+=(size_t)n;
    }
    if(!at) snprintf(body,sizeof(body),"No files.");
    static const char *const more[]={"Next files","Back"},*const last[]={"Back"};
    du_message(a->ui,CP_PACK_FILES,"Driver package files",body,DU_DLG_ICON_INFO,end<m->file_count?more:last,end<m->file_count?2:1,0,end<m->file_count?1:0);
}
static void preview(struct cp_app *a)
{
    if(a->pack_index>=a->pack_count) { message(a,"No more INF packages in this location. Select another folder or CD."); return; }
    if(!a->pack) a->pack=calloc(1,sizeof(*a->pack));
    if(!a->pack) { message(a,"Not enough memory to preview the package."); return; }
    int r=a->host.pack_preview(a->host.ctx,a->pack,a->pack_root,a->pack_names[a->pack_index],&a->pack_device,a->pack_reason);
    a->pack_valid=!r;
    char body[DU_DIALOG_TEXT]; size_t at=0;
    if(r) snprintf(body,sizeof(body),"INF: %s\n\nCannot install: %s",a->pack_names[a->pack_index],a->pack_reason);
    else {
        struct ndp_manifest *m=&a->pack->manifest;
        int n=snprintf(body,sizeof(body),"Driver: %s\nProvider: %s\nVersion: %s\nMatching ID: %s\nService: %s\nQualification: %s\nFiles:\n",m->name,m->provider,m->driver_version,m->hardware_id,m->service,m->reason);
        at=n>0 && (size_t)n<sizeof(body) ? (size_t)n : sizeof(body)-1;
        for(unsigned i=0;i<m->file_count && at<sizeof(body)-1;i++) {
            n=snprintf(body+at,sizeof(body)-at,"%s%s",i?", ":"",m->files[i].name);
            if(n<0 || (size_t)n>=sizeof(body)-at) { at=sizeof(body)-1; break; } at+=(size_t)n;
        }
        snprintf(body+at,sizeof(body)-at,"\nNative driver remains the default.");
    }
    static const char *const valid[]={"Install disabled","Files...","Next INF","Cancel"};
    static const char *const qualified[]={"Install","Files...","Next INF","Cancel"};
    static const char *const rejected[]={"Next INF","Cancel"};
    du_message(a->ui,CP_PACK_PREVIEW,"Install driver from pack",body,DU_DLG_ICON_INFO,r?rejected:a->pack->manifest.enabled?qualified:valid,r?2:4,r?0:3,r?1:3);
}
int cp_pack_event(struct cp_app *a,const struct du_event *e)
{
    if(e->id<CP_PACK_INSTALL || e->id>CP_PACK_FILES) return 0;
    if((e->id==CP_PACK_INSTALL || e->id==CP_PACK_REMOVE) && e->type==DU_EV_ACTIVATE) {
        const struct ciuki_device_record *d=selected(a); if(!d) return 1;
        a->pack_device=identity(d);
        if(e->id==CP_PACK_INSTALL && a->host.pack_scan && a->host.pack_preview && a->host.pack_install) {
            du_input_box(a->ui,CP_PACK_LOCATION,"Install driver from pack","Pack folder or mounted CD location:",a->pack_root[0]?a->pack_root:"/media");
        } else if(e->id==CP_PACK_REMOVE && a->host.pack_remove) {
            static const char *const buttons[]={"Remove","Cancel"};
            du_message(a->ui,CP_PACK_CONFIRM_REMOVE,"Remove driver package","Remove this adapter's imported package? The native driver remains the default.",DU_DLG_ICON_WARNING,buttons,2,1,1);
        }
    } else if(e->id==CP_PACK_LOCATION && e->type==DU_EV_DIALOG && !e->index && e->text) {
        if(strlen(e->text)>=sizeof(a->pack_root)) { message(a,"The location is too long."); return 1; }
        strcpy(a->pack_root,e->text); a->pack_index=0;
        a->pack_count=a->host.pack_scan(a->host.ctx,a->pack_root,a->pack_names,32);
        if(a->pack_count<=0) message(a,"No readable INF packages found, or the folder exceeds the bounded scan limit.");
        else preview(a);
    } else if(e->id==CP_PACK_PREVIEW && e->type==DU_EV_DIALOG) {
        if(a->pack_valid && !e->index) {
            int r=a->host.pack_install(a->host.ctx,a->pack,a->pack_root,a->pack_reason);
            if(r && !a->pack_reason[0]) snprintf(a->pack_reason,sizeof(a->pack_reason),"The package could not be installed.");
            message(a,a->pack_reason);
        } else if(a->pack_valid && e->index==1) { a->pack_file_page=0; files(a); }
        else if((a->pack_valid && e->index==2) || (!a->pack_valid && !e->index && a->pack_index<a->pack_count)) { a->pack_index++; preview(a); }
    } else if(e->id==CP_PACK_FILES && e->type==DU_EV_DIALOG && a->pack_valid) {
        if(!e->index && (a->pack_file_page+1)*12<a->pack->manifest.file_count) { a->pack_file_page++; files(a); }
        else preview(a);
    } else if(e->id==CP_PACK_CONFIRM_REMOVE && e->type==DU_EV_DIALOG && !e->index) {
        (void)a->host.pack_remove(a->host.ctx,&a->pack_device,a->pack_reason); message(a,a->pack_reason);
    }
    return 1;
}
