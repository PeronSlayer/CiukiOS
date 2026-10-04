/* Small cooperative IPv4/DNS/TCP/HTTP client for CiukWeb. It owns the private
 * INT 61 receive claim only during one fetch and never waits inside a call. */
#include "webnet.h"

#define FRAME_MAX 1600
#define TCP_PORT  49152
#define DNS_PORT  53000
#define DNS_ID    0xC17A

enum { S_IDLE, S_ARP_DNS, S_DNS, S_ARP_HOST, S_SYN, S_HTTP, S_DONE, S_FAIL };
static u8 frame[FRAME_MAX], *response;
static u8 status_buf[18];
static char host[64], path[128], error_text[80];
static u8 local_ip[4], mask_ip[4], gateway_ip[4], dns_ip[4], arp_ip[4];
static u8 local_mac[6], peer_mac[6], peer_ip[4];
static char http_request[320];
static u16 state, token, body_len, response_capacity, last_tick, tries, client_port;
static u16 request_len, http_status;
static u32 isn, snd_nxt, rcv_nxt;
static u32 request_seq;
static int request_sent;

static u16 be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 be32(const u8 *p)
{ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v)
{ p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }
static int same4(const u8 *a, const u8 *b)
{ return !mem_cmp(a, b, 4); }
static int starts(const char *a,const char *b)
{ while(*b) if(*a++!=*b++) return 0; return 1; }
static int lower(int c) { return c>='A'&&c<='Z'?c+32:c; }
static u16 checksum(const u8 *p,u16 n)
{
    u32 s=0;
    while(n>1) { s+=be16(p); p+=2; n-=2; }
    if(n) s+=(u16)(p[0]<<8);
    while(s>>16) s=(s&0xFFFFUL)+(s>>16);
    return (u16)~s;
}
static u16 transport_checksum(const u8 *src,const u8 *dst,u8 proto,const u8 *p,u16 n)
{
    u8 pseudo[12]; u32 s=0;
    mem_copy(pseudo,src,4); mem_copy(pseudo+4,dst,4);
    pseudo[8]=0; pseudo[9]=proto; put16(pseudo+10,n);
    s+=(u32)be16(pseudo); s+=(u32)be16(pseudo+2); s+=(u32)be16(pseudo+4);
    s+=(u32)be16(pseudo+6); s+=(u32)be16(pseudo+8); s+=(u32)be16(pseudo+10);
    while(n>1) { s+=be16(p); p+=2; n-=2; }
    if(n) s+=(u16)(p[0]<<8);
    while(s>>16) s=(s&0xFFFFUL)+(s>>16);
    return (u16)~s;
}
static int call_private(u8 op, struct regs *r)
{
    r->ax = (u16)(0xFE00 | op);
    return intr(0x61, r);
}
static int send_frame(u16 n)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0xFE06; r.bx = token; r.cx = n;
    r.ds = app_seg(); r.si = (u16)frame;
    return call_private(6, &r);
}
static int arp_send(const u8 *target)
{
    u8 *p = frame;
    mem_set(p, 0xFF, 6); mem_copy(p + 6, local_mac, 6);
    put16(p + 12, 0x0806); put16(p + 14, 1); put16(p + 16, 0x0800);
    p[18]=6; p[19]=4; put16(p + 20, 1);
    mem_copy(p + 22, local_mac, 6); mem_copy(p + 28, local_ip, 4);
    mem_set(p + 32, 0, 6); mem_copy(p + 38, target, 4);
    return send_frame(42);
}
static u16 ip_header(u8 *p, u8 protocol, u16 payload)
{
    u16 total = (u16)(20 + payload);
    p[0]=0x45; p[1]=0; put16(p+2,total); put16(p+4,(u16)(HOST.ticks ^ 0xC1));
    put16(p+6,0x4000); p[8]=64; p[9]=protocol; put16(p+10,0);
    mem_copy(p+12,local_ip,4); mem_copy(p+16,peer_ip,4);
    put16(p+10,checksum(p,20));
    return total;
}
static int ethernet_ip(u8 protocol, u16 payload)
{
    mem_copy(frame,peer_mac,6); mem_copy(frame+6,local_mac,6); put16(frame+12,0x0800);
    return send_frame((u16)(14 + ip_header(frame+14,protocol,payload)));
}
static int dns_send(void)
{
    u8 *ip=frame+14, *udp=ip+20, *d=udp+8;
    int i=0, n=0, label=0;
    mem_set(d,0,512); put16(d,DNS_ID); put16(d+2,0x0100); put16(d+4,1);
    while (host[i]) {
        int start=i;
        while (host[i] && host[i]!='.') ++i;
        if (i==start || i-start>63 || n+i-start+2>=450) return 0;
        d[12+n++]=(u8)(i-start);
        while (start<i) d[12+n++]=(u8)host[start++];
        if (host[i]=='.') ++i;
        ++label;
    }
    if (!label) return 0;
    d[12+n++]=0; put16(d+12+n,1); put16(d+14+n,1); n+=4;
    put16(udp,DNS_PORT); put16(udp+2,53); put16(udp+4,(u16)(8+12+n)); put16(udp+6,0);
    mem_copy(peer_ip,dns_ip,4);
    /* DNS is sent directly on the LAN or via the configured gateway. */
    return ethernet_ip(17,(u16)(8+12+n));
}
static int tcp_send(u8 flags, const u8 *data, u16 bytes, u32 seq, u32 ack)
{
    u8 *ip=frame+14, *t=ip+20;
    u16 n=(u16)(20+bytes), sum;
    mem_set(t,0,n); put16(t,client_port); put16(t+2,80); put32(t+4,seq);
    put32(t+8,ack); t[12]=0x50; t[13]=flags; put16(t+14,4096);
    if (bytes) mem_copy(t+20,data,bytes);
    put16(t+16,0);
    sum=transport_checksum(local_ip,peer_ip,6,t,(u16)(20+bytes));
    put16(ip+20+16,sum);
    return ethernet_ip(6,(u16)(20+bytes));
}
static int request_send(void)
{
    int n=0, i;
    const char *a="GET ", *b=" HTTP/1.0\r\nHost: ", *c="\r\nConnection: close\r\n\r\n";
    if (!request_len) {
        static const char hex[]="0123456789ABCDEF";
        while(a[n]) { if(n>=315) return 0; http_request[n]=a[n]; ++n; }
        for(i=0;path[i];++i) {
            int ch=(u8)path[i],safe=((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||
                (ch>='0'&&ch<='9')||ch=='-'||ch=='.'||ch=='_'||ch=='~'||
                ch=='!'||ch=='$'||ch=='&'||ch=='\''||ch=='('||ch==')'||
                ch=='*'||ch=='+'||ch==','||ch==';'||ch=='='||ch==':'||
                ch=='@'||ch=='/'||ch=='?'||ch=='%');
            if(safe) { if(n>=315)return 0; http_request[n++]=(char)ch; }
            else { if(n>=312)return 0; http_request[n++]='%'; http_request[n++]=hex[(ch>>4)&15]; http_request[n++]=hex[ch&15]; }
        }
        for(i=0;b[i];++i) { if(n>=315)return 0; http_request[n++]=b[i]; }
        for(i=0;host[i];++i) { if(n>=315)return 0; http_request[n++]=host[i]; }
        for(i=0;c[i];++i) { if(n>=319)return 0; http_request[n++]=c[i]; }
        if(n>=320) return 0;
        http_request[n]=0; request_len=(u16)n;
    }
    if(!request_sent) request_seq=snd_nxt;
    if (tcp_send(0x18,(const u8*)http_request,request_len,request_seq,rcv_nxt)) return 0;
    if(!request_sent) { snd_nxt += request_len; request_sent=1; }
    return 1;
}
static int parse_url(const char *url)
{
    int i=0,n=0;
    if (!starts(url,"http://")) return 0;
    i=7;
    while(url[i] && url[i]!='/' && url[i]!=':' && n<63) {
        int ch=(u8)url[i];
        if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='-')) return 0;
        host[n++]=url[i++];
    }
    host[n]=0;
    if (!n || url[i]==':' || host[0]=='.' || host[n-1]=='.') return 0;
    if (!url[i]) str_copy(path,"/");
    else {
        int j=0;
        while(url[i] && url[i]!='#') {
            if((u8)url[i]<32 || j>=126) return 0;
            path[j++]=url[i++];
        }
        path[j]=0;
    }
    return 1;
}
static int start_arp(const u8 *target, u16 next)
{
    mem_copy(arp_ip,target,4); state=next; tries=0; last_tick=HOST.ticks;
    return arp_send(arp_ip);
}
static int local_subnet(const u8 *ip)
{
    int i;
    for(i=0;i<4;++i) if((ip[i]&mask_ip[i])!=(local_ip[i]&mask_ip[i])) return 0;
    return 1;
}
static int config_ip(const u8 *text,int bytes,const char *key,u8 out[4])
{
    int pos=0,klen=str_len(key);
    while(pos<bytes) {
        int start=pos,j,oct;
        while(pos<bytes && text[pos]!='\n' && text[pos]!='\r') ++pos;
        if(pos-start>=klen && !mem_cmp(text+start,key,klen)) {
            j=start+klen;
            if(j<pos && text[j]!=' ' && text[j]!='\t') { ++pos; continue; }
            while(j<pos && (text[j]==' '||text[j]=='\t')) ++j;
            for(oct=0;oct<4;++oct) {
                int value=0,digits=0;
                while(j<pos && text[j]>='0' && text[j]<='9') {
                    value=value*10+(text[j++]-'0');
                    if(value>255) return 0;
                    ++digits;
                }
                if(!digits) return 0;
                out[oct]=(u8)value;
                if(oct<3) { if(j>=pos || text[j++]!='.') return 0; }
            }
            return 1;
        }
        while(pos<bytes && (text[pos]=='\n'||text[pos]=='\r')) ++pos;
    }
    return 0;
}
static void fail(const char *why)
{
    str_copy(error_text,why); state=S_FAIL;
    if (token) { struct regs r; mem_set(&r,0,sizeof r); r.bx=token; call_private(4,&r); token=0; }
}

int webnet_start(const char *url, void *response_buffer, int capacity)
{
    struct regs r; u8 *info; int f,n;
    if (state!=S_IDLE && state!=S_DONE && state!=S_FAIL) webnet_cancel();
    state=S_IDLE; body_len=0; tries=0; response=0; response_capacity=0; http_status=0; str_copy(error_text,"");
    if(!response_buffer || capacity<256) {
        str_copy(error_text,"Browser response buffer is unavailable."); state=S_FAIL; return state;
    }
    if (!parse_url(url)) { str_copy(error_text,"Use an HTTP URL (HTTPS is not available)."); state=S_FAIL; return state; }
    mem_set(&r,0,sizeof r);
    if (call_private(3,&r) || r.bx!=0xC161) { str_copy(error_text,"Native network service is unavailable or busy."); state=S_FAIL; return state; }
    token=r.bx;
    response=(u8*)response_buffer; response_capacity=(u16)capacity;
    mem_set(&r,0,sizeof r); r.bx=token; r.es=app_seg(); r.di=(u16)status_buf;
    if (call_private(5,&r) || r.es!=app_seg()) { fail("Network status query failed."); return state; }
    info=status_buf;
    if (mem_cmp(info,"CVNT",4) || info[4]!=1) { fail("Incompatible native network service."); return state; }
    mem_copy(local_ip,info+6,4); mem_copy(local_mac,info+10,6);
    f=dos_open("C:\\NET\\MTCP.CFG",0);
    if(f<0) { fail("Missing C:\\NET\\MTCP.CFG."); return state; }
    n=dos_read(f,frame,FRAME_MAX); dos_close(f);
    { static const u8 default_mask[4]={255,255,255,0};
      static const u8 default_gateway[4]={10,0,2,2};
      static const u8 default_dns[4]={10,0,2,3};
      mem_copy(mask_ip,default_mask,4); mem_copy(gateway_ip,default_gateway,4); mem_copy(dns_ip,default_dns,4); }
    /* Honor the active mTCP profile for routes and resolver without requiring
       a resident packet-driver environment variable in the CAPP process. */
    if(n>0 && n<FRAME_MAX) {
        config_ip(frame,n,"NETMASK",mask_ip);
        config_ip(frame,n,"GATEWAY",gateway_ip);
        config_ip(frame,n,"NAMESERVER",dns_ip);
    }
    client_port=(u16)(49152+(HOST.ticks&0x3FFF)); request_len=0; request_sent=0;
    isn=((u32)HOST.ticks<<16)|0xC1A0; snd_nxt=isn; rcv_nxt=0;
    last_tick=HOST.ticks;
    mem_copy(peer_ip,dns_ip,4);
    start_arp(local_subnet(dns_ip)?dns_ip:gateway_ip,S_ARP_DNS);
    return WEBNET_PENDING;
}

static void process_dns(const u8 *p, u16 n)
{
    u16 qd,an,pos=12; int i;
    if(n<12 || be16(p)!=DNS_ID || !(be16(p+2)&0x8000)) return;
    if(be16(p+2)&0x000F) { fail("DNS lookup failed."); return; }
    qd=be16(p+4); an=be16(p+6);
    for(i=0;i<qd && pos<n;++i) {
        while(pos<n && p[pos]) { if((p[pos]&0xC0)==0xC0) {pos+=2;break;} pos+=(u16)(p[pos]+1); }
        if(pos<n && p[pos]==0) ++pos;
        pos+=4;
    }
    for(i=0;i<an && pos+12<=n;++i) {
        u16 type,cls,rd;
        if((p[pos]&0xC0)==0xC0) pos+=2;
        else { while(pos<n && p[pos]) { pos+=(u16)(p[pos]+1); } ++pos; }
        if(pos+10>n) return;
        type=be16(p+pos); cls=be16(p+pos+2); rd=be16(p+pos+8); pos+=10;
        if(pos+rd>n) return;
        if(type==1 && cls==1 && rd==4) {
            mem_copy(peer_ip,p+pos,4);
            start_arp(local_subnet(peer_ip)?peer_ip:gateway_ip,S_ARP_HOST); return;
        }
        pos+=rd;
    }
    fail("DNS returned no IPv4 address.");
}

static void process_packet(const u8 *p, u16 n)
{
    u16 et, iplen, ihl, plen;
    if(n<14) return; et=be16(p+12);
    if(et==0x0806 && n>=42) {
        if(state==S_ARP_DNS || state==S_ARP_HOST) {
          if(be16(p+14)==1 && be16(p+16)==0x0800 && p[18]==6 && p[19]==4 &&
             be16(p+20)==2 && same4(p+28,arp_ip) && same4(p+38,local_ip) &&
             !mem_cmp(p+32,local_mac,6)) {
            mem_copy(peer_mac,p+22,6);
            if(state==S_ARP_DNS) { state=S_DNS; tries=0; last_tick=HOST.ticks; dns_send(); }
            else if(state==S_ARP_HOST) {
                state=S_SYN; tries=0; last_tick=HOST.ticks;
                if(!tcp_send(2,0,0,isn,0)) snd_nxt=isn+1;
            }
          }
        }
        return;
    }
    if(et!=0x0800 || n<34 || (p[14]>>4)!=4) return;
    ihl=(u16)((p[14]&15)*4); iplen=be16(p+16);
    if(ihl<20 || iplen<ihl || iplen>n-14 || (be16(p+20)&0xBFFF) ||
       checksum(p+14,ihl)!=0 || !same4(p+30,local_ip)) return;
    if(p[23]==17 && state==S_DNS && iplen>=ihl+8) {
        const u8 *u=p+14+ihl;
        u16 ul=be16(u+4), uc=be16(u+6);
        if(be16(u+2)==DNS_PORT && be16(u)==53 && same4(p+26,dns_ip) && ul>=8 && ul<=iplen-ihl &&
           (!uc || transport_checksum(p+26,p+30,17,u,ul)==0))
            process_dns(u+8,(u16)(ul-8));
    } else if(p[23]==6 && (state==S_SYN || state==S_HTTP) && iplen>=ihl+20) {
        const u8 *t=p+14+ihl; u16 th=(u16)((t[12]>>4)*4); u32 seq,ack; u8 flags;
        if(th<20 || ihl+th>iplen || be16(t+2)!=client_port || be16(t)!=80 || !same4(p+26,peer_ip) ||
           transport_checksum(p+26,p+30,6,t,(u16)(iplen-ihl))!=0) return;
        seq=be32(t+4); ack=be32(t+8); flags=t[13]; plen=(u16)(iplen-ihl-th);
        if(state==S_SYN && (flags&0x14)==0x14 && ack==snd_nxt) { fail("Web server refused the connection."); return; }
        if(state==S_HTTP && (flags&0x04)) {
            if(seq==rcv_nxt) fail("Web connection was reset by the server.");
            return;
        }
        if(!(flags&0x10) || (long)(ack-snd_nxt)>0 ||
           (state==S_HTTP && (long)(ack-(isn+1))<0)) return;
        if(state==S_SYN && (flags&0x12)==0x12 && ack==snd_nxt) {
            rcv_nxt=seq+1;
            if(!request_send()) { fail("Could not send HTTP request."); return; }
            state=S_HTTP; tries=0; last_tick=HOST.ticks; return;
        }
        if(state==S_HTTP) {
            int accepted_fin=0;
            if(plen && seq==rcv_nxt) {
                u16 room=(u16)(response_capacity-body_len), take;
                if(plen>room) { fail("Page is larger than CiukWeb's memory limit."); return; }
                take=plen<room?plen:room;
                if(take) { mem_copy(response+body_len,t+th,take); body_len+=take; rcv_nxt+=take; tries=0; last_tick=HOST.ticks; }
            }
            if((flags&1) && seq+(u32)plen==rcv_nxt) { ++rcv_nxt; accepted_fin=1; state=S_DONE; }
            if(accepted_fin) {
                /* Close our half immediately. We release without waiting for
                   the peer's final ACK so the browser remains cooperative. */
                if(!tcp_send(0x11,0,0,snd_nxt,rcv_nxt)) ++snd_nxt;
            } else (void)tcp_send(0x10,0,0,snd_nxt,rcv_nxt);
        }
    }
}

int webnet_poll(void)
{
    struct regs r; u16 now=HOST.ticks;
    if(state==S_DONE) { if(token) { mem_set(&r,0,sizeof r); r.bx=token; call_private(4,&r); token=0; } return WEBNET_COMPLETE; }
    if(state==S_FAIL || state==S_IDLE) return state==S_FAIL?WEBNET_FAILED:WEBNET_IDLE;
    { int drained;
      for(drained=0;drained<4;++drained) {
        mem_set(&r,0,sizeof r); r.bx=token; r.cx=FRAME_MAX; r.es=app_seg(); r.di=(u16)frame;
        if(call_private(7,&r) || !r.cx) break;
        if(r.cx>=14 && r.cx<=FRAME_MAX) process_packet(frame,r.cx);
        if(state==S_DONE || state==S_FAIL) break;
      }
    }
    if(state==S_DNS || state==S_SYN || state==S_HTTP || state==S_ARP_DNS || state==S_ARP_HOST) {
        if((u16)(now-last_tick)>=18) {
            last_tick=now;
            if(++tries>5) { fail("Network request timed out."); return WEBNET_FAILED; }
            if(state==S_ARP_DNS) arp_send(arp_ip);
            else if(state==S_DNS) dns_send();
            else if(state==S_ARP_HOST) arp_send(arp_ip);
            else if(state==S_SYN) { if(!tcp_send(2,0,0,isn,0)) snd_nxt=isn+1; }
            else if(state==S_HTTP) (void)request_send();
        }
    }
    return state==S_DONE?webnet_poll():state==S_FAIL?WEBNET_FAILED:WEBNET_PENDING;
}

int webnet_read(void)
{
    int i,head=-1,n=0,body_start,chunked=0;
    if(state!=S_DONE || !response || response_capacity==0) return -1;
    for(i=0;i+3<(int)body_len;++i) if(response[i]=='\r'&&response[i+1]=='\n'&&response[i+2]=='\r'&&response[i+3]=='\n') {head=i;break;}
    if(head<0) return -1;
    if(body_len>=12 && starts((const char*)response,"HTTP/")) {
        i=0; while(i<body_len && response[i]!=' ') ++i;
        if(i+3<body_len && response[i+1]>='0' && response[i+1]<='9' &&
           response[i+2]>='0' && response[i+2]<='9' &&
           response[i+3]>='0' && response[i+3]<='9')
            http_status=(u16)((response[i+1]-'0')*100+(response[i+2]-'0')*10+(response[i+3]-'0'));
    }
    body_start=head+4;
    for(i=0;i<head;++i) {
        static const char te[]="transfer-encoding:";
        int j=0;
        if(i!=0 && !(response[i-1]=='\n' && i>=2 && response[i-2]=='\r')) continue;
        if(i+(int)(sizeof te-1)>head) continue;
        while(te[j] && lower(response[i+j])==te[j]) ++j;
        if(!te[j]) {
            int k=i+j;
            while(k<head && (response[k]==' '||response[k]=='\t')) ++k;
            if(k+7<=head && lower(response[k])=='c' && lower(response[k+1])=='h' &&
               lower(response[k+2])=='u' && lower(response[k+3])=='n' &&
               lower(response[k+4])=='k' && lower(response[k+5])=='e' && lower(response[k+6])=='d')
                chunked=1;
            break;
        }
    }
    if(chunked) {
        int pos=body_start;
        for(;;) {
            u32 size=0; int digits=0;
            while(pos<(int)body_len && response[pos]!='\r' && response[pos]!=';' && digits<8) {
                int c=response[pos++],v;
                if(c>='0'&&c<='9') v=c-'0';
                else if(lower(c)>='a'&&lower(c)<='f') v=lower(c)-'a'+10;
                else return -1;
                size=(size<<4)|(u32)v; ++digits;
            }
            while(pos<(int)body_len && response[pos]!='\r') ++pos; /* bounded extensions */
            if(!digits || pos+1>=(int)body_len || response[pos+1]!='\n') return -1;
            pos+=2;
            if(!size) break;
            if(size>(u32)(body_len-pos) || pos+(int)size+1>=(int)body_len ||
               response[pos+(int)size]!='\r' || response[pos+(int)size+1]!='\n') return -1;
            while(size && n<response_capacity-1) { response[n++]=response[pos++]; --size; }
            if(size) return -1;
            pos+=2;
        }
        response[n]=0;
        return n;
    }
    n=(int)body_len-body_start;
    if(n>=response_capacity) n=response_capacity-1;
    mem_move(response,response+body_start,n);
    response[n]=0;
    return n;
}
void webnet_cancel(void)
{
    if(token) {
        struct regs r;
        if(state==S_SYN) (void)tcp_send(0x14,0,0,snd_nxt,0);
        else if(state==S_HTTP) (void)tcp_send(0x14,0,0,snd_nxt,rcv_nxt);
        mem_set(&r,0,sizeof r); r.bx=token; call_private(4,&r); token=0;
    }
    state=S_IDLE; body_len=0; response=0; response_capacity=0;
}
const char *webnet_error(void) { return error_text; }
int webnet_http_status(void) { return http_status; }
