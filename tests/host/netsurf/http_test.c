/* Shared production parser: fragmentation, bounds, decoding, cancellation.
 * SPDX-License-Identifier: MIT */
#include "sdk/netsurf/port/http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct received { size_t bytes,headers;bool cancel;unsigned heads;unsigned char data[256]; };
static bool callback(void *ctx,enum ciuki_http_event e,const void *s,size_t n)
{
    struct received *r=ctx;
    if(e==CIUKI_HTTP_DATA){if(r->bytes+n<=sizeof(r->data))memcpy(r->data+r->bytes,s,n);r->bytes+=n;}
    else if(e==CIUKI_HTTP_HEADER)r->headers++;
    else r->heads++;
    return !r->cancel;
}
static void example(const char *wire,const char *body,bool reusable)
{
    size_t len=strlen(wire);
    for(size_t split=1;split<=len;split++){
        struct ciuki_http p;struct received r={0};ciuki_http_init(&p,false,callback,&r);
        for(size_t i=0;i<len;i+=split)assert(ciuki_http_feed(&p,wire+i,len-i<split?len-i:split));
        assert(ciuki_http_eof(&p));assert(p.done && !p.error && r.heads==1);
        assert(r.bytes==strlen(body) && !memcmp(r.data,body,r.bytes));assert(ciuki_http_reusable(&p)==reusable);
        ciuki_http_destroy(&p);
    }
}
static void invalid(const char *wire)
{
    struct ciuki_http p;struct received r={0};ciuki_http_init(&p,false,callback,&r);
    bool result=ciuki_http_feed(&p,wire,strlen(wire));
    if(result)result=ciuki_http_eof(&p);
    assert(!result && p.error);assert(!ciuki_http_reusable(&p));ciuki_http_destroy(&p);
}
static void compressed(unsigned coding,bool chunked)
{
    const char *body="Ciuki compressed content";
    unsigned char wire[1024],encoded[256];z_stream z={0};
    assert(deflateInit2(&z,9,Z_DEFLATED,coding==1?31:15,8,Z_DEFAULT_STRATEGY)==Z_OK);
    z.next_in=(Bytef *)body;z.avail_in=(unsigned)strlen(body);z.next_out=encoded;z.avail_out=sizeof(encoded);
    assert(deflate(&z,Z_FINISH)==Z_STREAM_END);size_t n=z.total_out;deflateEnd(&z);
    int head=snprintf((char *)wire,sizeof(wire),"HTTP/1.1 200 OK\r\nContent-Encoding: %s\r\n%s",coding==1?"gzip":"deflate",chunked?"Transfer-Encoding: chunked\r\n\r\n":"");
    if(chunked)head+=snprintf((char *)wire+head,sizeof(wire)-(size_t)head,"%zx;test=value\r\n",n);
    else head+=snprintf((char *)wire+head,sizeof(wire)-(size_t)head,"Content-Length: %zu\r\n\r\n",n);
    memcpy(wire+head,encoded,n);size_t bytes=(size_t)head+n;
    if(chunked){const char *tail="\r\n0\r\nX-Trailer: end\r\n\r\n";memcpy(wire+bytes,tail,strlen(tail));bytes+=strlen(tail);}
    for(size_t split=1;split<bytes;split++){
        struct ciuki_http p;struct received r={0};ciuki_http_init(&p,false,callback,&r);
        for(size_t i=0;i<bytes;i+=split){size_t take=bytes-i<split?bytes-i:split;assert(ciuki_http_feed(&p,wire+i,take));}
        assert(p.done && r.bytes==strlen(body) && !memcmp(r.data,body,r.bytes));ciuki_http_destroy(&p);
    }
    /* Wrong checksum and truncated end-of-stream must not become FINISHED. */
    for(unsigned bad=0;bad<2;bad++){
        struct ciuki_http p;struct received r={0};ciuki_http_init(&p,false,callback,&r);
        if(!bad)wire[head+n-1]^=1;
        bool ok=ciuki_http_feed(&p,wire,bytes-bad);if(ok)ok=ciuki_http_eof(&p);
        assert(!ok && p.error);ciuki_http_destroy(&p);
        if(!bad)wire[head+n-1]^=1;
    }
}
static void expansion(bool excessive)
{
    unsigned char encoded[12000],block[1024];memset(block,'x',sizeof(block));
    z_stream z={0};assert(deflateInit2(&z,9,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
    z.next_out=encoded;z.avail_out=sizeof(encoded);
    unsigned blocks=excessive?8193:128;
    for(unsigned i=0;i<blocks;i++){z.next_in=block;z.avail_in=sizeof(block);assert(deflate(&z,Z_NO_FLUSH)==Z_OK && !z.avail_in);}
    assert(deflate(&z,Z_FINISH)==Z_STREAM_END);size_t n=z.total_out;deflateEnd(&z);
    struct ciuki_http p;struct received r={0};ciuki_http_init(&p,false,callback,&r);
    char header[128];int len=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %zu\r\n\r\n",n);
    assert(ciuki_http_feed(&p,header,(size_t)len));
    bool ok=ciuki_http_feed(&p,encoded,n);assert(r.bytes<=CIUKI_HTTP_SLICE);
    unsigned turns=1;
    while(ok && ciuki_http_pending(&p)){
        size_t before=r.bytes;ok=ciuki_http_drain(&p);assert(r.bytes-before<=CIUKI_HTTP_SLICE);assert(++turns<600);
    }
    if(excessive)assert(!ok && p.error && r.bytes<=CIUKI_HTTP_BODY);
    else assert(ok && p.done && r.bytes==128*1024 && turns>=8);
    ciuki_http_destroy(&p);
}
int main(void)
{
    example("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nCiuki","Ciuki",true);
    example("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;x=1\r\nCi\r\n3\r\nuki\r\n0\r\nX-Test: end\r\n\r\n","Ciuki",true);
    example("HTTP/1.0 200 OK\r\n\r\nCiuki","Ciuki",false);
    example("HTTP/1.1 200 OK\r\nConnection: upgrade, close\r\n\r\nCiuki","Ciuki",false);
    example("HTTP/1.1 204 No Content\r\n\r\n","",true);
    const char *bad[]={"HTTP/1.1 200 OK\n\n","HTTP/1.1 200 OK\r\n Content-Length: 1\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 8388609\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nCiuk",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffffffffffff\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\n\r\nx",
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxx"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)invalid(bad[i]);
    compressed(1,false);compressed(2,false);compressed(1,true);
    expansion(false);expansion(true);
    struct ciuki_http p;struct received r={.cancel=true};ciuki_http_init(&p,false,callback,&r);
    assert(!ciuki_http_feed(&p,"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nCiuki",43));
    assert(p.cancelled && r.headers==1 && !r.bytes);ciuki_http_destroy(&p);
    char *longline=malloc(9000);assert(longline);memset(longline,'a',8999);longline[8999]=0;invalid(longline);free(longline);
    char many[4096]="HTTP/1.1 200 OK\r\n";for(unsigned i=0;i<129;i++)strcat(many,"X: y\r\n");strcat(many,"\r\n");invalid(many);
    assert(ciuki_http_field("If-None-Match: abc",strlen("If-None-Match: abc"),true));
    assert(!ciuki_http_field("Host: bad",9,true));assert(!ciuki_http_field("X: a\r\nb",7,true));
    puts("PASS HTTP fragmented framing/chunks/trailers/lengths/gzip/deflate/checksums/limits/cancellation");
    return 0;
}
