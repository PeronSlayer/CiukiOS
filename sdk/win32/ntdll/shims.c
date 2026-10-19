/* Original bounded compatibility database. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shims.h>
const char *const w32_shim_names[12]={"version","ddraw-555","ddraw-scale","ddraw-windowed",
    "heap-zero","heap-pad","memstatus-cap","io-pit","io-gameport","dll-native","fpu-cw","w-stubs"};
const uint32_t w32_shim_versions[12]={1,1,1,1,1,1,1,1,1,1,1,1};
struct ciuki_w32_compat w32_shims;
const struct ciuki_w32_compat *w32_shim_current(void){return &w32_shims;}
static int compare_n(const char *a,const char *b,unsigned n,int fold){
    for(unsigned i=0;i<n;i++){unsigned x=(uint8_t)a[i],y=(uint8_t)b[i];
        if(fold){if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;}
        if(x!=y)return x<y?-1:1;if(!x)return 0;
    }return 0;
}
static int number(const char *s,uint32_t *out) {
    uint32_t n=0,base=10;unsigned i=0;
    if(s[0]=='0'&&s[1]=='x'){base=16;i=2;}
    if(!s[i])return 0;
    for(;s[i];i++){
        unsigned c=(uint8_t)s[i],v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
        if(v>=base||n>(UINT32_MAX-v)/base)return 0;
        n=n*base+v;
    }
    *out=n;return 1;
}
static int names(const char *s,int dll) {
    if(!*s)return 0;
    unsigned count=0;
    for(const char *p=s;;){
        const char *q=p;while(*q&&*q!='|'){
            unsigned c=(uint8_t)*q;
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||(!dll&&(c=='@'||c=='?'||c=='$'))))return 0;
            q++;
        }
        unsigned n=(unsigned)(q-p);
        if(!n||n>127||++count>16)return 0;
        if(dll){
            if(n<5||compare_n(q-4,".dll",4,1))return 0;
            const char *core[]={"ntdll.dll","kernel32.dll","user32.dll","gdi32.dll"};
            for(unsigned j=0;j<4;j++)if(w32_strlen(core[j])==n&&!compare_n(p,core[j],n,1))return 0;
        }else if(q[-1]!='W')return 0;
        if(!*q)return 1;
        p=q+1;
    }
}
static int parameter(unsigned id,char *p,struct ciuki_w32_compat *c) {
    uint32_t n;
    switch(id){
    case W32_SH_VERSION:{
        const char *v[]={"95","me","nt4","2000","xp"};
        for(unsigned i=0;i<5;i++)if(!w32_strcmp(p,v[i])){c->version=i+1;return 1;}return 0;
    }
    case W32_SH_HEAP_PAD:if(!number(p,&n)||n>65536)return 0;c->heap_pad=n;return 1;
    case W32_SH_MEMSTATUS_CAP:if(!number(p,&n)||!n||n>4096)return 0;c->memory_mib=n;return 1;
    case W32_SH_FPU_CW:
        if(!number(p,&n)||n>65535||(n&~0x1f7fu)||(n&0x300)==0x100||!(n&0x40))return 0;
        c->fpu_cw=n;return 1;
    case W32_SH_DDRAW_SCALE:
        if(number(p,&n)&&n&&n<=8){c->scale=n;return 1;}
        if(compare_n(p,"mode:",5,1))return 0;
        {char *x=p+5;while(*x&&*x!='x')x++;if(!*x)return 0;*x++=0;
        if(!number(p+5,&c->width)||!number(x,&c->height)||c->width<320||c->width>2048||c->height<200||c->height>2048)return 0;return 1;}
    case W32_SH_DLL_NATIVE:return names(p,1)&&w32_copy_string(c->native_dlls,sizeof(c->native_dlls),p);
    case W32_SH_W_STUBS:return names(p,0)&&w32_copy_string(c->w_exports,sizeof(c->w_exports),p);
    default:return !*p;
    }
}
int w32_shim_list(const char *s,struct ciuki_w32_compat *out) {
    struct ciuki_w32_compat c={0};
    if(!s||w32_strlen(s)>=W32_SHIM_LINE_MAX)return 0;
    while(*s){
        char name[64],arg[513];unsigned n=0;
        while(*s&&*s!='@'){if(n+1>=sizeof(name))return 0;name[n++]=*s++;}name[n]=0;
        if(*s++!='@')return 0;
        char version[16];n=0;
        while(*s&&*s!='('&&*s!=','){if(n+1>=sizeof(version))return 0;version[n++]=*s++;}version[n]=0;
        uint32_t v;if(!number(version,&v))return 0;
        unsigned id=0;while(id<12&&w32_strcmp(name,w32_shim_names[id]))id++;
        if(id==12||v!=w32_shim_versions[id]||(c.mask&W32_SHIM_BIT(id)))return 0;
        n=0;arg[0]=0;
        if(*s=='('){s++;while(*s&&*s!=')'){if(n+1>=sizeof(arg))return 0;arg[n++]=*s++;}arg[n]=0;if(*s++!=')')return 0;}
        if(!parameter(id,arg,&c))return 0;c.mask|=W32_SHIM_BIT(id);
        if(*s&&*s!=',')return 0;
        if(*s==','){s++;if(!*s)return 0;}
    }
    *out=c;return 1;
}
struct entry {char exe[128],hash[65];uint32_t stamp,image,bytes,keys;struct ciuki_w32_compat settings;};
static const char *base_name(const char *s){const char *b=s;for(;*s;s++)if(*s=='/'||*s=='\\')b=s+1;return b;}
static int match_entry(const struct entry *e,const struct w32_shim_identity *id,struct w32_shim_match *m) {
    if(!(e->keys&16)||!(e->keys&15))return 0;
    if(!w32_stricmp(e->exe,base_name(id->exe))&&(e->keys&8)&&w32_stricmp(e->hash,id->sha256))m->conflicts++;
    if(w32_stricmp(e->exe,base_name(id->exe))||
        ((e->keys&1)&&e->stamp!=id->timestamp)||((e->keys&2)&&e->image!=id->image_size)||
        ((e->keys&4)&&e->bytes!=id->file_size)||((e->keys&8)&&w32_stricmp(e->hash,id->sha256)))return 1;
    uint32_t rank=e->keys&8?4:(e->keys&3)==3?3:e->keys&3?2:1;
    if(m->rank){
        const uint8_t *a=(const void *)&m->settings,*b=(const void *)&e->settings;
        for(unsigned i=0;i<sizeof(e->settings);i++)if(a[i]!=b[i]){m->conflicts++;break;}
    }
    if(rank>m->rank){m->rank=rank;m->settings=e->settings;}return 1;
}
int w32_shim_parse(const void *input,uint32_t bytes,const struct w32_shim_identity *id,struct w32_shim_match *out) {
    if(bytes>W32_SHIM_TEXT_MAX||(!input&&bytes)||!id||!id->exe)return 0;
    const uint8_t *text=input;struct w32_shim_match m={0};struct entry e={0};unsigned entries=0;
    for(uint32_t at=0;at<bytes;){
        char line[W32_SHIM_LINE_MAX];unsigned n=0;
        while(at<bytes&&text[at]!='\n'){
            unsigned c=text[at++];if(c=='\r'){if(at<bytes&&text[at]=='\n')break;return 0;}
            if((c<32&&c!='\t')||c>126||n+1>=sizeof(line))return 0;line[n++]=(char)c;
        }
        if(at<bytes&&text[at]=='\n')at++;
        while(n&&(line[n-1]==' '||line[n-1]=='\t'))n--;line[n]=0;
        char *p=line;while(*p==' '||*p=='\t')p++;
        if(!*p||*p==';'||*p=='#')continue;
        n=w32_strlen(p);
        if(*p=='['){
            if(entries&&!match_entry(&e,id,&m))return 0;
            if(++entries>256||n<3||p[n-1]!=']'||n-2>=sizeof(e.exe))return 0;
            w32_fill(&e,0,sizeof(e));p[n-1]=0;
            for(char *q=p+1;*q;q++)if(*q=='/'||*q=='\\'||*q==':'||*q=='['||*q==']')return 0;
            w32_copy_string(e.exe,sizeof(e.exe),p+1);continue;
        }
        if(!entries)return 0;
        char *v=p;while(*v&&*v!='=')v++;if(!*v)return 0;
        char *tail=v;while(tail>p&&(tail[-1]==' '||tail[-1]=='\t'))tail--;*tail=0;v++;
        while(*v==' '||*v=='\t')v++;
        unsigned bit=!w32_strcmp(p,"TimeDateStamp")?1:!w32_strcmp(p,"SizeOfImage")?2:
            !w32_strcmp(p,"file-size")?4:!w32_strcmp(p,"sha256")?8:!w32_strcmp(p,"shims")?16:0;
        if(!bit||(e.keys&bit))return 0;e.keys|=bit;
        if(bit==16){if(!w32_shim_list(v,&e.settings))return 0;}
        else if(bit==8){
            if(w32_strlen(v)!=64)return 0;
            for(unsigned i=0;i<64;i++){unsigned c=(uint8_t)v[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return 0;}
            w32_copy(e.hash,v,65);
        }else if(!number(v,bit==1?&e.stamp:bit==2?&e.image:&e.bytes))return 0;
    }
    if(entries&&!match_entry(&e,id,&m))return 0;
    *out=m;return 1;
}
static int in_list(const char *list,const char *name,int fold){
    unsigned n=w32_strlen(name);
    for(const char *p=list;*p;){const char *q=p;while(*q&&*q!='|')q++;
        if((unsigned)(q-p)==n&&!compare_n(p,name,n,fold))return 1;
        p=*q?q+1:q;
    }return 0;
}
int w32_shim_native(const char *name){return in_list(w32_shims.native_dlls,name,1);}
int w32_shim_w_stub(const char *name){return in_list(w32_shims.w_exports,name,0);}
static int add(char *out,uint32_t cap,unsigned *at,const char *s){unsigned n=w32_strlen(s);if(*at>=cap||n>=cap-*at)return 0;w32_copy(out+*at,s,n+1);*at+=n;return 1;}
static void decimal(char out[16],uint32_t n){char tmp[16];unsigned k=0,j=0;do{tmp[k++]=(char)('0'+n%10);n/=10;}while(n);while(k)out[j++]=tmp[--k];out[j]=0;}
int w32_shim_format(const struct ciuki_w32_compat *c,char *out,uint32_t cap){
    unsigned at=0;if(!cap)return 0;out[0]=0;
    for(unsigned i=0;i<12;i++)if(c->mask&W32_SHIM_BIT(i)){
        if(at&&!add(out,cap,&at,","))return 0;
        if(!add(out,cap,&at,w32_shim_names[i])||!add(out,cap,&at,"@1"))return 0;
        char n[16];const char *p=0;
        if(i==W32_SH_VERSION){const char *v[]={"","95","me","nt4","2000","xp"};if(c->version>5)return 0;p=v[c->version];}
        else if(i==W32_SH_DLL_NATIVE)p=c->native_dlls;
        else if(i==W32_SH_W_STUBS)p=c->w_exports;
        else if(i==W32_SH_HEAP_PAD||i==W32_SH_MEMSTATUS_CAP||i==W32_SH_FPU_CW){decimal(n,i==W32_SH_HEAP_PAD?c->heap_pad:i==W32_SH_MEMSTATUS_CAP?c->memory_mib:c->fpu_cw);p=n;}
        else if(i==W32_SH_DDRAW_SCALE){
            if(c->scale){decimal(n,c->scale);p=n;}
            else {if(!add(out,cap,&at,"(mode:"))return 0;decimal(n,c->width);if(!add(out,cap,&at,n)||!add(out,cap,&at,"x"))return 0;decimal(n,c->height);if(!add(out,cap,&at,n)||!add(out,cap,&at,")"))return 0;}
        }
        if(p&&(!add(out,cap,&at,"(")||!add(out,cap,&at,p)||!add(out,cap,&at,")")))return 0;
    }
    return 1;
}
void w32_shim_version(struct ciuki_w32_identity *a){
    if(!(w32_shims.mask&W32_SHIM_BIT(W32_SH_VERSION)))return;
    const uint32_t versions[5][4]={{4,0,950,1},{4,90,3000,1},{4,0,1381,2},{5,0,2195,2},{5,1,2600,2}};
    if(!w32_shims.version||w32_shims.version>5)return;
    const uint32_t *v=versions[w32_shims.version-1];a->major=v[0];a->minor=v[1];a->build=v[2];a->platform=v[3];
    a->get_version=v[0]|v[1]<<8|(v[3]==1?0x80000000u:0)|(v[2]&0x7fff)<<16;w32_fill(a->csd,0,sizeof(a->csd));
}
uint32_t w32_shim_stub(volatile uint32_t *once,const char *dll,const char *name,uint32_t convention){
    if(!__atomic_exchange_n(once,1,__ATOMIC_RELAXED))w32_diag("stub",dll,name);
    NtCurrentTeb()->LastErrorValue=convention==5?5:120;
    return convention==1?0x80004001u:convention==2?8:convention==3?120:convention==4?UINT32_MAX:convention==6?0x7fffffffu:0;
}
