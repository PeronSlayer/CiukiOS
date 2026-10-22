/* Incremental HTTP/1.1 framing and bounded content decoding. SPDX-License-Identifier: MIT */
#include "http.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
enum { STATUS, HEADERS, BODY, CHUNK_SIZE, CHUNK_DATA, CHUNK_CRLF, TRAILERS, END };
static bool fail(struct ciuki_http *p,const char *text) { p->error=text;return false; }
static bool emit(struct ciuki_http *p,enum ciuki_http_event e,const void *data,size_t n)
{
    if (!p->callback(p->context,e,data,n)) { p->cancelled=true;return false; }
    return true;
}
static bool token(unsigned c)
{ return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||strchr("!#$%&'*+-.^_`|~",(int)c)!=NULL; }
static bool named(const char *line,size_t bytes,const char *name)
{ size_t n=strlen(name);return bytes>n && line[n]==':' && !strncasecmp(line,name,n); }
bool ciuki_http_field(const char *s,size_t n,bool request)
{
    size_t i=0;
    while (i<n && s[i]!=':') { if (!s[i] || !token((unsigned char)s[i])) return false;i++; }
    if (!i || i==n) return false;
    for (size_t j=i+1;j<n;j++) if (((unsigned char)s[j]<32 && s[j]!='\t') || (unsigned char)s[j]==127) return false;
    if (request) {
        static const char *const reserved[]={"Host","Connection","Content-Length","Transfer-Encoding","Accept-Encoding",
            "Authorization","Proxy-Authorization","Proxy-Connection","Cookie","Upgrade","Expect","TE","Trailer"};
        for (size_t j=0;j<sizeof(reserved)/sizeof(*reserved);j++) if (named(s,n,reserved[j])) return false;
    }
    return true;
}
void ciuki_http_init(struct ciuki_http *p,bool head,ciuki_http_callback cb,void *ctx)
{ memset(p,0,sizeof(*p));p->head=head;p->callback=cb;p->context=ctx; }
void ciuki_http_destroy(struct ciuki_http *p)
{ if (p->zactive) inflateEnd(&p->z);p->zactive=false; }
static bool complete(struct ciuki_http *p)
{
    if (p->zactive && !p->zend) return fail(p,"Truncated compressed HTTP response.");
    p->done=true;p->state=END;return true;
}
static bool decoded(struct ciuki_http *p,const void *bytes,size_t n)
{
    if (n>CIUKI_HTTP_BODY-p->body_bytes) return fail(p,"HTTP decoded body exceeds 8 MiB.");
    p->body_bytes+=n;
    return !n || emit(p,CIUKI_HTTP_DATA,bytes,n);
}
static bool body(struct ciuki_http *p,const unsigned char *s,size_t n,size_t *used,size_t *budget)
{
    *used=0;
    if (n>CIUKI_HTTP_BODY-p->wire_bytes) return fail(p,"HTTP encoded body exceeds 8 MiB.");
    if (!p->zactive) {
        if(n>*budget)n=*budget;
        *used=n;*budget-=n;p->wire_bytes+=n;return decoded(p,s,n);
    }
    if(p->zend && n){
        if(p->coding!=1)return fail(p,"Unexpected data after compressed HTTP response.");
        if(inflateReset2(&p->z,31)!=Z_OK)return fail(p,"Unable to reset HTTP gzip decoder.");
        p->zend=false;
    }
    p->z.next_in=(Bytef *)s;p->z.avail_in=(uInt)n;
    p->z.next_out=p->output;p->z.avail_out=(uInt)*budget;
    int r=inflate(&p->z,Z_NO_FLUSH);
    if(r!=Z_OK && r!=Z_STREAM_END && r!=Z_BUF_ERROR)return fail(p,"Invalid compressed HTTP response or checksum.");
    *used=n-p->z.avail_in;p->wire_bytes+=*used;
    size_t produced=*budget-p->z.avail_out;*budget-=produced;
    if(!decoded(p,p->output,produced))return false;
    p->zpending=!p->z.avail_out && r!=Z_STREAM_END;
    if(r==Z_STREAM_END){p->zend=true;if(p->z.avail_in && p->coding!=1)return fail(p,"Unexpected data after compressed HTTP response.");}
    return true;
}
static bool number(const char *s,size_t *out,unsigned base)
{
    size_t n=0;bool any=false;
    for (;*s;s++) {
        unsigned c=(unsigned char)*s,d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:base;
        if (d>=base || n>(CIUKI_HTTP_BODY-d)/base) return false;
        n=n*base+d;any=true;
    }
    *out=n;return any;
}
static bool connection(struct ciuki_http *p,const char *v)
{
    while (*v) {
        while (*v==' '||*v=='\t'||*v==',')v++;
        const char *e=v;while(*e && *e!=',')e++;
        size_t n=(size_t)(e-v);while(n && (v[n-1]==' '||v[n-1]=='\t'))n--;
        if (n==5 && !strncasecmp(v,"close",n))p->close=true;
        v=e;
    }
    return true;
}
static bool line(struct ciuki_http *p)
{
    char *s=p->line;
    size_t n=p->line_bytes-2;
    s[n]=0;
    if (p->state==STATUS) {
        if (n<12 || (strncmp(s,"HTTP/1.1 ",9) && strncmp(s,"HTTP/1.0 ",9)) ||
            s[9]<'1'||s[9]>'5'||s[10]<'0'||s[10]>'9'||s[11]<'0'||s[11]>'9'||(n>12 && s[12]!=' '))
            return fail(p,"Invalid HTTP status line.");
        p->status=(unsigned)(s[9]-'0')*100+(unsigned)(s[10]-'0')*10+(unsigned)(s[11]-'0');
        p->http10=s[7]=='0';p->state=HEADERS;
    } else if (p->state==CHUNK_SIZE) {
        char *semi=strchr(s,';');
        if (semi) { for (char *v=semi+1;*v;v++) if ((unsigned char)*v<32||(unsigned char)*v==127)return fail(p,"Invalid HTTP chunk extension.");*semi=0; }
        if (!number(s,&p->remaining,16))return fail(p,"Invalid or oversized HTTP chunk.");
        p->state=p->remaining?CHUNK_DATA:TRAILERS;return true;
    } else if (!n) {
        if (p->state==TRAILERS) return complete(p);
        if (p->status<200) {
            if(p->status==101 || ++p->interims>8 || p->length||p->transfer)return fail(p,"Unsupported HTTP interim response.");
            p->state=STATUS;p->coding=0;p->close=false;p->location[0]=0;return true;
        }
        if (p->length && p->transfer)return fail(p,"Conflicting HTTP Transfer-Encoding and Content-Length.");
        if (p->http10 && p->transfer)return fail(p,"Transfer-Encoding is invalid on HTTP/1.0.");
        if (p->status==204 && (p->length||p->transfer))return fail(p,"Invalid framing on HTTP 204 response.");
        if (!emit(p,CIUKI_HTTP_HEAD,NULL,0))return false;
        if (p->head || p->status==204 || p->status==304) { p->done=true;p->state=END;return true; }
        if (p->coding) {
            if (inflateInit2(&p->z,p->coding==1?31:15)!=Z_OK)return fail(p,"Unable to allocate HTTP decoder.");
            p->zactive=true;
        }
        p->state=p->transfer?CHUNK_SIZE:BODY;
        if (!p->length && !p->transfer)p->close=true;
        if (p->length && !p->remaining)return complete(p);
        return true;
    } else {
        if (++p->fields>CIUKI_HTTP_FIELDS || !ciuki_http_field(s,n,false))return fail(p,"Invalid or excessive HTTP headers.");
        char *v=strchr(s,':')+1;while(*v==' '||*v=='\t')v++;
        char *e=s+n;while(e>v && (e[-1]==' '||e[-1]=='\t'))*--e=0;
        if (p->state==TRAILERS) {
            if(named(s,n,"Content-Length")||named(s,n,"Transfer-Encoding")||named(s,n,"Content-Encoding")||named(s,n,"Host")||named(s,n,"Set-Cookie"))
                return fail(p,"Forbidden HTTP trailer.");
            return true;
        }
        if (named(s,n,"Content-Length")) {
            size_t count;if(!number(v,&count,10) || (p->length && count!=p->remaining))return fail(p,"Invalid or conflicting HTTP Content-Length.");
            p->length=true;p->remaining=count;
        } else if (named(s,n,"Transfer-Encoding")) {
            if(p->transfer || strcasecmp(v,"chunked"))return fail(p,"Unsupported HTTP Transfer-Encoding.");
            p->transfer=true;
        } else if (named(s,n,"Content-Encoding")) {
            if(p->coding || (strcasecmp(v,"gzip") && strcasecmp(v,"deflate")))return fail(p,"Unsupported HTTP Content-Encoding.");
            p->coding=!strcasecmp(v,"gzip")?1:2;
        } else if (named(s,n,"Connection"))connection(p,v);
        else if (named(s,n,"Location")) {
            size_t len=strlen(v);if(len>CIUKI_HTTP_URL || p->location[0])return fail(p,"Invalid HTTP redirect location.");
            memcpy(p->location,v,len+1);
        }
        /* Restore trailing OWS for faithful raw headers. */
        for (;e<s+n;e++)*e=' ';
    }
    if (p->status>=200) {
        s[n]='\r';s[n+1]='\n';s[n+2]=0;
        if (!emit(p,CIUKI_HTTP_HEADER,s,n+2))return false;
    }
    return true;
}
bool ciuki_http_pending(const struct ciuki_http *p)
{ return p->pending_bytes || p->zpending; }
bool ciuki_http_drain(struct ciuki_http *p)
{
    const unsigned char *s=p->pending;size_t count=p->pending_bytes,budget=CIUKI_HTTP_SLICE;
    if(p->error||p->cancelled)return false;
    while((count || p->zpending) && budget) {
        if(p->done)return fail(p,"Unexpected bytes after HTTP response.");
        if(p->state==BODY || p->state==CHUNK_DATA) {
            size_t n=count,used;
            if((p->length||p->state==CHUNK_DATA) && n>p->remaining)n=p->remaining;
            size_t before=budget;unsigned state=p->state;bool pending=p->zpending;
            if(!body(p,s,n,&used,&budget))return false;
            s+=used;count-=used;
            if(p->length||p->state==CHUNK_DATA) {
                p->remaining-=used;
                if(!p->remaining && !p->zpending) {
                    if(p->state==CHUNK_DATA){p->state=CHUNK_CRLF;p->crlf=0;}
                    else if(!complete(p))return false;
                }
            }
            /* A zero-input inflate can exhaust its buffered output exactly at
             * the slice boundary. Advancing to the next chunk is progress. */
            if(!used && before==budget && state==p->state && pending==p->zpending && (count||p->zpending))return fail(p,"Invalid compressed HTTP response.");
        } else if(p->state==CHUNK_CRLF) {
            if(*s++!=(p->crlf?'\n':'\r'))return fail(p,"Invalid HTTP chunk terminator.");
            count--;if(++p->crlf==2)p->state=CHUNK_SIZE;
        } else {
            unsigned c=*s++;count--;
            /* RFC 9112 section 7.1: chunk-size lines frame the body. Bound
             * each line, but do not spend the response-header allowance. */
            if(p->state!=CHUNK_SIZE && ++p->header_bytes>CIUKI_HTTP_HEADERS)return fail(p,"HTTP headers exceed 32 KiB.");
            if(p->line_bytes>=CIUKI_HTTP_LINE+2 || !c || c==127 || (c<32 && c!='\t' && c!='\r' && c!='\n'))return fail(p,"Invalid or oversized HTTP line.");
            if(c=='\n' && (!p->line_bytes || p->line[p->line_bytes-1]!='\r'))return fail(p,"HTTP requires CRLF lines.");
            if(p->line_bytes && p->line[p->line_bytes-1]=='\r' && c!='\n')return fail(p,"Invalid HTTP CRLF.");
            p->line[p->line_bytes++]=(char)c;
            if(c=='\n'){if(!line(p))return false;p->line_bytes=0;}
        }
    }
    if(count)memmove(p->pending,s,count);p->pending_bytes=count;
    return true;
}
bool ciuki_http_feed(struct ciuki_http *p,const void *bytes,size_t count)
{
    if(count>sizeof(p->pending) || ciuki_http_pending(p))return fail(p,"HTTP input slice exceeds the parser budget.");
    if(count)memcpy(p->pending,bytes,count);p->pending_bytes=count;
    return ciuki_http_drain(p);
}
bool ciuki_http_eof(struct ciuki_http *p)
{
    if(p->error||p->cancelled)return false;
    if(p->done)return true;
    if(p->state==BODY && !p->length)return complete(p);
    return fail(p,"Truncated HTTP response.");
}
bool ciuki_http_reusable(const struct ciuki_http *p)
{ return p->done && !p->error && !p->cancelled && !p->close && !p->http10; }
