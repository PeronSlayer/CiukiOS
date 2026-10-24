/* Proxy authentication and strict CONNECT handshake. SPDX-License-Identifier: MIT */
#ifndef CIUKI_PROXY_H
#define CIUKI_PROXY_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static inline int ciuki_proxy_basic(char *out,size_t cap,const char *user,const char *password)
{
    static const char digits[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char plain[193];size_t u=strlen(user),p=strlen(password);
    if(u>95 || p>95 || u+p+1>=sizeof(plain))return -1;
    if(!u) { if(cap)out[0]=0;return cap?0:-1; }
    for(size_t i=0;i<u;i++)if((unsigned char)user[i]<32 || (unsigned char)user[i]>126 || user[i]==':')return -1;
    for(size_t i=0;i<p;i++)if((unsigned char)password[i]<32 || (unsigned char)password[i]>126)return -1;
    memcpy(plain,user,u);plain[u]=':';memcpy(plain+u+1,password,p);size_t n=u+1+p;
    if(6+4*((n+2)/3)+1>cap)return -1;
    memcpy(out,"Basic ",6);size_t at=6;
    for(size_t i=0;i<n;i+=3) {
        unsigned a=(unsigned char)plain[i],b=i+1<n?(unsigned char)plain[i+1]:0,c=i+2<n?(unsigned char)plain[i+2]:0;
        out[at++]=digits[a>>2];out[at++]=digits[((a&3)<<4)|(b>>4)];out[at++]=i+1<n?digits[((b&15)<<2)|(c>>6)]:'=';out[at++]=i+2<n?digits[c&63]:'=';
    }
    out[at]=0;memset(plain,0,sizeof(plain));return 0;
}
static inline int ciuki_proxy_auth_valid(const char *auth)
{
    if(!auth || !*auth)return 1;if(strncmp(auth,"Basic ",6))return 0;
    size_t n=strlen(auth);if(n>262 || n<10 || (n-6)%4)return 0;
    unsigned padding=0;
    for(size_t i=6;i<n;i++) {
        unsigned c=(unsigned char)auth[i];
        if(c=='=') { if(++padding>2 || i<n-2)return 0; }
        else if(padding || !((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='+' || c=='/'))return 0;
    }
    return 1;
}
static inline int ciuki_proxy_connect_request(char *out,size_t cap,const char *host,unsigned port,const char *auth)
{
    if(!host || !*host || strlen(host)>253 || !port || port>65535 || !ciuki_proxy_auth_valid(auth))return -1;
    for(const unsigned char *p=(const void *)host;*p;p++)if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || *p=='.' || *p=='-'))return -1;
    int n=snprintf(out,cap,"CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n%s%s%s\r\n",host,port,host,port,
        auth && *auth?"Proxy-Authorization: ":"",auth?auth:"",auth && *auth?"\r\n":"");
    return n<0 || (size_t)n>=cap?-1:n;
}
/* 0 incomplete; 1 tunnel established; -407 authentication; -1 malformed or
 * refused. TLS must not start before success; CONNECT headers never reach the
 * origin HTTP parser. No response body is permitted on a successful tunnel. */
static inline int ciuki_proxy_connect_response(const char *data,size_t bytes)
{
    if(bytes>4096)return -1;
    size_t end=0;for(size_t i=0;i+3<bytes;i++)if(!memcmp(data+i,"\r\n\r\n",4)) { end=i+4;break; }
    if(!end)return bytes==4096?-1:0;
    if(end!=bytes || bytes<16 || (memcmp(data,"HTTP/1.1 ",9) && memcmp(data,"HTTP/1.0 ",9)) ||
        data[9]<'1' || data[9]>'5' || data[10]<'0' || data[10]>'9' || data[11]<'0' || data[11]>'9' || data[12]!=' ')return -1;
    unsigned code=(unsigned)(data[9]-'0')*100+(unsigned)(data[10]-'0')*10+(unsigned)(data[11]-'0');
    for(size_t i=0;i<bytes;i++)if((unsigned char)data[i]<32 && data[i]!='\r' && data[i]!='\n' && data[i]!='\t')return -1;
    for(size_t i=0;i<bytes;i++)if((data[i]=='\n' && (!i || data[i-1]!='\r')) || (data[i]=='\r' && (i+1==bytes || data[i+1]!='\n')))return -1;
    return code>=200 && code<300?1:code==407?-407:-1;
}
#endif
