/* SPDX-License-Identifier: MIT
 * Open/close per operation: no long-lived registry or volume handles. */
#include "autoplay.h"
#include "../../sdk/sysroot-overlay/include/ciuki/registry.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static struct reg_root *root(const char *dir) { return reg_open_root_at("Desktop",dir ? dir : REG_DIRECTORY); }
static int key(const char *id,char out[96])
{
    if (!id || !*id || strlen(id)>=40) { errno=EINVAL; return -1; }
    for (const char *p=id;*p;p++) if (!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-')) { errno=EINVAL; return -1; }
    snprintf(out,96,AP_REG_KEY "\\%s",id); return 0;
}
int ap_preference_get(const char *dir,const char *id)
{
    char path[96]; if (key(id,path)) return -1;
    struct reg_root *r=root(dir); if (!r) return -1;
    int32_t action=-1; int e=reg_get_int32(r,path,"Action",&action); reg_close_root(r);
    return !e && action>=0 && action<AP_CHOICES ? action : -1;
}
int ap_preference_set(const char *dir,const char *id,const char *label,int action)
{
    char path[96]; if (key(id,path) || action<0 || action>=AP_CHOICES) { errno=EINVAL; return -1; }
    struct reg_root *r=root(dir); if (!r) return -1;
    int e=reg_create_key(r,path);
    if (!e) e=reg_set_string(r,path,"Label",label ? label : "Removable media");
    if (!e) e=reg_set_int32(r,path,"Action",action);
    reg_close_root(r); return e;
}
int ap_preference_list(const char *dir,struct ap_preference *out,unsigned capacity)
{
    struct reg_root *r=root(dir); if (!r) return -1;
    unsigned used=0;
    for (unsigned i=0;used<capacity;i++) {
        char id[40],path[96];
        if (reg_enum_keys(r,AP_REG_KEY,i,id,sizeof(id))) { if (errno==ENOENT) break; reg_close_root(r); return -1; }
        if (key(id,path)) continue;
        int32_t action;
        if (reg_get_int32(r,path,"Action",&action) || action<0 || action>=AP_CHOICES) continue;
        struct ap_preference *p=&out[used++]; memset(p,0,sizeof(*p));
        snprintf(p->id,sizeof(p->id),"%s",id); p->action=action;
        size_t size=sizeof(p->label); if (reg_get_string(r,path,"Label",p->label,&size)) snprintf(p->label,sizeof(p->label),"%s",id);
    }
    reg_close_root(r); return (int)used;
}
int ap_preference_forget(const char *dir,const char *id)
{
    char path[96]; if (key(id,path)) return -1;
    struct reg_root *r=root(dir); if (!r) return -1;
    int e=reg_delete(r,path,NULL); reg_close_root(r); return e;
}
