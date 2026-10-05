/* Bounded CSS 2.1 subset for CiukWeb. State is private to this CAPP. */
#include "webstyle.h"

#ifndef WEBSTYLE_LOCAL_OFFSET
#define WEBSTYLE_LOCAL_OFFSET(p) ((u16)(p))
#endif

#define WS_MAX_RULES 256
#define WS_MAX_DEPTH 32
#define WS_SHEET_MAX 32768UL
#define WS_RULE_SELECTOR_MAX 48
#define WS_SELECTOR_BUF_MAX 256
#define WS_DECL_MAX 512
#define WS_DECL_POOL_MAX 8192
#define WS_PROP_COUNT 19

struct ws_rule { char selector[WS_RULE_SELECTOR_MAX]; u16 decl_offset; u32 order; };
struct ws_node { char tag[WEBSTYLE_TAG_MAX]; char id[WEBSTYLE_ID_MAX]; char classes[WEBSTYLE_CLASSES_MAX]; struct webstyle_computed style; };
struct ws_value { long n; u32 color; u8 kind; };

static struct ws_rule rules[WS_MAX_RULES];
static struct ws_node nodes[WS_MAX_DEPTH];
static char decl_pool[WS_DECL_POOL_MAX];
static u16 decl_pool_used;
static struct webstyle_request request;
static u16 rule_count, depth;
static u32 source_order, sheet_bytes;
static u8 parse_decl, parse_comment, parse_pending_slash, parse_quote, rule_bad, ws_limit_hit;
static u8 at_rule, at_depth;
static u16 selector_len, decl_len;
static char selector_buf[WS_SELECTOR_BUF_MAX], decl_buf[WS_DECL_MAX];

static int ws_space(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'; }
static char ws_lower(char c) { return c>='A'&&c<='Z'?(char)(c+32):c; }
static int ws_eq(const char *a,const char *b)
{
    while(*a&&*b) { if(ws_lower(*a++)!=ws_lower(*b++)) return 0; }
    return *a==0&&*b==0;
}
static int ws_exact(const char *a,const char *b)
{
    while(*a&&*b) if(*a++!=*b++) return 0;
    return *a==0&&*b==0;
}
static void ws_copy(char *d,const char *s,u16 cap)
{
    u16 i=0; if(!cap) return;
    while(s[i]&&i+1<cap) { d[i]=s[i]; ++i; } d[i]=0;
}
static void ws_trim(char *s)
{
    u16 a=0,b=0,n=0; while(s[n]) ++n;
    while(a<n&&ws_space(s[a])) ++a;
    b=n; while(b>a&&ws_space(s[b-1])) --b;
    if(a) { u16 i; for(i=a;i<b;i++) s[i-a]=s[i]; }
    s[b-a]=0;
}
static u32 ws_specificity(const char *s)
{
    u16 ids=0,classes=0,tags=0; int in_name=0;
    while(*s) {
        if(*s=='#') { if(ids<255) ++ids; in_name=0; }
        else if(*s=='.') { if(classes<255) ++classes; in_name=0; }
        else if(*s=='>'||ws_space(*s)) in_name=0;
        else if(!in_name&&*s!='*') { if(tags<255) ++tags; in_name=1; }
        else in_name=1;
        ++s;
    }
    return ((u32)ids<<16)|((u32)classes<<8)|tags;
}

static int ws_property(const char *s);
static int ws_is_supported_prop(const char *s)
{
    return ws_property(s)>=0||ws_eq(s,"margin")||ws_eq(s,"padding")||ws_eq(s,"border")||ws_eq(s,"background");
}
static void ws_compact_decl(const char *in,char *out,u16 cap)
{
    char name[32],val[96]; u16 i=0,j,out_len=0; out[0]=0;
    while(in[i]&&out_len+1<cap) {
        while(ws_space(in[i])||in[i]==';') ++i;
        j=0; while(in[i]&&in[i]!=':'&&in[i]!=';'&&j<sizeof name-1) name[j++]=in[i++]; name[j]=0; ws_trim(name);
        if(in[i]!=':') { while(in[i]&&in[i]!=';') ++i; continue; }
        ++i; j=0; while(in[i]&&in[i]!=';'&&j<sizeof val-1) val[j++]=in[i++]; val[j]=0; if(in[i]==';') ++i; ws_trim(val);
        if(name[0]&&ws_is_supported_prop(name)) {
            u16 nlen=0,vlen=0,k;
            while(name[nlen]) ++nlen;
            while(val[vlen]) ++vlen;
            if(out_len+nlen+1+vlen+2<cap) {
                for(k=0;k<nlen;k++) out[out_len++]=name[k];
                out[out_len++]=':';
                for(k=0;k<vlen;k++) out[out_len++]=val[k];
                out[out_len++]=';';
                out[out_len]=0;
            }
        }
    }
}

static void ws_add_selector(const char *s,u16 decl_offset)
{
    u16 i=0,j=0; char one[WS_RULE_SELECTOR_MAX];
    while(s[i]) {
        while(ws_space(s[i])||s[i]==',') ++i;
        if(!s[i]) break;
        j=0;
        while(s[i]&&s[i]!=','&&j<WS_RULE_SELECTOR_MAX-1) one[j++]=s[i++];
        while(j&&ws_space(one[j-1])) --j;
        one[j]=0;
        while(s[i]&&s[i]!=',') ++i;
        if(s[i]==',') ++i;
        if(!one[0]||one[0]=='@') continue;
        if(rule_count>=WS_MAX_RULES) { rule_bad=1;ws_limit_hit=1;continue; }
        ws_copy(rules[rule_count].selector,one,WS_RULE_SELECTOR_MAX);
        rules[rule_count].decl_offset=decl_offset;
        rules[rule_count].order=source_order++;
        ++rule_count;
    }
}

static void ws_finish_rule(void)
{
    static char compact_buf[WS_DECL_MAX];
    selector_buf[selector_len]=0;
    decl_buf[decl_len]=0;
    ws_trim(selector_buf); ws_trim(decl_buf);
    if(selector_buf[0]&&!rule_bad) {
        ws_compact_decl(decl_buf,compact_buf,sizeof compact_buf);
        if(compact_buf[0]) {
            u16 dlen=0; while(compact_buf[dlen]) ++dlen;
            if(decl_pool_used+dlen+1>WS_DECL_POOL_MAX) { rule_bad=1; ws_limit_hit=1; }
            else {
                u16 offset=decl_pool_used;
                ws_copy(decl_pool+offset,compact_buf,(u16)(dlen+1));
                decl_pool_used=(u16)(decl_pool_used+dlen+1);
                ws_add_selector(selector_buf,offset);
            }
        }
    }
    selector_len=decl_len=0; selector_buf[0]=decl_buf[0]=0;
    parse_decl=0; rule_bad=0;
}

static void ws_char(char c)
{
    if(parse_comment) {
        if(parse_comment==2&&c=='/') parse_comment=0;
        else if(c=='*') parse_comment=2;
        else parse_comment=1;
        return;
    }
    if(at_rule) {
        if(c=='{') { if(at_depth<255) ++at_depth; else ws_limit_hit=1; }
        else if(c=='}'&&at_depth) { --at_depth; if(!at_depth) { at_rule=0;selector_len=0;selector_buf[0]=0; } }
        else if(c==';'&&!at_depth) { at_rule=0;selector_len=0;selector_buf[0]=0; }
        return;
    }
    if(parse_pending_slash) {
        parse_pending_slash=0;
        if(c=='*') { parse_comment=1; return; }
        if(parse_decl) { if(decl_len+1<WS_DECL_MAX) decl_buf[decl_len++]='/'; else rule_bad=ws_limit_hit=1; }
        else { if(selector_len+1<WS_SELECTOR_BUF_MAX) selector_buf[selector_len++]='/'; else rule_bad=ws_limit_hit=1; }
    }
    if(!parse_decl&&!selector_len&&ws_space(c)) return;
    if(!parse_decl&&!selector_len&&c=='@') { at_rule=1;at_depth=0;return; }
    if(!parse_quote&&c=='/') { parse_pending_slash=1; return; }
    if(c=='\''||c=='"') {
        if(!parse_quote) parse_quote=(u8)c;
        else if(parse_quote==(u8)c) parse_quote=0;
    }
    if(!parse_quote&&!parse_decl&&c=='{') { parse_decl=1; return; }
    if(!parse_quote&&parse_decl&&c=='}') { ws_finish_rule(); return; }
    if(parse_decl) {
        if(decl_len+1<WS_DECL_MAX) decl_buf[decl_len++]=c; else rule_bad=ws_limit_hit=1;
    } else {
        if(selector_len+1<WS_SELECTOR_BUF_MAX) selector_buf[selector_len++]=c; else rule_bad=ws_limit_hit=1;
    }
}

static int ws_class_has(const char *list,const char *name)
{
    u16 i=0,n=0; while(name[n]) ++n;
    while(list[i]) {
        u16 start; while(ws_space(list[i])) ++i; start=i;
        while(list[i]&&!ws_space(list[i])) ++i;
        if(i-start==n&&n) { u16 j; for(j=0;j<n&&list[start+j]==name[j];++j) {} if(j==n) return 1; }
    }
    return 0;
}

struct ws_comp { char raw[WS_RULE_SELECTOR_MAX]; u8 rel; };
static int ws_simple_match(const char *s,const struct ws_node *n)
{
    char word[WEBSTYLE_ID_MAX]; u16 i=0,j=0; int have_tag=0;
    if(!s[0]) return 0;
    if(s[i]!='.'&&s[i]!='#'&&s[i]!='*') {
        while(s[i]&&s[i]!='.'&&s[i]!='#'&&j<WEBSTYLE_TAG_MAX-1) word[j++]=s[i++];
        word[j]=0; if(!ws_eq(word,n->tag)) return 0; have_tag=1;
    }
    if(!have_tag&&s[i]=='*') ++i;
    while(s[i]) {
        char mark=s[i++]; j=0;
        while(s[i]&&s[i]!='.'&&s[i]!='#'&&j<WEBSTYLE_ID_MAX-1) word[j++]=s[i++];
        word[j]=0; if(!j) return 0;
        if(mark=='#') { if(!ws_exact(n->id,word)) return 0; }
        else if(mark=='.') { if(!ws_class_has(n->classes,word)) return 0; }
        else return 0;
    }
    return 1;
}

static int ws_selector_match(const char *s,const char *tag,const char *id,const char *classes)
{
    struct ws_comp part[8]; struct ws_node target; u8 count=0; u16 i=0,j; int pi; int anc;
    while(s[i]&&ws_space(s[i])) ++i;
    while(s[i]&&count<8) {
        j=0;
        while(s[i]&&!ws_space(s[i])&&s[i]!='>'&&j<WS_RULE_SELECTOR_MAX-1) part[count].raw[j++]=s[i++];
        part[count].raw[j]=0; if(!j) return 0;
        while(ws_space(s[i])) ++i;
        part[count].rel=0;
        if(s[i]=='>') { part[count].rel=1; ++i; while(ws_space(s[i])) ++i; }
        else if(s[i]) part[count].rel=2;
        ++count;
    }
    if(s[i]||!count) return 0;
    ws_copy(target.tag,tag,WEBSTYLE_TAG_MAX); ws_copy(target.id,id,WEBSTYLE_ID_MAX); ws_copy(target.classes,classes,WEBSTYLE_CLASSES_MAX);
    if(!ws_simple_match(part[count-1].raw,&target)) return 0;
    anc=(int)depth-1;
    for(pi=(int)count-2;pi>=0;--pi) {
        if(part[pi].rel==1) {
            if(anc<0||!ws_simple_match(part[pi].raw,&nodes[anc])) return 0;
            --anc;
        } else {
            while(anc>=0&&!ws_simple_match(part[pi].raw,&nodes[anc])) --anc;
            if(anc<0) return 0;
            --anc;
        }
    }
    return 1;
}

static int ws_parse_px(const char *s,long *out)
{
    long n=0; int sign=1,any=0; if(*s=='-') { sign=-1; ++s; }
    while(*s>='0'&&*s<='9') { any=1; n=n*10+(*s++-'0'); if(n>4096) return 0; }
    while(ws_space(*s)) ++s;
    if(!any||!ws_eq(s,"px")) return 0;
    *out=n*sign; return 1;
}
static int ws_color(const char *s,u32 *out)
{
    static const struct { const char *name; u32 value; } names[]={
        {"black",0x000000UL},{"white",0xFFFFFFUL},{"red",0xFF0000UL},{"green",0x008000UL},
        {"blue",0x0000FFUL},{"yellow",0xFFFF00UL},{"gray",0x808080UL},{"grey",0x808080UL},
        {"silver",0xC0C0C0UL},{"maroon",0x800000UL},{"purple",0x800080UL},{"lime",0x00FF00UL},
        {"olive",0x808000UL},{"navy",0x000080UL},{"teal",0x008080UL},{"aqua",0x00FFFFUL},
        {"fuchsia",0xFF00FFUL},{"orange",0xFFA500UL}};
    int i; if(ws_eq(s,"transparent")) { *out=0xFFFFFFFFUL; return 1; }
    if(*s=='#') {
        u32 v=0; int k=0; ++s;
        while(s[k]&&k<6) { char c=ws_lower(s[k]); int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; if(d<0) return 0; v=(v<<4)|(u32)d; ++k; }
        if(s[k]) return 0;
        if(k==3) { *out=((v&0xF00)<<12)|((v&0xF00)<<8)|((v&0x0F0)<<8)|((v&0x0F0)<<4)|((v&0x00F)<<4)|(v&0x00F); return 1; }
        if(k==6) { *out=v; return 1; }
        return 0;
    }
    for(i=0;i<(int)(sizeof names/sizeof names[0]);++i) if(ws_eq(s,names[i].name)) { *out=names[i].value; return 1; }
    return 0;
}

static int ws_property(const char *s)
{
    static const char *names[WS_PROP_COUNT]={"color","background-color","font-weight","font-style","display",
        "margin-top","margin-right","margin-bottom","margin-left","padding-top","padding-right","padding-bottom","padding-left",
        "width","height","text-align","border-width","border-color","border-style"};
    int i; for(i=0;i<WS_PROP_COUNT;i++) if(ws_eq(s,names[i])) return i; return -1;
}

static int ws_value_parse(int prop,char *value,struct ws_value *v)
{
    long px; u32 color;
    ws_trim(value);
    if(ws_eq(value,"inherit")) { v->kind=3;v->n=0;return 1; }
    if(prop==0||prop==1||prop==18) {
        if(prop==18) {
            if(ws_eq(value,"none")) { v->kind=1; v->n=WEBSTYLE_BORDER_NONE; return 1; }
            if(ws_eq(value,"solid")) { v->kind=1; v->n=WEBSTYLE_BORDER_SOLID; return 1; }
            if(ws_eq(value,"dashed")) { v->kind=1; v->n=WEBSTYLE_BORDER_DASHED; return 1; }
            if(ws_eq(value,"dotted")) { v->kind=1; v->n=WEBSTYLE_BORDER_DOTTED; return 1; }
        } else if(ws_color(value,&color)) { v->kind=2; v->color=color; return 1; }
        return 0;
    }
    if(prop==2) { if(ws_eq(value,"bold")||ws_eq(value,"bolder")||ws_eq(value,"600")||ws_eq(value,"700")||ws_eq(value,"800")||ws_eq(value,"900")) { v->kind=1; v->n=WEBSTYLE_WEIGHT_BOLD; return 1; }
        if(ws_eq(value,"normal")||ws_eq(value,"100")||ws_eq(value,"200")||ws_eq(value,"300")||ws_eq(value,"400")||ws_eq(value,"500")) { v->kind=1; v->n=WEBSTYLE_WEIGHT_NORMAL; return 1; } return 0; }
    if(prop==3) { if(ws_eq(value,"italic")||ws_eq(value,"oblique")) { v->kind=1; v->n=WEBSTYLE_FONT_ITALIC; return 1; }
        if(ws_eq(value,"normal")) { v->kind=1; v->n=WEBSTYLE_FONT_NORMAL; return 1; } return 0; }
    if(prop==4) { if(ws_eq(value,"none")) {v->kind=1;v->n=WEBSTYLE_DISPLAY_NONE;return 1;}
        if(ws_eq(value,"block")) {v->kind=1;v->n=WEBSTYLE_DISPLAY_BLOCK;return 1;}
        if(ws_eq(value,"inline")||ws_eq(value,"inline-block")) {v->kind=1;v->n=WEBSTYLE_DISPLAY_INLINE;return 1;} return 0; }
    if(prop==15) { if(ws_eq(value,"left"))v->n=WEBSTYLE_ALIGN_LEFT; else if(ws_eq(value,"right"))v->n=WEBSTYLE_ALIGN_RIGHT;
        else if(ws_eq(value,"center"))v->n=WEBSTYLE_ALIGN_CENTER; else if(ws_eq(value,"justify"))v->n=WEBSTYLE_ALIGN_JUSTIFY; else return 0; v->kind=1;return 1; }
    if(prop==17&&ws_color(value,&color)) { v->kind=2;v->color=color;return 1; }
    if(prop==17) return 0;
    if(ws_parse_px(value,&px)) {
        if(px<0&&!(prop>=5&&prop<=8))return 0;
        v->kind=1;v->n=px;return 1;
    }
    if(((prop>=5&&prop<=14)||prop==16)&&ws_eq(value,"0")) { v->kind=1;v->n=0;return 1; }
    return 0;
}

struct ws_best { u32 spec,order; struct ws_value value; u8 set,important; };
static void ws_set_decl(struct ws_best *best,int prop,char *value,u8 important,u32 spec,u32 order)
{
    struct ws_value v; int take=0;
    if(prop<0||prop>=WS_PROP_COUNT||!ws_value_parse(prop,value,&v)) return;
    if(!best[prop].set) take=1;
    else if(important>best[prop].important) take=1;
    else if(important==best[prop].important&&(spec>best[prop].spec||(spec==best[prop].spec&&order>=best[prop].order))) take=1;
    if(take) { best[prop].set=1;best[prop].important=important;best[prop].spec=spec;best[prop].order=order;best[prop].value=v; }
}

static void ws_shorthand(struct ws_best *best,const char *name,const char *value,u8 important,u32 spec,u32 order)
{
    char copy[64],token[4][16]; int n=0; long px; u16 i=0,j;
    ws_copy(copy,value,sizeof copy);
    while(copy[i]&&n<4) { while(ws_space(copy[i])) ++i; if(!copy[i]) break; j=0;
        while(copy[i]&&!ws_space(copy[i])&&j<15) token[n][j++]=copy[i++]; token[n][j]=0; ++n; }
    if(!n||copy[i]) return;
    if(ws_eq(name,"margin")||ws_eq(name,"padding")) {
        int first=ws_eq(name,"margin")?5:9;
        ws_set_decl(best,first,token[0],important,spec,order);
        ws_set_decl(best,first+1,token[n>1?1:0],important,spec,order);
        ws_set_decl(best,first+2,token[n>2?2:0],important,spec,order);
        ws_set_decl(best,first+3,token[n>3?3:(n>1?1:0)],important,spec,order);
    } else if(ws_eq(name,"border")) {
        int k; for(k=0;k<n;k++) {
            char *t=token[k];
            if(ws_parse_px(t,&px)) ws_set_decl(best,16,t,important,spec,order);
            else if(ws_eq(t,"none")||ws_eq(t,"solid")||ws_eq(t,"dashed")||ws_eq(t,"dotted")) ws_set_decl(best,18,t,important,spec,order);
            else ws_set_decl(best,17,t,important,spec,order);
        }
    }
}

static void ws_apply_declarations(struct ws_best *best,const char *decl,u8 important,u32 spec,u32 order)
{
    char name[32],value[64]; u16 i=0,j,k;
    while(decl[i]) {
        u8 this_important=important;
        while(ws_space(decl[i])||decl[i]==';') ++i;
        j=0;while(decl[i]&&decl[i]!=':'&&decl[i]!=';'&&j<sizeof name-1)name[j++]=decl[i++];name[j]=0;ws_trim(name);
        if(decl[i]!=':'){while(decl[i]&&decl[i]!=';')++i;continue;}
        ++i;j=0;while(decl[i]&&decl[i]!=';'&&j<sizeof value-1)value[j++]=decl[i++];value[j]=0;if(decl[i]==';')++i;ws_trim(value);
        k=0;while(value[k])++k;
        if(k>=10){u16 p=k;while(p&&ws_space(value[p-1]))--p;if(p>=10&&ws_eq(value+p-10,"!important")){value[p-10]=0;ws_trim(value);this_important=1;}}
        if(ws_eq(name,"margin")||ws_eq(name,"padding")||ws_eq(name,"border")) ws_shorthand(best,name,value,this_important,spec,order);
        else if(ws_eq(name,"background")) {
            u32 color;
            if(ws_eq(value,"none")||ws_eq(value,"transparent")) ws_set_decl(best,1,"transparent",this_important,spec,order);
            else if(ws_color(value,&color)) ws_set_decl(best,1,value,this_important,spec,order);
        }
        else ws_set_decl(best,ws_property(name),value,this_important,spec,order);
    }
}

static void ws_inherit(struct webstyle_computed *out)
{
    int i;
    out->specified=0;out->declared=0;out->color=0;out->background_color=0;out->border_color=0;
    out->font_weight=WEBSTYLE_WEIGHT_NORMAL;out->font_style=WEBSTYLE_FONT_NORMAL;
    out->display=WEBSTYLE_DISPLAY_INLINE;out->text_align=WEBSTYLE_ALIGN_LEFT;out->border_style=WEBSTYLE_BORDER_NONE;
    for(i=0;i<4;i++){out->margin[i]=0;out->padding[i]=0;}
    out->width=out->height=-1;out->border_width=0;
    if(depth) {
        struct webstyle_computed *p=&nodes[depth-1].style;
        out->color=p->color;out->font_weight=p->font_weight;out->font_style=p->font_style;out->text_align=p->text_align;
        out->specified=p->specified&(WEBSTYLE_PROP_COLOR|WEBSTYLE_PROP_FONT_WEIGHT|WEBSTYLE_PROP_FONT_STYLE|WEBSTYLE_PROP_TEXT_ALIGN);
    }
}

static void ws_commit(struct webstyle_computed *out,struct ws_best *best)
{
    int p; for(p=0;p<WS_PROP_COUNT;p++) if(best[p].set) {
        struct ws_value *v=&best[p].value; out->specified|=1UL<<p;out->declared|=1UL<<p;
        if(v->kind==3) {
            if(depth) {
                struct webstyle_computed *parent=&nodes[depth-1].style;
                switch(p) {
                case 0:out->color=parent->color;break;case 1:out->background_color=parent->background_color;break;
                case 2:out->font_weight=parent->font_weight;break;case 3:out->font_style=parent->font_style;break;
                case 4:out->display=parent->display;break;
                case 5:case 6:case 7:case 8:out->margin[p-5]=parent->margin[p-5];break;
                case 9:case 10:case 11:case 12:out->padding[p-9]=parent->padding[p-9];break;
                case 13:out->width=parent->width;break;case 14:out->height=parent->height;break;
                case 15:out->text_align=parent->text_align;break;case 16:out->border_width=parent->border_width;break;
                case 17:out->border_color=parent->border_color;break;case 18:out->border_style=parent->border_style;break;
                }
            }
            continue;
        }
        switch(p) {
        case 0:out->color=v->color;break; case 1:out->background_color=v->color;break;
        case 2:out->font_weight=(u8)v->n;break;case 3:out->font_style=(u8)v->n;break;
        case 4:out->display=(u8)v->n;break;
        case 5:case 6:case 7:case 8:out->margin[p-5]=(short)v->n;break;
        case 9:case 10:case 11:case 12:out->padding[p-9]=(short)v->n;break;
        case 13:out->width=(short)v->n;break;case 14:out->height=(short)v->n;break;
        case 15:out->text_align=(u8)v->n;break;case 16:out->border_width=(short)v->n;break;
        case 17:out->border_color=v->color;break;case 18:out->border_style=(u8)v->n;break;
        }
    }
}

static void ws_compute(struct webstyle_computed *out,const char *tag,const char *id,const char *classes,const char *inline_style)
{
    struct ws_best best[WS_PROP_COUNT]; u16 i; u32 spec;
    ws_inherit(out); for(i=0;i<WS_PROP_COUNT;i++){best[i].set=0;best[i].important=0;best[i].spec=best[i].order=0;}
    for(i=0;i<rule_count;i++) if(ws_selector_match(rules[i].selector,tag,id,classes)) {
        spec=ws_specificity(rules[i].selector);
        ws_apply_declarations(best,decl_pool+rules[i].decl_offset,0,spec,rules[i].order);
    }
    if(inline_style&&inline_style[0]) ws_apply_declarations(best,inline_style,0,0x01000000UL,0xFFFFFFFFUL);
    ws_commit(out,best);
}

static void ws_reset(void)
{
    rule_count=depth=0;source_order=sheet_bytes=0;parse_decl=parse_comment=parse_pending_slash=parse_quote=rule_bad=ws_limit_hit=0;at_rule=at_depth=0;
    selector_len=decl_len=0;selector_buf[0]=decl_buf[0]=0;decl_pool_used=0;
}

int app_event(int ev,int a,int b,int c)
{
    u16 i; int op; (void)c;
    if(ev==EV_OPEN) { ws_reset(); return 1; }
    if(ev==EV_CLOSE) { ws_reset(); return 1; }
    if(ev!=EV_POLL) return 0;
    far_copy(app_seg(),WEBSTYLE_LOCAL_OFFSET(&request),(u16)a,(u16)b,sizeof request);
    if(request.abi_version!=WEBSTYLE_ABI_VERSION||request.struct_bytes<sizeof request) {
        request.status=WEBSTYLE_STATUS_BAD_ABI;far_copy((u16)a,(u16)b,app_seg(),WEBSTYLE_LOCAL_OFFSET(&request),sizeof request);return 1;
    }
    op=request.op;request.status=WEBSTYLE_STATUS_OK;
    if(op==WEBSTYLE_OP_RESET) ws_reset();
    else if(op==WEBSTYLE_OP_FEED) {
        if(request.css_bytes>WEBSTYLE_CHUNK_MAX||sheet_bytes+request.css_bytes>WS_SHEET_MAX) request.status=WEBSTYLE_STATUS_LIMIT;
        else {
            sheet_bytes+=request.css_bytes;
            for(i=0;i<request.css_bytes;i++) ws_char(request.css[i]);
            if(ws_limit_hit) request.status=WEBSTYLE_STATUS_LIMIT;
        }
    } else if(op==WEBSTYLE_OP_SHEET_END) {
        if(parse_decl||parse_comment||at_rule) request.status=WEBSTYLE_STATUS_ERROR;
        else if(ws_limit_hit) request.status=WEBSTYLE_STATUS_LIMIT;
    } else if(op==WEBSTYLE_OP_ENTER||op==WEBSTYLE_OP_APPLY) {
        for(i=0;i<WEBSTYLE_INLINE_MAX&&request.inline_style[i];i++) {}
        if(i>=WEBSTYLE_INLINE_MAX) { request.status=WEBSTYLE_STATUS_LIMIT;far_copy((u16)a,(u16)b,app_seg(),WEBSTYLE_LOCAL_OFFSET(&request),sizeof request);return 1; }
        ws_compute(&request.style,request.tag,request.id,request.classes,request.inline_style);
        if(op==WEBSTYLE_OP_ENTER&&!(request.flags&1)) {
            if(depth>=WS_MAX_DEPTH) request.status=WEBSTYLE_STATUS_LIMIT;
            else {
                ws_copy(nodes[depth].tag,request.tag,sizeof nodes[depth].tag);
                ws_copy(nodes[depth].id,request.id,sizeof nodes[depth].id);
                ws_copy(nodes[depth].classes,request.classes,sizeof nodes[depth].classes);
                nodes[depth].style=request.style;++depth;
            }
        }
    } else if(op==WEBSTYLE_OP_LEAVE) {
        if(!depth) request.status=WEBSTYLE_STATUS_ERROR;
        else if(!request.tag[0]) --depth;
        else {
            int found=-1; for(i=depth;i>0;i--) if(ws_eq(nodes[i-1].tag,request.tag)){found=(int)i-1;break;}
            if(found<0) request.status=WEBSTYLE_STATUS_ERROR; else depth=(u16)found;
        }
        ws_inherit(&request.style);
    } else request.status=WEBSTYLE_STATUS_ERROR;
    far_copy((u16)a,(u16)b,app_seg(),WEBSTYLE_LOCAL_OFFSET(&request),sizeof request);
    return 1;
}
