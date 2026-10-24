/* G24: formatting locales for the newlib CiukiOS port. The private table
 * types are from the pinned upstream setlocale.h, never a duplicated ABI.
 * The C locale is the initial state as required by C/POSIX; programs opt
 * into environment/system preferences with setlocale(LC_ALL, "").
 * SPDX-License-Identifier: MIT */
#include <reent.h>
#include <errno.h>
#include <stdio.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "setlocale.h" /* upstream inline fallbacks have unused parameters */
#pragma GCC diagnostic pop
#include "regional_model.h"

extern struct __locale_t __global_locale;
static int selected[_LC_LAST];
static struct regional numeric,monetary,times;
static struct lc_numeric_T numeric_table;
static struct lc_time_T time_table;
static char date_picture[192],time_picture[192],long_picture[192],combined_picture[400];
static char all_names[_LC_LAST*(ENCODING_LEN+1)];

/* Small bounded streaming read; a failed/oversized file yields defaults. */
static void system_region(struct regional *r)
{
    reg_defaults(r,0);
    int saved=errno;
    FILE *f=fopen(REG_FILE,"rb");
    if(f) {
        char line[160]; unsigned used=0,total=0; int c,long_line=0;
        while((c=fgetc(f))!=EOF && ++total<=32768) {
            if(c=='\n') {
                if(!long_line) { line[used]=0; reg_line(r,line); }
                used=0; long_line=0;
            } else if(used+1<sizeof(line)) line[used++]=(char)c; else long_line=1;
        }
        if(total>32768 || ferror(f)) reg_defaults(r,0);
        else if(used && !long_line) { line[used]=0; reg_line(r,line); }
        fclose(f);
    }
    reg_sanitize(r); errno=saved;
}
static const char *locale_name(int id) { return id==2?"it_IT":id==1?"en_US":"C"; }
static const char *environment(int category)
{
    static const char *const vars[]={"LC_ALL","LC_COLLATE","LC_CTYPE","LC_MONETARY","LC_NUMERIC","LC_TIME","LC_MESSAGES"};
    const char *s=getenv("LC_ALL"); if(s && *s) return s;
    s=getenv(vars[category]); if(s && *s) return s;
    s=getenv("LANG"); return s && *s ? s : NULL;
}
static void formats(struct regional *out,int id,const struct regional *system)
{
    if(id && id==(system->italian?2:1)) *out=*system;
    else reg_defaults(out,id==2);
}
/* localeconv, printf and scanf share newlib's global lconv record. */
static void update_time(void);
static void update_lconv(void)
{
    struct lconv *l=&__global_locale.lconv;
    l->decimal_point=selected[LC_NUMERIC]?numeric.value[REG_DECIMAL]:".";
    l->thousands_sep=selected[LC_NUMERIC]?numeric.value[REG_THOUSAND]:"";
    static char grouping[2],mon_grouping[2];
    grouping[0]=selected[LC_NUMERIC]?(char)(numeric.value[REG_GROUPING][0]-'0'):0;
    grouping[1]=0; l->grouping=grouping;
    int on=selected[LC_MONETARY]!=0;
    mon_grouping[0]=on?(char)(monetary.value[REG_GROUPING][0]-'0'):0;
    mon_grouping[1]=0; l->mon_grouping=mon_grouping;
    l->int_curr_symbol=on?(selected[LC_MONETARY]==2?"EUR ":"USD "):"";
    l->currency_symbol=on?monetary.value[REG_SYMBOL]:"";
    l->mon_decimal_point=on?monetary.value[REG_MON_DECIMAL]:"";
    l->mon_thousands_sep=on?monetary.value[REG_MON_THOUSAND]:"";
    l->positive_sign=""; l->negative_sign=on?"-":"";
    l->int_frac_digits=l->frac_digits=on?(char)(monetary.value[REG_MON_DIGITS][0]-'0'):CHAR_MAX;
    unsigned pos=(unsigned)(monetary.value[REG_MON_POSITIVE][0]-'0');
    unsigned neg=0;
    if(on) for(const char *p=monetary.value[REG_MON_NEGATIVE];*p;p++) neg=neg*10+(unsigned)(*p-'0');
    static const char before[16]={1,1,1,1,0,0,0,0,0,1,0,1,1,0,1,0};
    static const char space[16]={0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1};
    static const char sign[16]={0,1,4,2,0,1,3,2,1,1,2,2,4,3,0,0};
    l->p_cs_precedes=on?(char)(pos==0||pos==2):CHAR_MAX;
    l->p_sep_by_space=on?(char)(pos>=2):CHAR_MAX;
    l->n_cs_precedes=on?before[neg]:CHAR_MAX; l->n_sep_by_space=on?space[neg]:CHAR_MAX;
    l->p_sign_posn=on?1:CHAR_MAX; l->n_sign_posn=on?sign[neg]:CHAR_MAX;
    l->int_p_cs_precedes=l->p_cs_precedes; l->int_n_cs_precedes=l->n_cs_precedes;
    l->int_p_sep_by_space=l->p_sep_by_space; l->int_n_sep_by_space=l->n_sep_by_space;
    l->int_p_sign_posn=l->p_sign_posn; l->int_n_sign_posn=l->n_sign_posn;
}
char *__ciuki_setlocale_r(struct _reent *reent,int category,const char *locale)
{
    if(category<LC_ALL||category>=_LC_LAST) { reent->_errno=EINVAL; return NULL; }
    if(locale) {
        struct regional system; system_region(&system);
        int next[_LC_LAST]; memcpy(next,selected,sizeof(next));
        const char *part=locale;
        for(int i=1;i<_LC_LAST;i++) if(category==LC_ALL||category==i) {
            char name[ENCODING_LEN+1]; const char *s=locale;
            if(!*locale) s=environment(i);
            if(category==LC_ALL && *locale && strchr(locale,'/')) {
                size_t n=0; while(*part && *part!='/') { if(n==ENCODING_LEN) goto invalid; name[n++]=*part++; }
                name[n]=0; if(*part=='/') part++; else if(i!=_LC_LAST-1) goto invalid;
                s=name;
            }
            int id=s?reg_id(s):(system.italian?2:1);
            if(id<0) goto invalid;
            next[i]=id;
        }
        if(category==LC_ALL && *locale && strchr(locale,'/') && *part) goto invalid;
        memcpy(selected,next,sizeof(selected));
        for(int i=1;i<_LC_LAST;i++) reg_copy(__global_locale.categories[i],locale_name(selected[i]));
        if(category==LC_ALL||category==LC_NUMERIC) formats(&numeric,selected[LC_NUMERIC],&system);
        if(category==LC_ALL||category==LC_MONETARY) formats(&monetary,selected[LC_MONETARY],&system);
        if(category==LC_ALL||category==LC_TIME) formats(&times,selected[LC_TIME],&system);
        update_lconv();
        numeric_table=(struct lc_numeric_T){numeric.value[REG_DECIMAL],numeric.value[REG_THOUSAND],__global_locale.lconv.grouping};
        update_time();
    }
    if(category!=LC_ALL) return __global_locale.categories[category];
    int same=1; for(int i=2;i<_LC_LAST;i++) same&=selected[i]==selected[1];
    reg_copy(all_names,locale_name(selected[1]));
    if(!same) for(int i=2;i<_LC_LAST;i++) { strcat(all_names,"/"); strcat(all_names,locale_name(selected[i])); }
    return all_names;
invalid:
    reent->_errno=EINVAL; return NULL;
}
const struct lc_numeric_T *__ciuki_numeric_locale(void)
{
    if(!selected[LC_NUMERIC]) return &_C_numeric_locale;
    return &numeric_table;
}
static void update_time(void)
{
    static const char *const mon[]={"gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic"};
    static const char *const month[]={"gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre"};
    static const char *const day[]={"dom","lun","mar","mer","gio","ven","sab"};
    static const char *const weekday[]={"domenica","lunedì","martedì","mercoledì","giovedì","venerdì","sabato"};
    time_table=_C_time_locale;
    if(selected[LC_TIME]==2) {
        for(unsigned i=0;i<12;i++) { time_table.mon[i]=mon[i]; time_table.month[i]=month[i]; time_table.alt_month[i]=month[i]; }
        for(unsigned i=0;i<7;i++) { time_table.wday[i]=day[i]; time_table.weekday[i]=weekday[i]; }
    }
    reg_strftime_picture(times.value[REG_DATE],date_picture);
    reg_strftime_picture(times.value[REG_TIME],time_picture);
    reg_strftime_picture(times.value[REG_LONG_DATE],long_picture);
    snprintf(combined_picture,sizeof(combined_picture),"%s %s",long_picture,time_picture);
    time_table.x_fmt=date_picture; time_table.X_fmt=time_picture; time_table.c_fmt=combined_picture;
    time_table.am_pm[0]=times.value[REG_AM]; time_table.am_pm[1]=times.value[REG_PM];
    time_table.md_order=selected[LC_TIME]==2?"dm":"md";
}
const struct lc_time_T *__ciuki_time_locale(void)
{
    return selected[LC_TIME]?&time_table:&_C_time_locale;
}
