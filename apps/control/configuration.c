/* Offline configuration through the same codecs used by Control Panel:
 * desktop settings, libreg and f2-100 NETWORK.INI. All destinations come
 * from the caller's store; no implicit C: or live-service activation.
 * SPDX-License-Identifier: MIT */
#include "configuration.h"
#include "../desktop/settings.h"
#include "../desktop/catalog.h"
#include "../../sdk/libreg/internal.h"
#include <ciuki/tzrule.h>
#include <ciuki/tzdata.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_LIMIT REG_TEXT_MAX
static int directory(const struct cp_configuration_store *s,const char *p)
{ int e=s->mkdir(s->ctx,p); return e==-EEXIST ? 0 : e; }
static int string_value(struct rt *t,const char *key,const char *name,const char *value)
{ return reg_put(t,key,name,REG_STRING,value,strlen(value)+1) ? -errno : 0; }
static int number_value(struct rt *t,const char *key,const char *name,int32_t value)
{ return reg_put(t,key,name,REG_INT32,&value,sizeof(value)) ? -errno : 0; }
static int registry(const struct cp_configuration_store *s,const char *root,const struct setup_answers *a)
{
    char path[80]; snprintf(path,sizeof(path),"System/Registry/%s.reg",root);
    char *text=malloc(CONFIG_LIMIT); if (!text) return -ENOMEM;
    unsigned n=0; int e=s->read(s->ctx,path,text,CONFIG_LIMIT,&n);
    struct rt *t=e==-ENOENT ? reg_tree_new(root) : e ? NULL : reg_parse(root,text,n);
    free(text);
    if (!t) return e && e!=-ENOENT ? e : -(errno ? errno : EINVAL);
    e=0;
    if (!strcmp(root,"System")) {
        e=string_value(t,"System\\Identity","UserName",a->user_name);
        if (!e) e=string_value(t,"System\\Identity","ComputerName",a->computer_name);
        if (!e) e=number_value(t,"System\\TimeZone","OffsetMinutes",a->tz_minutes);
        if (!e) e=number_value(t,"System\\TimeZone","RTCLocal",a->clock==SETUP_CLOCK_LOCAL);
        /* G1: the zone and its POSIX rule (clock.cfg is the kernel's copy). */
        int zone=ciuki_tz_find(a->tz_zone);
        char rule[TZR_TEXT]; if (zone<0) tzr_fixed_text(a->tz_minutes,rule,sizeof(rule));
        if (!e) e=string_value(t,"System\\TimeZone","Zone",zone>=0 ? ciuki_tz_zones[zone].id : "");
        if (!e) e=string_value(t,"System\\TimeZone","TZ",zone>=0 ? ciuki_tz_zones[zone].tz : rule);
        if (!e) e=number_value(t,"System\\TimeZone","AutoDST",1);
    } else {
        static const char *const packages[]={"wolf3d-shareware","doom-shareware","quake-shareware","war2-demo","duke3d-shareware"};
        for (unsigned i=0;!e && i<DESK_GAME_COUNT;i++) {
            char key[96]; snprintf(key,sizeof(key),"Programs\\BundledGames\\%s",packages[i]);
            e=number_value(t,key,"Installed",!!(a->components&SETUP_COMP_GAMES));
            if (!e) e=string_value(t,key,"Name",desk_games[i].label);
            if (!e) e=string_value(t,key,"Command",desk_games[i].command);
            if (!e) e=string_value(t,key,"Image","C:\\Games\\CIUKIGAM.IMG");
        }
    }
    if (!e) {
        if (t->generation==UINT64_MAX) e=-EOVERFLOW;
        else {
            t->generation++;
            size_t bytes=0; text=reg_serialise(t,NULL,&bytes);
            if (!text) e=-errno;
            else { e=bytes>CONFIG_LIMIT ? -E2BIG : s->write(s->ctx,path,text,(unsigned)bytes); free(text); }
        }
    }
    reg_tree_free(t); return e;
}
static int desktop(const struct cp_configuration_store *s,const struct setup_answers *a)
{
    struct ciuki_settings *cfg=malloc(sizeof(*cfg)); char *text=malloc(CFG_TEXT_MAX);
    if (!cfg || !text) { free(cfg); free(text); return -ENOMEM; }
    cfg_defaults(cfg); unsigned n=0;
    int e=s->read(s->ctx,"system/settings/desktop.cfg",text,CFG_TEXT_MAX,&n);
    if (!e) cfg_parse(cfg,text,n); else if (e==-ENOENT) e=0;
    if (!e) {
        cfg->keyboard=!strcmp(a->keyboard,"it") ? CFG_KEYS_IT : CFG_KEYS_US;
        size_t bytes=cfg_format(cfg,text,CFG_TEXT_MAX);
        e=bytes ? s->write(s->ctx,"system/settings/desktop.cfg",text,(unsigned)bytes) : -E2BIG;
    }
    free(cfg); free(text); return e;
}
static int network(const struct cp_configuration_store *s,const struct setup_answers *a)
{
    struct ciuki_net_config c[2]; struct net_config_identity ids[2]={0};
    int e=s->network(s->ctx,ids); if (e) return e;
    for (unsigned i=0;i<2;i++) {
        net_config_default(&c[i]); snprintf(c[i].hostname,sizeof(c[i].hostname),"%s",a->computer_name);
        c[i].enabled=a->network!=SETUP_NET_NONE;
        if (a->network==SETUP_NET_STATIC && i==0 && ids[i].vendor) {
            c[i].dhcp=0; memcpy(&c[i].address,a->ip,4); memcpy(&c[i].gateway,a->gateway,4); memcpy(&c[i].dns[0],a->dns,4);
            uint8_t mask[4]; for (unsigned j=0;j<4;j++) mask[j]=(uint8_t)(0xffu << (a->prefix>j*8 ? (a->prefix-j*8>=8 ? 0 : 8-a->prefix+j*8) : 8));
            memcpy(&c[i].mask,mask,4);
        }
    }
    char text[NET_SETTINGS_BYTES]; size_t n=net_config_format(c,ids,text,sizeof(text));
    return n ? s->write(s->ctx,"SYSTEM/NETWORK.INI",text,(unsigned)n) : -EINVAL;
}
int cp_configuration_write(const struct cp_configuration_store *s,const struct setup_answers *a)
{
    if (!s || !a || !s->read || !s->write || !s->mkdir || !s->network || !s->clock || !s->now) return -EINVAL;
    static const char *const dirs[]={"System","system/settings","System/Registry"};
    int e=0; for (unsigned i=0;!e && i<3;i++) e=directory(s,dirs[i]);
    if (!e) e=desktop(s,a);
    if (!e) e=registry(s,"System",a);
    if (!e) e=registry(s,"Programs",a);
    if (!e) e=network(s,a);
    /* G1: clock.cfg version 2 (lib/tzrule.c): the zone's rule, automatic
     * daylight saving, and the offset of the local time written to the RTC
     * now (rtc_bias_minutes), so boot reads the RTC back exactly. */
    int64_t seconds=0; int32_t offset=a->tz_minutes;
    if (!e) {
        seconds=s->now(s->ctx);
        if ((a->clock_adjust_s>0 && seconds>INT64_MAX-a->clock_adjust_s) ||
            (a->clock_adjust_s<0 && seconds<INT64_MIN-a->clock_adjust_s)) return -EOVERFLOW;
        seconds+=a->clock_adjust_s;
        struct tzr_config c; tzr_config_default(&c);
        int zone=ciuki_tz_find(a->tz_zone);
        c.rtc_local=a->clock==SETUP_CLOCK_LOCAL; c.auto_dst=true; c.offset_minutes=a->tz_minutes;
        if (zone>=0) { snprintf(c.zone,sizeof(c.zone),"%s",ciuki_tz_zones[zone].id); snprintf(c.tz,sizeof(c.tz),"%s",ciuki_tz_zones[zone].tz); }
        else tzr_fixed_text(a->tz_minutes,c.tz,sizeof(c.tz));
        struct tzr r;
        if (!tzr_config_rule(&c,&r)) return -EINVAL;
        offset=tzr_offset(&r,seconds,0)/60;
        c.rtc_bias_minutes=c.rtc_local ? offset : 0;
        char text[512]; size_t n=tzr_config_format(&c,text,sizeof(text));
        e=n ? s->write(s->ctx,"system/settings/clock.cfg",text,(unsigned)n) : -E2BIG;
    }
    if (!e) e=s->clock(s->ctx,seconds,offset,a->clock==SETUP_CLOCK_LOCAL);
    return e;
}
