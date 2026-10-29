/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HEADER 40u
#define COMMITTED 0x43524731u
static const char hex[] = "0123456789abcdef";
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8)); }
static int digit(unsigned c) { return c>='0'&&c<='9' ? (int)c-'0' : c>='a'&&c<='f' ? (int)c-'a'+10 : -1; }
static char *encode(char *p, const char *s)
{
    for (;*s;s++) {
        unsigned c=(unsigned char)*s;
        if (c<32 || c==127 || c=='\\' || c=='[' || c==']' || c=='=' || c==':') { *p++='\\';*p++='x';*p++=hex[c>>4];*p++=hex[c&15]; }
        else *p++=*s;
    }
    return p;
}
static int decode(char *out, size_t cap, const char *s, size_t n)
{
    size_t at=0;
    for (size_t i=0;i<n;i++) {
        unsigned c=(unsigned char)s[i];
        if (c=='\\') {
            if (n-i<4 || s[i+1]!='x' || digit(s[i+2])<0 || digit(s[i+3])<0) return 0;
            c=(unsigned)(digit(s[i+2])*16+digit(s[i+3]));i+=3;
        } else if (c<32 || c==127 || c=='[' || c==']' || c=='=' || c==':') return 0;
        if (!c || at+1>=cap) return 0;
        out[at++]=(char)c;
    }
    out[at]=0;return 1;
}
static char *path_text(char *p, struct wr_key *k)
{
    /* Bound path depth independently of attacker-controlled persistent text. */
    struct wr_key *stack[256]; unsigned n=0;
    for (;k;k=k->parent) { if(n==256) return NULL;stack[n++]=k; }
    while(n) { p=encode(p,stack[--n]->name);if(n) { memcpy(p,"\\x5c",4);p+=4; } }
    return p;
}
static uint8_t *serialise(struct wr_server *s, unsigned h, size_t *bytes)
{
    size_t expected=s->hive[h].text;
    if(expected>W32R_TEXT) return NULL;
    uint8_t *buffer=calloc(1,HEADER+expected);if(!buffer)return NULL;
    char *p=(char *)buffer+HEADER;
    for(struct wr_key *k=s->keys;k;k=k->next) if(k->hive==h && !k->deleted) {
        *p++='[';p=path_text(p,k);if(!p){free(buffer);return NULL;}*p++=']';*p++='\n';
        for(struct wr_value *v=k->values;v;v=v->next) {
            p=encode(p,v->name);*p++='=';
            for(int i=7;i>=0;i--)*p++=hex[(v->type>>(i*4))&15];*p++=':';
            for(uint32_t i=0;i<v->bytes;i++){*p++=hex[v->data[i]>>4];*p++=hex[v->data[i]&15];}*p++='\n';
        }
    }
    if((size_t)(p-(char *)buffer)!=HEADER+expected){free(buffer);return NULL;}
    memcpy(buffer,"CRG1",4);put32(buffer+4,1);put32(buffer+8,h);
    uint64_t sequence=s->hive[h].sequence+1;put32(buffer+12,(uint32_t)sequence);put32(buffer+16,(uint32_t)(sequence>>32));
    put32(buffer+20,(uint32_t)expected);put32(buffer+24,wr_crc(buffer+HEADER,expected));put32(buffer+28,wr_crc(buffer,28));
    *bytes=HEADER+expected;return buffer;
}
static int valid(const uint8_t *b,size_t n,unsigned h,uint64_t *seq)
{
    if(n<HEADER || memcmp(b,"CRG1",4) || get32(b+4)!=1 || get32(b+8)!=h ||
       get32(b+20)>W32R_TEXT || n!=HEADER+(size_t)get32(b+20) || get32(b+36) ||
       get32(b+28)!=wr_crc(b,28) || get32(b+24)!=wr_crc(b+HEADER,n-HEADER) || get32(b+32)!=COMMITTED) return 0;
    *seq=(uint64_t)get32(b+12)|((uint64_t)get32(b+16)<<32);
    return *seq && *seq!=UINT64_MAX;
}
static int parse_body(struct wr_server *s,unsigned h,const uint8_t *text,size_t n,char *path)
{
    struct wr_key *key=NULL;const char *p=(const char *)text,*end=p+n;
    while(p<end) {
        const char *e=memchr(p,'\n',(size_t)(end-p));if(!e || memchr(p,0,(size_t)(e-p)))return 0;
        if(*p=='[') {
            if(e-p<2 || e[-1]!=']' || !decode(path,W32R_PATH,p+1,(size_t)(e-p-2)))return 0;
            char *component=path,*slash=strchr(component,'\\');
            if(slash)*slash=0;
            if(strcmp(component,h ? "HKU" : "HKLM"))return 0;
            if(!s->roots[h]) s->roots[h]=wr_add_key(s,NULL,component,h);
            key=s->roots[h];if(!key)return 0;
            unsigned depth=0;
            while(slash) {
                component=slash+1;slash=strchr(component,'\\');if(slash)*slash=0;
                if(!*component || !wr_name(component) || ++depth>254)return 0;
                struct wr_key *child=wr_child(s,key,component);
                if(!child)child=wr_add_key(s,key,component,h);
                if(!child)return 0;key=child;
            }
            if(key->seen)return 0;key->seen=1;
        } else {
            if(!key)return 0;
            const char *eq=memchr(p,'=',(size_t)(e-p));char name[W32R_NAME];
            if(!eq || e-eq<10 || eq[9]!=':' || !decode(name,sizeof(name),p,(size_t)(eq-p)) || !wr_name(name))return 0;
            for(struct wr_value *v=key->values;v;v=v->next)if(wr_equal(v->name,name))return 0;
            uint32_t type=0;for(unsigned i=1;i<=8;i++){int d=digit(eq[i]);if(d<0)return 0;type=(type<<4)|(unsigned)d;}
            size_t digits=(size_t)(e-eq-10);if(digits%2 || digits/2>W32R_DATA)return 0;
            uint8_t *data=malloc(digits/2+1);if(!data)return 0;
            int ok=1;for(size_t i=0;i<digits;i+=2){int a=digit(eq[10+i]),b=digit(eq[11+i]);if(a<0||b<0){ok=0;break;}data[i/2]=(uint8_t)(a*16+b);}
            uint32_t err=ok?wr_put(s,key,name,type,data,(uint32_t)(digits/2)):87;free(data);if(err)return 0;
        }
        p=e+1;
    }
    return s->roots[h]!=NULL;
}
static int parse(struct wr_server *s,unsigned h,const uint8_t *text,size_t n)
{
    char *path=malloc(W32R_PATH);if(!path)return 0;
    int ok=parse_body(s,h,text,n,path);free(path);return ok;
}
static void filename(struct wr_server *s,char *out,size_t cap,unsigned h,unsigned slot)
{
    if(s->secured){
        if(h&&s->user_path[0])snprintf(out,cap,"%s/Settings/Registry/USER.CRG.%u",s->user_path,slot);
        else snprintf(out,cap,"/System/Security/Registry/Machine/%s.CRG.%u",h?"USER":"SYSTEM",slot);
        return;
    }
    snprintf(out,cap,"%s%s%s.CRG.%u",h && s->user_path[0] ? s->user_path : "",
        h && s->user_path[0] ? "/" : "",h?"USER":"SYSTEM",slot);
}
uint32_t wr_commit(struct wr_server *s,unsigned h)
{
    struct wr_hive *v=&s->hive[h];if(!v->dirty)return 0;
    if(v->sequence>=UINT64_MAX-1)return 1016;
    size_t n;uint8_t *b=serialise(s,h,&n);if(!b)return 8;
    char name[160];unsigned slot=v->sequence ? 1-v->slot : 0;filename(s,name,sizeof(name),h,slot);
    uint8_t mark[4];put32(mark,COMMITTED);
    int ok=!s->io.write(s->io.opaque,name,b,n) && !s->io.sync(s->io.opaque,name) &&
           !s->io.patch(s->io.opaque,name,32,mark,4) && !s->io.sync(s->io.opaque,name);
    free(b);
    if(!ok){s->io.log(s->io.opaque,"[win32] registry commit-failed");return 1016;}
    v->sequence++;v->slot=slot;v->dirty=0;return 0;
}
int wr_load_hive(struct wr_server *s,unsigned h)
{
    {
        uint8_t *b[2]={NULL,NULL};size_t n[2]={0,0};uint64_t seq[2]={0,0};int status[2],ok[2]={0,0};
        struct wr_server candidate[2];memset(candidate,0,sizeof(candidate));
        for(unsigned i=0;i<2;i++) {
            char name[160];filename(s,name,sizeof(name),h,i);status[i]=s->io.read(s->io.opaque,name,&b[i],&n[i]);
            if(status[i]<0){free(b[0]);free(b[1]);wr_destroy(&candidate[0]);wr_destroy(&candidate[1]);return 0;}
            if(status[i]>0 && valid(b[i],n[i],h,&seq[i]))ok[i]=parse(&candidate[i],h,b[i]+HEADER,n[i]-HEADER);
        }
        int chosen=ok[1]&&(!ok[0]||seq[1]>seq[0])?1:ok[0]?0:-1;
        if((status[0]>0&&!ok[0])||(status[1]>0&&!ok[1]))s->io.log(s->io.opaque,"[win32] registry invalid-slot fallback");
        if(chosen<0) {
            if(status[0]||status[1]){free(b[0]);free(b[1]);wr_destroy(&candidate[0]);wr_destroy(&candidate[1]);return 0;}
            s->roots[h]=wr_add_key(s,NULL,h?"HKU":"HKLM",h);if(!s->roots[h])return 0;
        } else {
            struct wr_server *c=&candidate[chosen];s->roots[h]=c->roots[h];
            struct wr_key **tail=&s->keys;while(*tail)tail=&(*tail)->next;*tail=c->keys;c->keys=NULL;
            s->hive[h]=c->hive[h];s->hive[h].sequence=seq[chosen];s->hive[h].slot=(unsigned)chosen;s->hive[h].dirty=0;
        }
        free(b[0]);free(b[1]);wr_destroy(&candidate[0]);wr_destroy(&candidate[1]);
    }
    return 1;
}
int wr_load(struct wr_server *s)
{
    for (unsigned h=0;h<2;h++) if (!wr_load_hive(s,h)) return 0;
    s->roots[2]=wr_add_key(s,NULL,"HKCC",2);s->roots[3]=wr_add_key(s,NULL,"HKDD",3);
    return s->roots[2] && s->roots[3] && wr_defaults(s,640,480,16);
}

/* Load and validate the new hive before replacing any active identity.
 * A profile change is a G3 boundary: all announced Win32 programs end first. */
static int profile_name(const char *s)
{
    size_t n=strlen(s); if (n>=32) return 0;
    for (size_t i=0;i<n;i++) if (!((s[i]>='a' && s[i]<='z') ||
        (s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9') || s[i]=='_' || s[i]=='-')) return 0;
    return 1;
}
static struct wr_key *profile_key(struct wr_server *s,const char *path)
{
    struct wr_key *k=s->roots[1]; char part[256];
    while (*path) {
        const char *end=strchr(path,'\\'); size_t n=end ? (size_t)(end-path) : strlen(path);
        if (!n || n>=sizeof(part)) return NULL;
        memcpy(part,path,n); part[n]=0;
        struct wr_key *next=wr_child(s,k,part);
        k=next ? next : wr_add_key(s,k,part,1); if (!k) return NULL;
        if (!end) break; path=end+1;
    }
    return k;
}
static uint32_t profile_folders(struct wr_server *s,const char *root)
{
    /* sec-user-05: "Local AppData" (CSIDL_LOCAL_APPDATA) for profile hives
     * only; integ9: WinINet repositories follow the Windows 98 profile root. */
    static const char *const names[]={"Personal","Desktop","Programs","Start Menu","Favorites", "AppData","Recent","SendTo","Templates","Startup","Local AppData","Cache","Cookies","History"};
    static const char *const suffix[]={"Documents","Documents/Desktop","Start Menu/Programs","Start Menu","Favorites", "Application Data","Recent","SendTo","Templates","Start Menu/Programs/StartUp","Local Settings/Application Data","Temporary Internet Files","Cookies","History"};
    for (unsigned u=0;u<2;u++) {
        struct wr_key *k=profile_key(s,u ? ".DEFAULT\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders" :
            ".DEFAULT\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders");
        if (!k) return 8;
        for (unsigned i=0;i<sizeof(names)/sizeof(*names);i++) {
            char path[260]; uint8_t wide[520];
            if (!*root && i==10) continue; /* sec-user-05: profile hives only */
            if (*root) snprintf(path,sizeof(path),"C:%s/%s",root,suffix[i]);
            else if (i==0) snprintf(path,sizeof(path),"C:/Documents");
            else if (i==1) snprintf(path,sizeof(path),"C:/WINDOWS/Desktop");
            else snprintf(path,sizeof(path),"C:/WINDOWS/%s",suffix[i]);
            size_t n=strlen(path)+1;
            for (size_t b=0;b<n;b++) { wide[b*2]=(uint8_t)(path[b]=='/' ? '\\' : path[b]); wide[b*2+1]=0; }
            uint32_t e=wr_put(s,k,names[i],1,wide,(uint32_t)n*2); if (e) return e;
        }
    }
    return 0;
}
static uint32_t clear_template_startup(struct wr_server *s)
{
    static const char *const queues[]={"Run","RunOnce","RunServices","RunServicesOnce"};
    for(unsigned i=0;i<sizeof(queues)/sizeof(*queues);i++){
        char path[128];snprintf(path,sizeof(path),".DEFAULT\\Software\\Microsoft\\Windows\\CurrentVersion\\%s",queues[i]);
        struct wr_key *k=wr_walk(s,1,path);
        while(k&&k->values){uint32_t e=wr_remove_value(s,k,k->values->name);if(e)return e;}
    }
    return 0;
}
uint32_t wr_select_profile(struct wr_server *s,const char *name,const char *root)
{
    if (!profile_name(name) || strlen(root)>=sizeof(s->user_path)) return 87;
    if (*root) {
        const char *base="/Windows/Profiles/";size_t n=strlen(base);
        if(!*name||strncmp(root,base,n)||!root[n])return 87;
        if(root[n]=='u'){
            if(strlen(root)!=n+9)return 87;
            for(const char *p=root+n+1;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')))return 87;
        }else for(const char *p=root+n;*p;p++)if(*p<'0'||*p>'9')return 87;
    }
    if (!strcmp(s->user_name,name) && !strcmp(s->user_path,root)) return 0;
    if (s->live_count || s->handles || s->profile_owner) return 170;
    uint32_t e=wr_flush(s); if (e) return e;
    struct wr_server *c=calloc(1,sizeof(*c)); if (!c) return 8;
    c->io=s->io; c->now=s->now; c->secured=s->secured; strcpy(c->user_path,root);
    if (!wr_load_hive(c,1)) { wr_destroy(c); free(c); return 1016; }
    int created=!c->hive[1].sequence;
    if (*root && created) {
        /* No profile slots: discard its empty root and copy the default hive.
         * Corrupt existing slots fail above and are never replaced by defaults. */
        wr_destroy(c); c->io=s->io; c->now=s->now; c->secured=s->secured;
        if (!wr_load_hive(c,1)) { wr_destroy(c); free(c); return 1016; }
        strcpy(c->user_path,root); c->hive[1].sequence=0; c->hive[1].slot=0; c->hive[1].dirty=1;
    }
    /* Templates provide preferences, never ownership of deferred commands. */
    e=created&&s->secured ? clear_template_startup(c) : 0;
    if(!e)e=created ? profile_folders(c,root) : 0;
    if (!e) e=wr_commit(c,1);
    if (e) { wr_destroy(c); free(c); return e; }
    struct wr_key **at=&s->keys;
    while (*at) {
        struct wr_key *k=*at;
        if (k->hive!=1) { at=&k->next; continue; }
        *at=k->next;
        while (k->values) { struct wr_value *v=k->values; k->values=v->next; free(v->name); free(v->data); free(v); }
        free(k->name); free(k);
    }
    *at=c->keys; c->keys=NULL;
    s->roots[1]=c->roots[1]; s->hive[1]=c->hive[1];
    strcpy(s->user_path,root); strcpy(s->user_name,name);
    free(c);
    if (s->shell_profile) s->shell_profile(s->shell,root);
    return 0;
}
