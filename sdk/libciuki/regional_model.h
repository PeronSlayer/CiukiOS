/* G24: shared, private regional formatting model. No wire or SDK ABI.
 * Pure bounded operations; callers own I/O. SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGIONAL_MODEL_H
#define CIUKI_REGIONAL_MODEL_H
#include <stddef.h>
#define REG_FILE "/system/settings/desktop.cfg"
#define REG_VALUE 48
enum { REG_DECIMAL, REG_THOUSAND, REG_DIGITS, REG_NEGATIVE, REG_GROUPING,
       REG_SYMBOL, REG_MON_DECIMAL, REG_MON_THOUSAND, REG_MON_DIGITS,
       REG_MON_NEGATIVE, REG_MON_POSITIVE, REG_DATE, REG_LONG_DATE, REG_TIME,
       REG_AM, REG_PM, REG_MEASURE, REG_FIELDS };
struct regional { int italian; char value[REG_FIELDS][REG_VALUE]; };
static const char *const reg_keys[REG_FIELDS] = {
    "decimal", "thousand", "digits", "negative", "grouping", "currency",
    "mon_decimal", "mon_thousand", "mon_digits", "mon_negative", "mon_positive",
    "date", "long_date", "time", "am", "pm", "measure"
};
static inline size_t reg_len(const char *s) { size_t n=0; while(s[n]) n++; return n; }
static inline int reg_eq(const char *a,const char *b)
{ while (*a && *a==*b) { a++; b++; } return *a==*b; }
static inline void reg_copy(char *to,const char *from)
{ while ((*to++=*from++)) {} }
static inline const char *reg_locale(const struct regional *r) { return r->italian ? "it_IT" : "en_US"; }
static inline void reg_defaults(struct regional *r,int italian)
{
    static const char *const us[REG_FIELDS]={".",",","2","1","3","$",".",",","2","0","0",
        "M/d/yy","dddd, MMMM dd, yyyy","h:mm:ss tt","AM","PM","1"};
    static const char *const it[REG_FIELDS]={",",".","2","1","3","EUR",",",".","2","8","3",
        "dd/MM/yyyy","dddd d MMMM yyyy","HH:mm:ss","","","0"};
    r->italian=!!italian;
    for(unsigned i=0;i<REG_FIELDS;i++) reg_copy(r->value[i],italian ? it[i] : us[i]);
}
/* Canonical locale names; UTF-8 aliases select formatting only, not a codec. */
static inline int reg_id(const char *s)
{
    if(reg_eq(s,"C") || reg_eq(s,"POSIX")) return 0;
    if(reg_eq(s,"en_US") || reg_eq(s,"en_US.UTF-8")) return 1;
    if(reg_eq(s,"it_IT") || reg_eq(s,"it_IT.UTF-8")) return 2;
    return -1;
}
static inline int reg_order(const char *s,unsigned max)
{
    unsigned n=0;
    if(!*s) return 0;
    for(;*s;s++) { if(*s<'0'||*s>'9') return 0; n=n*10+(unsigned)(*s-'0'); if(n>max) return 0; }
    return 1;
}
static inline int reg_picture(const char *s,int time)
{
    int quote=0; unsigned seen=0;
    for(;*s;s++) {
        if(*s=='\'') { if(s[1]=='\'') s++; else quote=!quote; continue; }
        if(quote) continue;
        char c=*s;
        if(time && (c=='H'||c=='h')) seen|=1;
        else if(time && c=='m') seen|=2;
        else if(!time && c=='d') seen|=1;
        else if(!time && c=='M') seen|=2;
        else if(!time && c=='y') seen|=4;
        else if(time && (c=='s'||c=='t')) {}
        else if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='%') return 0;
    }
    return !quote && seen==(time ? 3u : 7u);
}
static inline int reg_valid(unsigned i,const char *v)
{
    size_t n=reg_len(v); if(n>=REG_VALUE) return 0;
    for(size_t j=0;j<n;j++) if((unsigned char)v[j]<32 || (unsigned char)v[j]>126) return 0;
    switch(i) {
    case REG_DECIMAL: case REG_MON_DECIMAL: return n==1 && (v[0]=='.'||v[0]==',');
    case REG_THOUSAND: case REG_MON_THOUSAND: return n<=1 && (!n||v[0]=='.'||v[0]==','||v[0]==' ');
    case REG_DIGITS: case REG_MON_DIGITS: case REG_GROUPING: return n==1 && reg_order(v,9);
    case REG_NEGATIVE: return n==1 && reg_order(v,4);
    case REG_MON_NEGATIVE: return n<=2 && reg_order(v,15);
    case REG_MON_POSITIVE: return n==1 && reg_order(v,3);
    case REG_MEASURE: return n==1 && reg_order(v,1);
    case REG_SYMBOL: return n>0 && n<12;
    case REG_AM: case REG_PM: return n<12;
    case REG_DATE: case REG_LONG_DATE: return reg_picture(v,0);
    case REG_TIME: return reg_picture(v,1);
    default: return 0;
    }
}
static inline int reg_consistent(const struct regional *r)
{
    for(unsigned i=0;i<REG_FIELDS;i++) if(!reg_valid(i,r->value[i])) return 0;
    return !reg_eq(r->value[REG_DECIMAL],r->value[REG_THOUSAND]) &&
           !reg_eq(r->value[REG_MON_DECIMAL],r->value[REG_MON_THOUSAND]);
}
static inline unsigned reg_run(const char *p,char token)
{
    int quote=0;
    for(;*p;p++) {
        if(*p=='\'') { if(p[1]=='\'') p++; else quote=!quote; }
        else if(!quote && *p==token) { unsigned n=0; while(*p++==token) n++; return n; }
    }
    return 0;
}
static inline const char *reg_separator(const char *p)
{
    int quote=0;
    for(;*p;p++) {
        if(*p=='\'') { if(p[1]=='\'') p++; else quote=!quote; }
        else if(!quote) switch(*p) {
        case '/': return "/"; case '-': return "-"; case '.': return "."; case ':': return ":"; case ' ': return " ";
        }
    }
    return "";
}
static inline const char *reg_date_order(const char *p)
{
    int quote=0;
    for(;*p;p++) {
        if(*p=='\'') { if(p[1]=='\'') p++; else quote=!quote; }
        else if(!quote) {
            if(*p=='M') return "0";
            if(*p=='d') {
                unsigned n=0; while(p[n]=='d') n++;
                if(n<3) return "1"; /* a weekday name is not the day field */
                p+=n-1;
            }
            if(*p=='y') return "2";
        }
    }
    return "0";
}
/* Returns -1 for an unknown key, 0 invalid, 1 assigned. Locale appears first
 * in canonical files; changing it resets its defaults before overrides. */
static inline int reg_assign(struct regional *r,const char *key,const char *v)
{
    if(reg_eq(key,"regional_locale")) {
        int id=reg_id(v); if(id<1) return 0; reg_defaults(r,id==2); return 1;
    }
    for(unsigned i=0;i<REG_FIELDS;i++) {
        char name[40]="regional_"; reg_copy(name+9,reg_keys[i]);
        if(reg_eq(key,name)) {
            if((i==REG_THOUSAND||i==REG_MON_THOUSAND) && reg_eq(v,"\\s")) v=" ";
            if(!reg_valid(i,v)) return 0; reg_copy(r->value[i],v); return 1;
        }
    }
    return -1;
}
/* Feed one complete line. Overlong lines must be discarded by the reader. */
static inline void reg_line(struct regional *r,char *line)
{
    while(*line==' '||*line=='\t') line++;
    char *p=line; while(*p && *p!='=') p++;
    if(*p) {
        char *end=p; while(end>line && (end[-1]==' '||end[-1]=='\t')) end--;
        *end=0; p++;
        while(*p==' '||*p=='\t') p++;
        size_t n=reg_len(p); while(n && (p[n-1]=='\r'||p[n-1]==' '||p[n-1]=='\t')) p[--n]=0;
        reg_assign(r,line,p);
    }
}
static inline void reg_sanitize(struct regional *r)
{
    if(reg_eq(r->value[REG_DECIMAL],r->value[REG_THOUSAND])) r->value[REG_THOUSAND][0]=0;
    if(reg_eq(r->value[REG_MON_DECIMAL],r->value[REG_MON_THOUSAND])) r->value[REG_MON_THOUSAND][0]=0;
}
/* Convert the supported Windows pictures to strftime without ever allowing
 * a user '%' to inject recursive %x/%X/%c directives. */
static inline void reg_strftime_picture(const char *s,char out[192])
{
    size_t used=0; int quote=0;
    while(*s && used<188) {
        char c=*s++; unsigned n=1;
        if(c=='\'') { if(*s=='\'') { out[used++]='\''; s++; } else quote=!quote; continue; }
        if(!quote) while(*s==c) { n++; s++; }
        char spec=0;
        if(!quote) switch(c) {
        case 'd': spec=n<3?'d':n==3?'a':'A'; break;
        case 'M': spec=n<3?'m':n==3?'b':'B'; break;
        case 'y': spec=n<3?'y':'Y'; break;
        case 'H': spec='H'; break; case 'h': spec='I'; break;
        case 'm': spec='M'; break; case 's': spec='S'; break; case 't': spec=n==1?'v':'p'; break;
        }
        if(spec) { out[used++]='%'; if(n==1 && c!='t') out[used++]='-'; out[used++]=spec; }
        else { if(c=='%') out[used++]='%'; out[used++]=c; }
    }
    out[used]=0;
}
#endif
