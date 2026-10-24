/* SPDX-License-Identifier: MIT
 * Shared native/Win32 appearance value. No I/O, allocation or host ABI types. */
#ifndef CIUKI_APPEARANCE_H
#define CIUKI_APPEARANCE_H
#include <stdint.h>
enum {
    AP_DESKTOP, AP_ACTIVE_TITLE, AP_INACTIVE_TITLE, AP_MENU, AP_MESSAGE,
    AP_ICON, AP_ICON_X, AP_ICON_Y, AP_SCROLLBAR, AP_SELECTED, AP_TOOLTIP,
    AP_WINDOW, AP_ACTIVE_BORDER, AP_INACTIVE_BORDER, AP_3D, AP_DISABLED,
    AP_SMALL_TITLE, AP_PALETTE, AP_WORKSPACE, AP_HOT, AP_ITEMS
};
enum { AP_LARGE_ICONS=1, AP_FULL_DRAG=2, AP_SMOOTH=4, AP_MENU_ANIMATION=8 };
#define AP_SCHEMES 8
#define AP_BUILTINS 6
struct ap_item { uint32_t colour, gradient, text; uint8_t size, font, points, style; };
struct appearance { uint16_t dpi, effects; struct ap_item item[AP_ITEMS]; };
struct ap_scheme { char name[32]; struct appearance value; };
#define AP_HEX_BYTES (8 + AP_ITEMS * 32)
static inline int ap_scale(const struct appearance *a,int x) { return (x*(int)a->dpi+48)/96; }
static inline int ap_name_equal(const char *a,const char *b) {
    for(;;a++,b++) {
        unsigned x=(unsigned char)*a,y=(unsigned char)*b;
        if(x>='A' && x<='Z') x+=32;
        if(y>='A' && y<='Z') y+=32;
        if(x!=y) return 0;
        if(!x) return 1;
    }
}
static inline const char *ap_item_name(int n) {
    static const char *const names[AP_ITEMS]={"Desktop","Active title bar","Inactive title bar","Menu",
        "Message box","Icon","Icon spacing (horizontal)","Icon spacing (vertical)","Scrollbar",
        "Selected items","Tooltip","Window","Active window border","Inactive window border",
        "3D objects","Disabled items","Small caption","Palette title","Application background","Hot tracking"};
    return n>=0 && n<AP_ITEMS ? names[n] : "";
}
static inline const char *ap_builtin_name(int n) {
    static const char *const names[AP_BUILTINS]={"Windows Standard","High Contrast Black","High Contrast White",
        "Desert","Rainy Day","Ciuki Blue"};
    return n>=0 && n<AP_BUILTINS ? names[n] : "";
}
static inline void ap_default(struct appearance *a,int scheme) {
    a->dpi=96; a->effects=AP_FULL_DRAG|AP_SMOOTH|AP_MENU_ANIMATION;
    for(int i=0;i<AP_ITEMS;i++) a->item[i]=(struct ap_item){0xc0c0c0,0xc0c0c0,0,18,0,9,0};
    a->item[AP_DESKTOP].colour=0x008080;
    a->item[AP_ACTIVE_TITLE]=(struct ap_item){0x000080,0x1084d0,0xffffff,24,1,10,1};
    a->item[AP_INACTIVE_TITLE]=(struct ap_item){0x808080,0xb5b5b5,0xc0c0c0,24,1,10,1};
    a->item[AP_WINDOW].colour=a->item[AP_MESSAGE].colour=0xffffff;
    a->item[AP_ICON]=(struct ap_item){0x008080,0x008080,0xffffff,32,0,9,0};
    a->item[AP_ICON_X].size=96; a->item[AP_ICON_Y].size=94;
    a->item[AP_SCROLLBAR].size=16;
    a->item[AP_SELECTED]=(struct ap_item){0x000080,0x000080,0xffffff,18,0,9,0};
    a->item[AP_TOOLTIP]=(struct ap_item){0xffffe1,0xffffe1,0,18,0,9,0};
    a->item[AP_ACTIVE_BORDER].size=a->item[AP_INACTIVE_BORDER].size=3;
    a->item[AP_DISABLED].text=0x808080;
    a->item[AP_HOT].text=0x0000ff;
    if(scheme==1 || scheme==2) {
        uint32_t bg=scheme==1 ? 0 : 0xffffff, fg=bg^0xffffff;
        for(int i=0;i<AP_ITEMS;i++) { a->item[i].colour=a->item[i].gradient=bg; a->item[i].text=fg; }
        a->item[AP_SELECTED].colour=a->item[AP_ACTIVE_TITLE].colour=scheme==1 ? 0x800080 : 0x000080;
        a->item[AP_ACTIVE_TITLE].gradient=a->item[AP_ACTIVE_TITLE].colour;
        a->item[AP_SELECTED].text=a->item[AP_ACTIVE_TITLE].text=0xffffff;
        a->effects&=~AP_SMOOTH;
    } else if(scheme==3 || scheme==4 || scheme==5) {
        uint32_t face=scheme==3 ? 0xd8d0b0 : scheme==4 ? 0xaebfc5 : 0xdce4ec;
        a->item[AP_MENU].colour=a->item[AP_3D].colour=face;
        a->item[AP_ACTIVE_TITLE].colour=scheme==3 ? 0x806040 : scheme==4 ? 0x405860 : 0x173f70;
        a->item[AP_ACTIVE_TITLE].gradient=scheme==3 ? 0xc0a880 : scheme==4 ? 0x90b0b8 : 0x3f719d;
    }
}
static inline int ap_size_min(int i) {
    return i==AP_ACTIVE_TITLE || i==AP_INACTIVE_TITLE ? 18 : i==AP_SCROLLBAR ? 8 :
        i==AP_ICON ? 16 : i==AP_ICON_X || i==AP_ICON_Y ? 32 : 1;
}
static inline int ap_size_max(int i) {
    return i==AP_ACTIVE_BORDER || i==AP_INACTIVE_BORDER ? 8 : i==AP_ICON_X || i==AP_ICON_Y ? 255 : 128;
}
static inline int ap_valid(const struct appearance *a) {
    if(a->dpi<48 || a->dpi>240 || (a->effects&~15u)) return 0;
    for(int i=0;i<AP_ITEMS;i++) {
        const struct ap_item *v=&a->item[i];
        if((v->colour|v->gradient|v->text)>0xffffff || v->size<ap_size_min(i) || v->size>ap_size_max(i) || v->font>6 || v->points<6 || v->points>36 || v->style>3) return 0;
    }
    return 1;
}
static inline void ap_hexword(char *p,uint32_t v,int digits) {
    const char *hex="0123456789abcdef";
    for(int i=digits-1;i>=0;i--) { p[i]=hex[v&15]; v>>=4; }
}
static inline void ap_encode(const struct appearance *a,char out[AP_HEX_BYTES+1]) {
    ap_hexword(out,a->dpi,4); ap_hexword(out+4,a->effects,4);
    for(int i=0;i<AP_ITEMS;i++) {
        char *p=out+8+i*32; const struct ap_item *v=&a->item[i];
        ap_hexword(p,v->colour,8); ap_hexword(p+8,v->gradient,8); ap_hexword(p+16,v->text,8);
        ap_hexword(p+24,v->size,2); ap_hexword(p+26,v->font,2); ap_hexword(p+28,v->points,2); ap_hexword(p+30,v->style,2);
    }
    out[AP_HEX_BYTES]=0;
}
static inline int ap_unhex(const char *p,int digits,uint32_t *v) {
    *v=0;
    for(int i=0;i<digits;i++) {
        unsigned c=(unsigned char)p[i], d=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : 16;
        if(d>15) return 0;
        *v=(*v<<4)|d;
    }
    return 1;
}
static inline int ap_decode(struct appearance *a,const char *p) {
    struct appearance next; uint32_t v;
    for(int i=0;i<AP_HEX_BYTES;i++) if(!p[i]) return 0;
    if(p[AP_HEX_BYTES] || !ap_unhex(p,4,&v)) return 0;
    next.dpi=(uint16_t)v; if(!ap_unhex(p+4,4,&v)) return 0; next.effects=(uint16_t)v;
    for(int i=0;i<AP_ITEMS;i++) {
        struct ap_item *t=&next.item[i]; const char *s=p+8+i*32;
        if(!ap_unhex(s,8,&t->colour) || !ap_unhex(s+8,8,&t->gradient) || !ap_unhex(s+16,8,&t->text)) return 0;
        if(!ap_unhex(s+24,2,&v)) return 0;
        t->size=(uint8_t)v;
        if(!ap_unhex(s+26,2,&v)) return 0;
        t->font=(uint8_t)v;
        if(!ap_unhex(s+28,2,&v)) return 0;
        t->points=(uint8_t)v;
        if(!ap_unhex(s+30,2,&v)) return 0;
        t->style=(uint8_t)v;
    }
    if(!ap_valid(&next)) return 0;
    *a=next; return 1;
}
/* The same whitespace/CRLF rules as desktop.cfg. Last valid value wins. */
static inline int ap_settings_decode(struct appearance *out,const char *text,unsigned bytes) {
    int found=0;
    for(unsigned at=0;at<bytes;) {
        unsigned start=at; while(at<bytes && text[at]!='\n') at++;
        unsigned end=at; if(at<bytes) at++;
        while(start<end && (text[start]==' ' || text[start]=='\t')) start++;
        const char *key="appearance"; unsigned i=0;
        while(i<10 && start+i<end && text[start+i]==key[i]) i++;
        if(i!=10) continue;
        start+=10;
        while(start<end && (text[start]==' ' || text[start]=='\t')) start++;
        if(start==end || text[start++]!='=') continue;
        while(start<end && (text[start]==' ' || text[start]=='\t')) start++;
        while(end>start && (text[end-1]==' ' || text[end-1]=='\t' || text[end-1]=='\r')) end--;
        if(end-start!=AP_HEX_BYTES) continue;
        char value[AP_HEX_BYTES+1];
        for(i=0;i<AP_HEX_BYTES;i++) value[i]=text[start+i];
        value[AP_HEX_BYTES]=0;
        if(ap_decode(out,value)) found=1;
    }
    return found;
}
#endif
