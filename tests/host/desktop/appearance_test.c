/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "theme.h"
static struct ciuki_settings a,b;
static char text[CFG_TEXT_MAX],hex[AP_HEX_BYTES+1];
static uint32_t pixels[512*100];
int main(void)
{
    cfg_defaults(&a);
    for(int scheme=0;scheme<AP_BUILTINS;scheme++) {
        assert(!cfg_scheme_use(&a,ap_builtin_name(scheme)));
        assert(ap_valid(&a.appearance));
        ap_encode(&a.appearance,hex); struct appearance decoded;
        assert(ap_decode(&decoded,hex) && !memcmp(&decoded,&a.appearance,sizeof(decoded)));
    }
    assert(!cfg_scheme_use(&a,"Windows Standard"));
    for(int item=0;item<AP_ITEMS;item++) {
        struct ap_item *v=&a.appearance.item[item];
        v->colour=(unsigned)item*0x010203; v->gradient=v->colour^0xffffff;
        v->text=0x123456; v->size=(uint8_t)ap_size_min(item); v->font=(uint8_t)(item%7); v->points=(uint8_t)(6+item); v->style=(uint8_t)(item%4);
    }
    a.appearance.dpi=120; a.appearance.effects=AP_LARGE_ICONS;
    assert(!cfg_scheme_save(&a,"Test Scheme"));
    a.appearance.item[AP_MENU].text=0xaabbcc;
    assert(!cfg_scheme_save(&a,"Test Scheme"));
    assert(!cfg_scheme_use(&a,"test scheme"));
    assert(a.appearance.item[AP_MENU].text==0xaabbcc);
    size_t n=cfg_format(&a,text,sizeof(text)); assert(n);
    cfg_defaults(&b); assert(!cfg_parse(&b,text,n));
    assert(!memcmp(&a.appearance,&b.appearance,sizeof(a.appearance)));
    assert(!strcmp(b.schemes[0].name,"Test Scheme"));
    assert(cfg_scheme_save(&b,"Windows Standard"));
    assert(cfg_scheme_save(&b,"bad|name"));
    assert(cfg_scheme_delete(&b,"High Contrast Black"));
    assert(!cfg_scheme_delete(&b,"Test Scheme")); assert(cfg_scheme_use(&b,"Test Scheme"));
    for(int i=0;i<AP_SCHEMES;i++) { char name[32]; snprintf(name,sizeof(name),"Scheme %d",i); assert(!cfg_scheme_save(&b,name)); }
    assert(cfg_scheme_save(&b,"No room"));
    ap_encode(&a.appearance,hex);
    int line=snprintf(text,sizeof(text),"  appearance \t= %s \r\n",hex);
    struct appearance whitespace;
    assert(ap_settings_decode(&whitespace,text,(unsigned)line) && !memcmp(&whitespace,&a.appearance,sizeof(whitespace)));
    ap_encode(&a.appearance,hex); hex[0]='g'; assert(!ap_decode(&b.appearance,hex));
    ap_encode(&a.appearance,hex); hex[10]=0; assert(!ap_decode(&b.appearance,hex));
    struct sound_scheme sounds={.enabled=1,.volume=75}, restored={0};
    strcpy(sounds.file[0],"startup.wav");
    strcpy(a.wallpaper,"pattern:3"); a.wallpaper_mode=CFG_WALL_TILE; a.saver_kind=2; a.saver_seconds=120;
    a.document_icon=a.recycle_icon=1; a.pointer_scheme=2; a.appearance.effects|=AP_LARGE_ICONS;
    assert(!cfg_desktop_theme_save(&a,"My Desktop",&sounds));
    n=cfg_format(&a,text,sizeof(text)); assert(n);
    cfg_defaults(&b); assert(!cfg_parse(&b,text,n));
    assert(!cfg_desktop_theme_use(&b,"my desktop",&restored));
    assert(!strcmp(b.wallpaper,"pattern:3") && b.wallpaper_mode==CFG_WALL_TILE);
    assert(b.saver_kind==2 && b.saver_seconds==120 && b.pointer_scheme==2 && b.document_icon==1 && b.recycle_icon==1);
    assert(restored.enabled && restored.volume==75 && !strcmp(restored.file[0],"startup.wav"));
    assert(!memcmp(&a.appearance,&b.appearance,sizeof(a.appearance)));
    assert(!cfg_desktop_theme_delete(&b,"My Desktop") && cfg_desktop_theme_use(&b,"My Desktop",&restored));
    assert(!cfg_scheme_use(&a,"Windows Standard"));
    assert(!desk_theme_use("Ciuki Blue")); desk_appearance_apply(&a.appearance);
    assert(desk_theme->title[0]==0x000080 && desk_theme->title[1]==0x1084d0);
    assert(desk_metric(AP_SCROLLBAR,0)==16);
    int width=desk_text_measure(desk_item_font(AP_MESSAGE),"Scale and hit targets");
    a.appearance.dpi=120; desk_appearance_apply(&a.appearance);
    assert(desk_metric(AP_SCROLLBAR,0)==20);
    assert(desk_text_measure(desk_item_font(AP_MESSAGE),"Scale and hit targets")>width);
    desk_theme_logical(1); assert(desk_metric(AP_SCROLLBAR,0)==16);
    assert(desk_text_measure(desk_item_font(AP_MESSAGE),"Scale and hit targets")==width);
    desk_theme_logical(0);
    a.appearance.effects=AP_FULL_DRAG; desk_appearance_apply(&a.appearance); assert(!desk_theme->text_aa);
    struct desk_paint p={pixels,512,100,0,0,512,100};
    for(int item=0;item<AP_ITEMS;item++) desk_paint_text(&p,desk_item_font(item),0,30,ap_item_name(item),0xabcdef);
    puts("PASS appearance: every item, palette/gradient/fonts/sizes/styles, custom DPI, effects, named save/replace/delete, catalogue bounds and persistence");
    fflush(stdout);
    return 0;
}
