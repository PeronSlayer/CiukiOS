/* Bounded INF selection and private HKR materialization. No setup commands.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "driver_pack.h"
#include "../../src/kernel/include/ciuki/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define DP_DOCS 8
#define DP_ROWS 1024
#define DP_LINE 512
struct row { char *section,*text; };
struct document { char name[64]; char *text; struct row rows[DP_ROWS]; unsigned count; };
struct parser {
    struct document docs[DP_DOCS]; unsigned count;
    struct dp_plan *plan; dp_fetch fetch; void *ctx; char *reason;
    char stack[16][128]; unsigned depth;
};
static int lower(int c) { return c>='A' && c<='Z' ? c+32 : c; }
static int equal(const char *a,const char *b)
{ while(*a && lower(*a)==lower(*b)) { a++; b++; } return *a==*b; }
static int prefix(const char *a,const char *b)
{ while(*b) if(lower(*a++)!=lower(*b++)) return 0; return 1; }
static int fail(struct parser *p,const char *why)
{ snprintf(p->reason,160,"%s",why); return -EINVAL; }
static char *trim(char *s)
{ while(*s==' '||*s=='\t'||*s=='\r') s++; size_t n=strlen(s); while(n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r')) s[--n]=0; return s; }
static int copy(char *out,size_t cap,const char *in)
{ size_t n=strlen(in); if(n>=cap) return -1; memcpy(out,in,n+1); return 0; }
static void unquote(char *s)
{
    size_t n=strlen(s);
    if(n>=2 && s[0]=='"' && s[n-1]=='"') { memmove(s,s+1,n-2); s[n-2]=0; }
    char *a=s,*b=s;
    while(*a) { *b++=*a++; if(a[-1]=='"' && *a=='"') a++; } *b=0;
}
/* Semicolon comments and commas respect quotes, as in build_hwids.py. */
static int csv(char *s,char **v,unsigned cap)
{
    unsigned n=0; int quoted=0; char *start=s;
    for(char *at=s;;at++) {
        if(*at=='"') quoted=!quoted;
        if(!*at || (*at==',' && !quoted)) {
            int end=!*at; *at=0;
            if(n==cap) return -1;
            v[n]=trim(start); unquote(v[n]); n++;
            if(end) break;
            start=at+1;
        }
    }
    return quoted ? -1 : (int)n;
}
static int number(const char *s,uint32_t *v)
{
    if(!*s) { *v=0; return 0; }
    if(*s=='-' || *s=='+') return -1;
    char *end; errno=0; unsigned long value=strtoul(s,&end,prefix(s,"0x") ? 16 : 10);
    if(errno || *end || value>UINT32_MAX) return -1;
    *v=(uint32_t)value; return 0;
}
static int hexbyte(const char *s,uint8_t *v)
{
    if(!*s || strlen(s)>2) return -1;
    unsigned x=0;
    for(;*s;s++) { int c=lower(*s); if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return -1; x=x*16+(unsigned)(c<='9'?c-'0':c-'a'+10); }
    *v=(uint8_t)x; return 0;
}
static int has_section(const struct document *d,const char *section)
{ for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) return 1; return 0; }
static int value(const struct document *d,const char *section,const char *key,char *out,size_t cap)
{
    int found=0;
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) {
        char line[DP_LINE]; if(copy(line,sizeof(line),d->rows[i].text)) return -1;
        char *eq=strchr(line,'='); if(!eq) continue; *eq++=0;
        if(equal(trim(line),key)) { if(found || copy(out,cap,trim(eq))) return -1; found=1; }
    }
    return found ? 0 : -1;
}
static int string_value(const struct document *d,const char *section,const char *key,char *out,size_t cap)
{
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) {
        char line[DP_LINE]; if(copy(line,sizeof(line),d->rows[i].text)) return -1;
        char *eq=strchr(line,'='); if(!eq) continue; *eq++=0;
        if(equal(trim(line),key)) return copy(out,cap,trim(eq));
    }
    return -1;
}
static int expand(const struct document *d,const char *in,char *out,size_t cap,unsigned depth)
{
    size_t at=0;
    if(depth>8) return -1;
    while(*in) {
        if(*in=='%') {
            if(in[1]=='%') { if(at+1>=cap) return -1; out[at++]='%'; in+=2; continue; }
            const char *end=strchr(in+1,'%'); char key[96],raw[DP_LINE],replacement[DP_LINE];
            if(!end || end-in-1>=(long)sizeof(key)) return -1;
            memcpy(key,in+1,(size_t)(end-in-1)); key[end-in-1]=0;
            /* Directory IDs are resolved by the restricted file policy. */
            if(equal(key,"12") || equal(key,"11")) {
                if(at+(size_t)(end-in+1)>=cap) return -1;
                memcpy(out+at,in,(size_t)(end-in+1)); at+=(size_t)(end-in+1);
            } else {
                int result=string_value(d,"Strings",key,raw,sizeof(raw));
                if(result) {
                    const char *best=NULL;
                    for(unsigned i=0;i<d->count;i++) if(prefix(d->rows[i].section,"strings.") &&
                        (!best || strcmp(d->rows[i].section,best)<0) &&
                        !string_value(d,d->rows[i].section,key,raw,sizeof(raw))) best=d->rows[i].section;
                    if(!best || string_value(d,best,key,raw,sizeof(raw))) return -1;
                }
                unquote(raw);
                if(expand(d,raw,replacement,sizeof(replacement),depth+1) || at+strlen(replacement)>=cap) return -1;
                memcpy(out+at,replacement,strlen(replacement)); at+=strlen(replacement);
            }
            in=end+1;
        } else { if(at+1>=cap) return -1; out[at++]=*in++; }
    }
    out[at]=0; return 0;
}
static int expanded_value(const struct document *d,const char *section,const char *key,char *out,size_t cap)
{ char raw[DP_LINE]; if(value(d,section,key,raw,sizeof(raw))) return -1; unquote(raw); return expand(d,raw,out,cap,0); }
static int fields(const struct document *d,const char *raw,char out[DP_LINE],char **v,unsigned capacity)
{
    char tokens[DP_LINE]; if(copy(tokens,sizeof(tokens),raw)) return -1;
    int count=csv(tokens,v,capacity); size_t at=0;
    if(count<0) return -1;
    /* Token contents belong to their original field. A localized comma or
     * semicolon must not create another directive argument. */
    for(int i=0;i<count;i++) {
        if(at>=DP_LINE || expand(d,v[i],out+at,DP_LINE-at,0)) return -1;
        v[i]=out+at; at+=strlen(v[i])+1;
    }
    return count;
}
static int add_file(struct parser *p,const char *source,const char *dest)
{
    struct dp_plan *plan=p->plan; struct ndp_manifest *m=&plan->manifest;
    if(!ndp_relative(source) || strlen(source)>=sizeof(plan->source[0]) || !ndp_component(dest)) return fail(p,"Unsafe dependency path.");
    const char *ext=strrchr(dest,'.');
    if(!ext || !(equal(ext,".inf") || equal(ext,".sys") || equal(ext,".bin") || equal(ext,".dat") || equal(ext,".fw")))
        return fail(p,"Unsupported dependency or compressed source; no helpers are executed.");
    const char *source_ext=strrchr(source,'.');
    if(!source_ext || !(equal(source_ext,".inf") || equal(source_ext,".sys") || equal(source_ext,".bin") || equal(source_ext,".dat") || equal(source_ext,".fw")))
        return fail(p,"Compressed or unsupported source dependency.");
    if(equal(dest,"manifest.ndp") || equal(dest,"registry.ndp") || equal(dest,"transaction.ndp")) return fail(p,"Reserved package filename.");
    for(unsigned i=0;i<m->file_count;i++) if(equal(m->files[i].name,dest)) {
        if(equal(plan->source[i],source)) return (int)i;
        return fail(p,"Dependency basename collision.");
    }
    if(m->file_count==NDP_FILES) return fail(p,"Too many package files.");
    unsigned i=m->file_count++;
    copy(m->files[i].name,sizeof(m->files[i].name),dest); copy(plan->source[i],sizeof(plan->source[i]),source);
    for(char *c=plan->source[i];*c;c++) if(*c=='\\') *c='/';
    return (int)i;
}
static struct document *load(struct parser *p,const char *name)
{
    if(!ndp_component(name)) { fail(p,"Unsafe Include filename."); return NULL; }
    for(unsigned i=0;i<p->count;i++) if(equal(p->docs[i].name,name)) return &p->docs[i];
    if(p->count==DP_DOCS) { fail(p,"Too many included INFs."); return NULL; }
    uint8_t *bytes=NULL; size_t size=0;
    if(p->fetch(p->ctx,name,&bytes,&size) || !bytes || !size || size>DP_INF_LIMIT) { free(bytes); fail(p,"Missing or oversized INF dependency."); return NULL; }
    uint8_t source_hash[32]; sha256(bytes,size,source_hash);
    struct document *d=&p->docs[p->count++]; copy(d->name,sizeof(d->name),name);
    d->text=calloc(size+2,1); if(!d->text) { free(bytes); fail(p,"Not enough memory for INF."); return NULL; }
    size_t n=0,start=0; int wide=0,be=0;
    if(size>=2 && ((bytes[0]==255 && bytes[1]==254)||(bytes[0]==254 && bytes[1]==255))) { wide=1; be=bytes[0]==254; start=2; }
    else {
        size_t even=0,odd=0;
        for(size_t i=0;i<size;i++) if(!bytes[i]) { if(i&1) odd++; else even++; }
        if(even>size/20 || odd>size/20) { wide=1; be=even>odd; }
        if(size>=3 && !memcmp(bytes,"\xef\xbb\xbf",3)) start=3;
    }
    if(wide && (size-start)%2) { free(bytes); fail(p,"Truncated UTF-16 INF."); return NULL; }
    for(size_t i=start;i<size;i+=(wide?2:1)) {
        unsigned c=wide ? (be ? ((unsigned)bytes[i]<<8)|bytes[i+1] : ((unsigned)bytes[i+1]<<8)|bytes[i]) : bytes[i];
        if(!c) { free(bytes); fail(p,"NUL in INF."); return NULL; }
        /* CP1252/UTF-16 labels become printable ASCII, matching ascii_name.
         * Non-ASCII identifiers cannot match this restricted parser. */
        d->text[n++]=(char)(c>=128 ? '?' : c);
    }
    free(bytes); d->text[n]=0;
    /* Collapse line continuations before comments and sections. */
    char *a=d->text,*b=d->text;
    while(*a) { if(a[0]=='\\' && a[1]=='\n') a+=2; else if(a[0]=='\\' && a[1]=='\r' && a[2]=='\n') a+=3; else *b++=*a++; } *b=0;
    char *section=NULL,*line=d->text;
    while(*line) {
        char *next=strchr(line,'\n'); if(next) *next++=0;
        int quoted=0; for(char *s=line;*s;s++) { if(*s=='"') quoted=!quoted; else if(*s==';' && !quoted) { *s=0; break; } }
        char *s=trim(line);
        if(strlen(s)>=DP_LINE || quoted) { fail(p,"Oversized or malformed INF line."); return NULL; }
        if(*s=='[') {
            char *end=strchr(s,']'); if(!end || *trim(end+1)) { fail(p,"Malformed INF section."); return NULL; }
            *end=0; section=trim(s+1); for(char *c=section;*c;c++) *c=(char)lower(*c);
        } else if(*s && section) {
            if(d->count==DP_ROWS) { fail(p,"Too many INF entries."); return NULL; }
            d->rows[d->count++]=(struct row){section,s};
        }
        if(!next) break;
        line=next;
    }
    int f=add_file(p,name,name); if(f<0) return NULL;
    p->plan->manifest.files[f].bytes=(uint32_t)size;
    memcpy(p->plan->manifest.files[f].hash,source_hash,32);
    return d;
}
/* Only NT/x86 minimum decorations through the requested NT 5.x target. */
static int os_rank(const char *s,unsigned minor)
{
    int arch=0; if(prefix(s,"ntx86")) { s+=5; arch=1; } else if(prefix(s,"nt")) s+=2; else return -1;
    if(!*s) return arch;
    if(*s++!='.' || *s++!='5' || *s++!='.' || *s<'0' || *s>'2') return -1;
    unsigned m=(unsigned)(*s++-'0'); if(*s || m>minor) return -1;
    return 4+(int)m*2+arch;
}
static int id_rank(const char *id,const struct ndp_device *d)
{
    char base[96],test[96];
    snprintf(base,sizeof(base),"PCI\\VEN_%04X&DEV_%04X",d->vendor,d->device);
    snprintf(test,sizeof(test),"%s&SUBSYS_%04X%04X&REV_%02X",base,d->sub_device,d->sub_vendor,d->revision);
    if(equal(id,test)) return 4;
    snprintf(test,sizeof(test),"%s&SUBSYS_%04X%04X",base,d->sub_device,d->sub_vendor);
    if(equal(id,test)) return 3;
    snprintf(test,sizeof(test),"%s&REV_%02X",base,d->revision); if(equal(id,test)) return 2;
    return equal(id,base) ? 1 : 0;
}
static int select_model(struct parser *p,struct document *d,unsigned minor)
{
    struct ndp_manifest *m=&p->plan->manifest; int best_id=0,best_os=-1,ambiguous=0;
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,"Manufacturer")) {
        char line[DP_LINE],expanded[DP_LINE]; copy(line,sizeof(line),d->rows[i].text);
        char *eq=strchr(line,'='); if(!eq) return fail(p,"Malformed Manufacturer row.");
        char *parts[16]; int count=fields(d,eq+1,expanded,parts,16); if(count<1) return fail(p,"Malformed Manufacturer decoration.");
        /* x86 permits an undecorated model table, but requires an explicit NT
         * DDInstall section and associated kernel service below. */
        int rank=-1; char section[96]; section[0]=0;
        if(count==1) { rank=0; if(copy(section,sizeof(section),parts[0])) return fail(p,"Model section name too long."); }
        for(int j=1;j<count;j++) { int r=os_rank(parts[j],minor); if(r>rank) { rank=r; if(snprintf(section,sizeof(section),"%s.%s",parts[0],parts[j])>=(int)sizeof(section)) return fail(p,"Model section name too long."); } }
        if(rank<0) continue;
        for(unsigned j=0;j<d->count;j++) if(equal(d->rows[j].section,section)) {
            copy(line,sizeof(line),d->rows[j].text); eq=strchr(line,'='); if(!eq) return fail(p,"Malformed model row."); *eq++=0;
            char label[128]; if(expand(d,trim(line),label,sizeof(label),0)) return fail(p,"Unresolved model string."); unquote(label);
            int n=fields(d,eq,expanded,parts,16); if(n<2) return fail(p,"Malformed model hardware IDs.");
            for(int k=1;k<n;k++) {
                int r=id_rank(parts[k],&m->device); if(!r) continue;
                if(r>best_id || (r==best_id && rank>best_os)) {
                    if(copy(m->models,sizeof(m->models),section) || copy(m->install,sizeof(m->install),parts[0]) ||
                       copy(m->hardware_id,sizeof(m->hardware_id),parts[k]) || copy(m->name,sizeof(m->name),label)) return fail(p,"Model metadata too long.");
                    best_id=r; best_os=rank; ambiguous=0;
                } else if(r==best_id && rank==best_os && (!equal(m->install,parts[0]) || !equal(m->models,section))) ambiguous=1;
            }
        }
    }
    if(!best_id) return fail(p,"No exact PCI hardware ID and NT 5.x x86 model match.");
    if(ambiguous) return fail(p,"Ambiguous model selection; select an unambiguous INF.");
    char base[96],candidate[96]; copy(base,sizeof(base),m->install);
    const char *suffixes[]={"ntx86.5.2","ntx86.5.1","ntx86.5.0","ntx86","nt"};
    for(unsigned i=0;i<5;i++) if(os_rank(suffixes[i],minor)>=0) {
        if(snprintf(candidate,sizeof(candidate),"%s.%s",base,suffixes[i])>=(int)sizeof(candidate)) return fail(p,"Install section name too long.");
        if(has_section(d,candidate)) { copy(m->install,sizeof(m->install),candidate); break; }
    }
    /* Already decorated rows are accepted; plain Win9x installs are not. */
    int nt_install=0;
    for(const char *at=m->install;*at;at++) if(*at=='.' && os_rank(at+1,minor)>=0) nt_install=1;
    if(!has_section(d,m->install) || !nt_install) return fail(p,"No explicit NT install section.");
    if(snprintf(m->services,sizeof(m->services),"%s.Services",m->install)>=(int)sizeof(m->services)) return fail(p,"Services section name too long.");
    return 0;
}
static int parameter(struct parser *p,const char *view,char **v,int n)
{
    struct ndp_manifest *m=&p->plan->manifest;
    if(n<4 || !equal(v[0],"HKR")) return fail(p,"Only private HKR registry values are permitted.");
    uint32_t flags; if(number(v[3],&flags) || (flags!=0 && flags!=0x10001 && flags!=1)) return fail(p,"Unsupported AddReg type or operation.");
    if(strstr(v[1],"..") || strchr(v[1],':') || *v[1]=='\\' || *v[1]=='/' ||
       equal(v[2],"UpperFilters") || equal(v[2],"LowerFilters") || equal(v[2],"CoInstallers32")) return fail(p,"Unsafe registry key or helper registration.");
    struct ndp_parameter item; memset(&item,0,sizeof(item));
    if(snprintf(item.key,sizeof(item.key),"%s%s",view,v[1])>=(int)sizeof(item.key) || copy(item.name,sizeof(item.name),v[2])) return fail(p,"Registry key too long.");
    if(flags==0x10001) {
        uint32_t value; if(n!=5 || number(v[4],&value)) return fail(p,"Invalid DWORD parameter.");
        item.type=NDP_DWORD; item.bytes=4; for(unsigned i=0;i<4;i++) item.data[i]=(uint8_t)(value>>(8*i));
    } else if(flags==1) {
        item.type=NDP_BINARY; if(n<5 || (unsigned)(n-4)>NDP_DATA) return fail(p,"Invalid binary parameter length.");
        item.bytes=(unsigned)(n-4); for(unsigned i=0;i<item.bytes;i++) if(hexbyte(v[i+4],&item.data[i])) return fail(p,"Invalid binary parameter.");
    } else {
        item.type=NDP_STRING; const char *s=n==4?"":v[4]; if(n>5 || copy((char *)item.data,sizeof(item.data),s)) return fail(p,"Invalid string parameter."); item.bytes=(uint32_t)strlen(s)+1;
    }
    for(unsigned i=0;i<m->parameter_count;i++) if(equal(m->parameters[i].key,item.key) && equal(m->parameters[i].name,item.name)) { m->parameters[i]=item; return 0; }
    if(m->parameter_count==NDP_PARAMS) return fail(p,"Too many registry parameters.");
    m->parameters[m->parameter_count++]=item; return 0;
}
static int addreg(struct parser *p,struct document *d,const char *section,const char *view)
{
    if(!has_section(d,section)) return fail(p,"Missing AddReg section.");
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) {
        char expanded[DP_LINE]; char *v[260];
        int n=fields(d,d->rows[i].text,expanded,v,260);
        if(n<0) return fail(p,"Unresolved or malformed AddReg string.");
        if(parameter(p,view,v,n)) return -EINVAL;
    }
    return 0;
}
static int copyfile(struct parser *p,struct document *d,const char *dest,const char *source)
{
    if(!ndp_component(source)) return fail(p,"Unsafe source file name.");
    char raw[DP_LINE],path[192]; copy(path,sizeof(path),source);
    if(!value(d,"SourceDisksFiles.x86",source,raw,sizeof(raw)) || !value(d,"SourceDisksFiles",source,raw,sizeof(raw))) {
        char *v[8]; int n=csv(raw,v,8); if(n<1) return fail(p,"Malformed SourceDisksFiles.");
        char disk[DP_LINE],subdir[128]="";
        if(!value(d,"SourceDisksNames.x86",v[0],disk,sizeof(disk)) || !value(d,"SourceDisksNames",v[0],disk,sizeof(disk))) {
            char *parts[8]; int count=csv(disk,parts,8); if(count<1) return fail(p,"Malformed SourceDisksNames.");
            if(count>3 && *parts[3] && copy(subdir,sizeof(subdir),parts[3])) return fail(p,"Source path too long.");
        }
        if(snprintf(path,sizeof(path),"%s%s%s%s%s",subdir,*subdir?"/":"",n>1?v[1]:"",n>1&&*v[1]?"/":"",source)>=(int)sizeof(path)) return fail(p,"Source path too long.");
    }
    return add_file(p,path,dest)<0 ? -EINVAL : 0;
}
static int copyfiles(struct parser *p,struct document *d,const char *section)
{
    if(*section=='@') return copyfile(p,d,section+1,section+1);
    if(!has_section(d,section)) return fail(p,"Missing CopyFiles section.");
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) {
        char text[DP_LINE]; char *v[8];
        int n=fields(d,d->rows[i].text,text,v,8); if(n<1 || (n>2 && *v[2]) || (n>3 && *v[3] && !equal(v[3],"0")) || n>4) return fail(p,"Unsupported CopyFiles operation.");
        if(copyfile(p,d,v[0],n>1&&*v[1]?v[1]:v[0])) return -EINVAL;
    }
    return 0;
}
static int walk(struct parser *,struct document *,const char *,const char *,int);
static int service(struct parser *p,struct document *d,char **v,int n)
{
    struct ndp_manifest *m=&p->plan->manifest; uint32_t flags;
    if(n!=3 || number(v[1],&flags) || flags!=2 || !ndp_component(v[0]) || m->service[0]) return fail(p,"Exactly one associated miniport service is required.");
    copy(m->service,sizeof(m->service),v[0]);
    char binary[192],type[32]; uint32_t t;
    if(expanded_value(d,v[2],"ServiceType",type,sizeof(type)) || number(type,&t) || t!=1 ||
       expanded_value(d,v[2],"ServiceBinary",binary,sizeof(binary)) || !prefix(binary,"%12%\\") || !ndp_component(binary+5)) return fail(p,"Service must be an NT kernel .sys in DIRID 12.");
    const char *ext=strrchr(binary,'.'); if(!ext || !equal(ext,".sys")) return fail(p,"Service binary is not a .sys file.");
    int index=-1;
    for(unsigned i=0;i<m->file_count;i++) if(equal(m->files[i].name,binary+5)) index=(int)i;
    if(index<0) { if(copyfile(p,d,binary+5,binary+5)) return -EINVAL; index=(int)m->file_count-1; }
    m->binary=(uint32_t)index;
    return walk(p,d,v[2],"service/",2);
}
static int walk(struct parser *p,struct document *d,const char *section,const char *view,int mode)
{
    char identity[128]; if(snprintf(identity,sizeof(identity),"%s:%s",d->name,section)>=(int)sizeof(identity)) return fail(p,"Dependency name too long.");
    if(p->depth==16) return fail(p,"INF dependency depth exceeded.");
    for(unsigned i=0;i<p->depth;i++) if(equal(p->stack[i],identity)) return fail(p,"Include/Needs dependency cycle.");
    if(!has_section(d,section)) return fail(p,"Unresolved Needs or install section.");
    copy(p->stack[p->depth++],128,identity);
    struct document *included[DP_DOCS]; unsigned includes=0; int needs=0;
    /* Load Includes before Needs regardless of textual order. */
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) {
        char line[DP_LINE]; copy(line,sizeof(line),d->rows[i].text); char *eq=strchr(line,'='); if(!eq) return fail(p,"Malformed install directive."); *eq++=0;
        if(equal(trim(line),"Include")) {
            char text[DP_LINE]; char *v[16];
            int n=fields(d,eq,text,v,16); if(n<1) return fail(p,"Malformed Include list.");
            for(int j=0;j<n;j++) { if(includes==DP_DOCS) return fail(p,"Too many Include dependencies."); included[includes]=load(p,v[j]); if(!included[includes++]) return -EINVAL; }
        }
    }
    for(unsigned i=0;i<d->count;i++) if(equal(d->rows[i].section,section)) {
        char line[DP_LINE],text[DP_LINE]; copy(line,sizeof(line),d->rows[i].text); char *eq=strchr(line,'='); if(!eq) return fail(p,"Malformed install directive."); *eq++=0;
        char *key=trim(line);
        char *v[32]; int n=fields(d,trim(eq),text,v,32); if(n<1) return fail(p,"Malformed directive list.");
        if(equal(key,"Include")) continue;
        if(equal(key,"Needs")) {
            needs=1;
            for(int j=0;j<n;j++) {
                struct document *owner=NULL;
                if(has_section(d,v[j])) owner=d;
                for(unsigned k=0;k<includes;k++) if(has_section(included[k],v[j])) { if(owner && owner!=included[k]) return fail(p,"Ambiguous Include/Needs section."); owner=included[k]; }
                if(!owner || walk(p,owner,v[j],view,mode)) return fail(p,"Unresolved or cyclic Include/Needs dependency.");
            }
        } else if(equal(key,"CopyFiles") && mode==0) {
            for(int j=0;j<n;j++) if(copyfiles(p,d,v[j])) return -EINVAL;
        } else if(equal(key,"AddReg")) {
            for(int j=0;j<n;j++) if(addreg(p,d,v[j],view)) return -EINVAL;
        } else if(equal(key,"AddService") && mode==1) {
            if(service(p,d,v,n)) return -EINVAL;
        } else if(mode==2 && (equal(key,"ServiceBinary") || equal(key,"ServiceType") || equal(key,"StartType") || equal(key,"ErrorControl") || equal(key,"DisplayName") || equal(key,"LoadOrderGroup"))) {
            /* Metadata only; it never controls the host's lifecycle. */
        } else if(mode==0 && (equal(key,"Characteristics") || equal(key,"BusType"))) {
            uint32_t number_value; if(n!=1 || number(v[0],&number_value) ||
                (equal(key,"Characteristics") ? number_value!=4 : number_value!=5)) return fail(p,"Only physical PCI Ethernet adapters are eligible.");
        } else return fail(p,"Unsupported install directive; helpers and arbitrary services are prohibited.");
    }
    if(includes && !needs) return fail(p,"Include without Needs is not an install permission.");
    p->depth--; return 0;
}
static struct ndp_parameter *find_param(struct ndp_manifest *m,const char *key,const char *name)
{ for(unsigned i=0;i<m->parameter_count;i++) if(equal(m->parameters[i].key,key) && equal(m->parameters[i].name,name)) return &m->parameters[i]; return NULL; }
static void ascii_label(char *s)
{
    /* build_hwids.ascii_name: printable label family, normalized spaces,
     * deterministic 64-byte display name; never used to rewrite paths/IDs. */
    size_t at=0; int space=0;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) {
        unsigned char c=*p;
        if(c==' ' || c=='\t' || c=='\r') { space=at!=0; continue; }
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr(".,()_+&/#:\\-",c))) continue;
        if(space && at<64) s[at++]=' ';
        space=0; if(at==64) break; s[at++]=(char)c;
    }
    while(at && (s[at-1]==' ' || s[at-1]=='.')) at--;
    s[at]=0;
}
static int defaults(struct parser *p)
{
    struct ndp_manifest *m=&p->plan->manifest; unsigned count=m->parameter_count;
    for(unsigned i=0;i<count;i++) {
        struct ndp_parameter item=m->parameters[i];
        const char *stem="adapter/Ndi\\Params\\";
        if(!prefix(item.key,stem) || !equal(item.name,"default")) continue;
        const char *name=item.key+strlen(stem); if(!ndp_component(name) || item.type!=NDP_STRING) return fail(p,"Invalid Ndi parameter default.");
        struct ndp_parameter *type=find_param(m,item.key,"type"); if(!type || type->type!=NDP_STRING) return fail(p,"Ndi default has no string type.");
        char *v[]={"HKR","",(char *)name,"0",(char *)item.data};
        if(equal((char *)type->data,"int")) {
            uint32_t number_value; if(number(v[4],&number_value)) return fail(p,"Invalid integer default.");
            struct ndp_parameter *min=find_param(m,item.key,"min"),*max=find_param(m,item.key,"max"); uint32_t bound;
            if((min && (min->type!=NDP_STRING || number((char *)min->data,&bound) || number_value<bound)) ||
               (max && (max->type!=NDP_STRING || number((char *)max->data,&bound) || number_value>bound))) return fail(p,"Integer default outside declared range.");
            v[3]="65537";
        } else if(equal((char *)type->data,"enum")) {
            char key[128]; if(snprintf(key,sizeof(key),"%s\\enum",item.key)>=(int)sizeof(key) || !find_param(m,key,v[4])) return fail(p,"Default is absent from Ndi enumeration.");
        } else if(!equal((char *)type->data,"edit")) return fail(p,"Unsupported Ndi default type.");
        if(!find_param(m,"adapter/",name) && parameter(p,"adapter/",v,5)) return -EINVAL;
    }
    return 0;
}
int dp_parse(struct dp_plan *plan,const char *inf,const struct ndp_device *device,unsigned minor,dp_fetch fetch,void *ctx,char reason[160])
{
    if(!plan || !device || !fetch || !reason || minor>2) return -EINVAL;
    memset(plan,0,sizeof(*plan)); reason[0]=0;
    struct parser *p=calloc(1,sizeof(*p)); if(!p) { snprintf(reason,160,"Not enough memory for INF parser."); return -ENOMEM; }
    p->plan=plan; p->fetch=fetch; p->ctx=ctx; p->reason=reason;
    struct ndp_manifest *m=&plan->manifest; m->version=NDP_VERSION; m->size=sizeof(*m); m->device=*device;
    int result=-EINVAL; struct document *d=load(p,inf); if(!d) goto done;
    char class_name[64],signature[64];
    if(expanded_value(d,"Version","Class",class_name,sizeof(class_name)) || !equal(class_name,"Net") ||
       expanded_value(d,"Version","Signature",signature,sizeof(signature)) || !equal(signature,"$Windows NT$")) { fail(p,"Only NT Net-class packages are eligible."); goto done; }
    if(copy(m->inf,sizeof(m->inf),inf) || expanded_value(d,"Version","Provider",m->provider,sizeof(m->provider)) ||
       expanded_value(d,"Version","DriverVer",m->driver_version,sizeof(m->driver_version))) { fail(p,"Missing or oversized driver metadata."); goto done; }
    if(select_model(p,d,minor) || walk(p,d,m->install,"adapter/",0) || walk(p,d,m->services,"service/",1) || !m->service[0] || defaults(p)) goto done;
    ascii_label(m->name); ascii_label(m->provider);
    if(!m->name[0] || !m->provider[0]) { fail(p,"Missing display name or provider."); goto done; }
    struct ndp_parameter *address=find_param(m,"adapter/","NetworkAddress");
    if(address) {
        uint8_t mac[6]; unsigned any=0;
        if(address->type!=NDP_STRING || address->bytes!=13) { fail(p,"Invalid NetworkAddress override."); goto done; }
        for(unsigned i=0;i<6;i++) { char byte[3]={(char)address->data[i*2],(char)address->data[i*2+1],0}; if(hexbyte(byte,&mac[i])) { fail(p,"Invalid NetworkAddress override."); goto done; } any|=mac[i]; }
        if(!any || (mac[0]&1)) { fail(p,"NetworkAddress override must be nonzero unicast."); goto done; }
    }
    for(unsigned i=0;i<m->file_count;i++) {
        const char *ext=strrchr(m->files[i].name,'.');
        if(ext && equal(ext,".sys") && i!=m->binary) { fail(p,"Additional kernel service binary is prohibited."); goto done; }
    }
    /* The INF basename is descriptive; BDF makes each adapter's package unique. */
    char stem[33]; size_t len=strcspn(inf,"."); if(len>24) len=24; memcpy(stem,inf,len); stem[len]=0;
    if(snprintf(m->package,sizeof(m->package),"%s-%02x-%02x-%x",stem,device->bus,device->slot,device->function)>=(int)sizeof(m->package) || !ndp_component(m->package)) { fail(p,"Unsafe package directory."); goto done; }
    copy(m->reason,sizeof(m->reason),NDP_DISABLED); result=0;
 done:
    for(unsigned i=0;i<p->count;i++) free(p->docs[i].text);
    free(p); return result;
}
