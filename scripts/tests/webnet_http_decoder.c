/* Host regression harness for the production WebNet HTTP decoder.
 * Build only: cc -std=c99 -Wall -Wextra -I scripts/tests \
 *   scripts/tests/webnet_http_decoder.c -o /tmp/webnet-http-decoder
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBNET_HTTP_HOST_TEST
#include "../../src/apps/webnet.c"

struct host webnet_test_host;
static int test_work_submit_result, test_work_poll_result, test_intr_calls, test_work_close_calls, test_work_debug_calls;
static u16 test_work_operation, test_work_bytes;
static u8 test_work_input[1024];
static struct cww_header test_work_reply;

int str_len(const char *s) { return (int)strlen(s); }
void str_copy(char *d,const char *s) { strcpy(d,s); }
void str_ncopy(char *d,const char *s,int n)
{
    int i=0;
    if(n<=0) return;
    while(i<n-1 && s[i]) { d[i]=s[i]; ++i; }
    d[i]=0;
}
void str_cat(char *d,const char *s) { strcat(d,s); }
int str_cmp(const char *a,const char *b) { return strcmp(a,b); }
void mem_copy(void *d,const void *s,int n) { memcpy(d,s,(size_t)n); }
void mem_move(void *d,const void *s,int n) { memmove(d,s,(size_t)n); }
void mem_set(void *d,int c,int n) { memset(d,c,(size_t)n); }
int mem_cmp(const void *a,const void *b,int n) { return memcmp(a,b,(size_t)n); }
void fmt_u32(char *d,u32 v) { sprintf(d,"%lu",(unsigned long)v); }
int intr(int n,struct regs *r) { (void)n;(void)r;++test_intr_calls; return -1; }
int dos_open(const char *p,int m) { (void)p;(void)m; return -1; }
int dos_read(int h,void *b,int n) { (void)h;(void)b;(void)n; return -1; }
int dos_close(int h) { (void)h; return 0; }
u16 app_seg(void) { return 0; }
int webwork_submit(u16 op,const void *data,u16 bytes,u32 aux)
{
    (void)aux;test_work_operation=op;test_work_bytes=bytes;
    if(bytes>sizeof test_work_input)return 0;
    if(bytes)memcpy(test_work_input,data,bytes);
    return test_work_submit_result;
}
int webwork_poll(struct cww_header *r,void *data,u16 cap)
{
    (void)data;(void)cap;
    if(test_work_poll_result==1&&r)memcpy(r,&test_work_reply,sizeof *r);
    return test_work_poll_result;
}
int webwork_result(u16 off,void *data,u16 bytes) {(void)off;(void)data;(void)bytes;return 0;}
int webwork_close(void) {++test_work_close_calls;return 1;}
void webwork_debug(void) {++test_work_debug_calls;}

static int checks, failures;
#define CHECK(c,what) do { ++checks; if(!(c)) { ++failures; fprintf(stderr,"FAIL: %s\n",what); } } while(0)

static void decoder_init(u8 *buffer,int capacity)
{
    state=S_HTTP; token=0; body_len=0; response=buffer; response_capacity=(u16)capacity;
    http_header_len=html_tag_len=trailer_line_len=trailer_bytes=chunk_line_bytes=0;
    http_status=0; redirect_target[0]=0;
    wire_body_bytes=declared_body_bytes=chunk_value=chunk_remaining=0;
    request_sent=0; response_truncated=headers_done=chunked_body=has_content_length=0;
    filter_html=1; html_state=HTML_TEXT; html_quote=comment_tail=skip_match=html_tag_overflow=0;
    chunk_state=CH_SIZE; chunk_digits=chunk_extension=trailer_cr=0;
    sink_mode=0; body_sink=0; sink_chunk_len=0; max_body_bytes=WEBNET_BUFFER_MAX_BYTES;
    content_type[0]=0;
    error_text[0]=0;
}
static void feed(const u8 *data,int length,int fragment)
{
    int pos=0;
    while(pos<length && state==S_HTTP) {
        int n=length-pos;
        if(n>fragment) n=fragment;
        http_consume(data+pos,(u16)n);
        pos+=n;
    }
}
static int append_chunked(char *out,int capacity,const char *body,int length,int chunk_bytes)
{
    int pos=0,n=0;
    n+=snprintf(out+n,(size_t)(capacity-n),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n");
    while(pos<length && n<capacity) {
        int amount=length-pos;
        if(amount>chunk_bytes) amount=chunk_bytes;
        n+=snprintf(out+n,(size_t)(capacity-n),"%X\r\n",amount);
        if(n+amount+2>=capacity) return -1;
        memcpy(out+n,body+pos,(size_t)amount); n+=amount;
        out[n++]='\r'; out[n++]='\n'; pos+=amount;
    }
    if(n+5>=capacity) return -1;
    memcpy(out+n,"0\r\n\r\n",5); n+=5;
    return n;
}
static void test_split_chunked_html(void)
{
    static const char body[]="<html><head><style>STYLE_SECRET</style>"
        "<script type=\"text/javascript\">SCRIPT_SECRET</script>"
        "<!--COMMENT_SECRET--></head><body><h1>VISIBLE_TEXT</h1></body></html>";
    char raw[1024]; u8 output[512]; int length,n;
    length=append_chunked(raw,sizeof raw,body,(int)strlen(body),13);
    CHECK(length>0,"build byte-split chunked fixture");
    decoder_init(output,sizeof output); feed((const u8*)raw,length,1);
    CHECK(state==S_HTTP && chunk_state==CH_DONE,"chunk framing survives byte-at-a-time packet splits");
    CHECK(wire_body_bytes==strlen(body),"source body count excludes chunk framing");
    state=S_DONE; n=webnet_read();
    CHECK(n>0 && strstr((char*)output,"VISIBLE_TEXT")!=0,"visible HTML survives filtering");
    CHECK(strstr((char*)output,"SCRIPT_SECRET")==0,"script contents are filtered");
    CHECK(strstr((char*)output,"STYLE_SECRET")==0,"style contents are filtered");
    CHECK(strstr((char*)output,"COMMENT_SECRET")==0,"comment contents are filtered");
    CHECK(!webnet_was_truncated(),"small response is not marked partial");
}
static void test_large_script_before_body(void)
{
    const int script_bytes=65536;
    int body_capacity=script_bytes+256, wire_capacity=body_capacity+2048;
    char *body=(char*)malloc((size_t)body_capacity), *wire=(char*)malloc((size_t)wire_capacity);
    u8 *output=(u8*)malloc(16384); int n,body_len;
    CHECK(body && wire && output,"allocate bounded large-page fixture");
    if(!body || !wire || !output) goto done;
    strcpy(body,"<html><head><script>");
    body_len=(int)strlen(body); memset(body+body_len,'x',(size_t)script_bytes); body_len+=script_bytes;
    strcpy(body+body_len,"</script></head><body><h1>VISIBLE_AFTER_62K</h1></body></html>");
    body_len+=(int)strlen(body+body_len);
    n=append_chunked(wire,wire_capacity,body,body_len,4093);
    CHECK(n>0,"build >62 KiB script response");
    if(n>0) {
        decoder_init(output,16384); feed((const u8*)wire,n,37);
        CHECK(state==S_HTTP && chunk_state==CH_DONE,"large chunked response completes in fragments");
        CHECK(wire_body_bytes==(u32)body_len,"large decoded body count is exact");
        state=S_DONE;
        CHECK(webnet_read()>0,"large response yields renderable content");
        CHECK(strstr((char*)output,"VISIBLE_AFTER_62K")!=0,"visible body after large script is retained");
        CHECK(strstr((char*)output,"xxxxxxxx") == 0,"large inline script is discarded");
        CHECK(!webnet_was_truncated(),"discarded script bytes do not cause partial-page status");
    }
done:
    free(output); free(wire); free(body);
}
static void test_visible_content_truncation(void)
{
    char raw[1100]; u8 output[256]; int header,n;
    header=snprintf(raw,sizeof raw,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 900\r\n\r\n");
    memset(raw+header,'v',900); decoder_init(output,sizeof output);
    feed((const u8*)raw,header+900,29); state=S_DONE;
    n=webnet_read();
    CHECK(n==255 && output[255]==0,"visible body is clipped to response capacity");
    CHECK(webnet_was_truncated(),"visible overflow is reported as partial");
    CHECK(wire_body_bytes==900 && declared_body_bytes==900,"source and retained byte counters stay separate");
    CHECK(http_body_complete(),"declared response length is complete");
}
static void test_redirect_and_bad_framing(void)
{
    static const char redirect[]="HTTP/1.1 301 Moved Permanently\r\nLocation: /next?q=/inside\r\nContent-Length: 0\r\n\r\n";
    static const char bad_chunk[]="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n";
    char target[128], oversized[8300]; u8 output[512];
    decoder_init(output,sizeof output); feed((const u8*)redirect,(int)strlen(redirect),2);
    CHECK(headers_done && http_body_complete() && http_status==301,"redirect headers and empty body parse");
    state=S_DONE;
    CHECK(webnet_redirect(target,sizeof target) && !strcmp(target,"/next?q=/inside"),"bounded relative Location is retained exactly");
    decoder_init(output,sizeof output); feed((const u8*)bad_chunk,(int)strlen(bad_chunk),1);
    CHECK(state==S_FAIL && strstr(error_text,"chunk framing")!=0,"malformed chunk size is rejected");
    decoder_init(output,sizeof output); memset(oversized,'A',sizeof oversized);
    feed((const u8*)oversized,sizeof oversized,83);
    CHECK(state==S_FAIL && strstr(error_text,"headers exceed")!=0,"oversized response headers are bounded and rejected");
}
static void test_declared_length_mismatch(void)
{
    static const char header[]="HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nshort";
    u8 output[512];
    decoder_init(output,sizeof output); feed((const u8*)header,(int)strlen(header),5);
    CHECK(!http_body_complete(),"close before Content-Length is rejected");
    http_consume((const u8*)"data",4);
    CHECK(http_body_complete(),"body count reaches declared Content-Length");
}
static void test_download_limit_and_deadline(void)
{
    u8 output[256]; char header[160]; int n;
    decoder_init(output,sizeof output);
    wire_body_bytes=WEBNET_BUFFER_MAX_BYTES-1;
    html_byte('x');
    CHECK(state==S_HTTP && wire_body_bytes==WEBNET_BUFFER_MAX_BYTES,"body cap permits exactly 1 MiB");
    html_byte('y');
    CHECK(state==S_FAIL && strstr(error_text,"configured download limit")!=0,"byte beyond body cap fails clearly");

    decoder_init(output,sizeof output);
    n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n",
               (unsigned long)(WEBNET_BUFFER_MAX_BYTES+1));
    feed((const u8*)header,n,7);
    CHECK(state==S_FAIL && strstr(error_text,"configured download limit")!=0,"oversized Content-Length fails before body receipt");

    fetch_started_tick=65530;
    CHECK(!transfer_deadline_expired((u16)(fetch_started_tick+WEBNET_TIMEOUT_TICKS-1)),"60-second timeout remains clear before deadline across tick wrap");
    CHECK(transfer_deadline_expired((u16)(fetch_started_tick+WEBNET_TIMEOUT_TICKS)),"60-second timeout expires at deadline across tick wrap");
}
static void test_tls_worker_time_is_not_network_retry_time(void)
{
    int i,result;
    state=S_HTTP;secure=1;tls_busy=1;tls_started=1;tls_job=CWW_TLS_OPEN;
    tries=5;fetch_started_tick=100;last_tick=400;HOST.ticks=400;token=0;
    test_work_poll_result=0;test_work_close_calls=test_work_debug_calls=0;
    for(i=0;i<6;++i){
        HOST.ticks=(u16)(HOST.ticks+18);
        result=webnet_poll();
        CHECK(result==WEBNET_PENDING&&state==S_HTTP&&tls_busy&&tries==5,
              "pending local TLS worker does not consume network retry attempts");
        CHECK(last_tick==HOST.ticks,
              "pending TLS worker refreshes the network retry window");
    }
    fetch_started_tick=(u16)(HOST.ticks-WEBNET_TIMEOUT_TICKS);
    result=webnet_poll();
    CHECK(result==WEBNET_FAILED&&state==S_FAIL&&
          strstr(error_text,"60-second time limit")!=0,
          "global 60-second transfer deadline still applies while TLS RPC is pending");
    CHECK(test_work_debug_calls==1&&test_work_close_calls==1,
          "secure timeout records worker RPC diagnostics before closing it");
    secure=0;tls_busy=tls_started=0;state=S_IDLE;token=0;
    test_work_poll_result=1;
}
static void test_redirect_dot_segments(void)
{
    CHECK(webnet_redirect_target_supported("http://host/a/b"),"ordinary absolute HTTP path is accepted");
    CHECK(webnet_redirect_target_supported("https://host/a/b"),"absolute HTTPS redirect target is accepted");
    CHECK(!webnet_redirect_target_supported("ftp://host/a/b"),"non-HTTP redirect schemes are rejected");
    CHECK(webnet_redirect_target_supported("http://host/a/../b"),"parent dot-segment redirect is accepted for normalization");
    CHECK(webnet_redirect_target_supported("http://host/./b?q=/safe/segment"),"literal dot path is accepted for normalization");
    CHECK(webnet_redirect_target_supported("http://host/a/b?q=/../safe"),"slashes and dot text in query are not treated as path segments");
}
static void check_resolve(const char *base,const char *ref,const char *expected,const char *name)
{
    char out[128];int ok=webnet_resolve_url(base,ref,out,sizeof out);
    CHECK(ok && !strcmp(out,expected),name);
    if(!ok||strcmp(out,expected))fprintf(stderr,"  ref=[%s] got=[%s] expected=[%s]\n",ref,out,expected);
}
static void test_rfc3986_url_resolution(void)
{
    static const char base[]="http://a/b/c/d;p?q";
    static const char secure_base[]="https://a/b/c/d;p?q";
    check_resolve(base,"g","http://a/b/c/g","simple relative path resolves against base directory");
    check_resolve(base,"../g","http://a/b/g","parent segment is removed");
    check_resolve(base,"../../g","http://a/g","multiple parent segments are removed");
    check_resolve(base,"/g/./h/../i","http://a/g/i","absolute reference path dot segments are normalized");
    check_resolve(base,"?y","http://a/b/c/d;p?y","query-only reference replaces query");
    check_resolve(base,"g?y#frag","http://a/b/c/g?y#frag","relative path preserves query and fragment");
    check_resolve(base,"#new","http://a/b/c/d;p?q#new","fragment-only reference preserves base query");
    check_resolve(base,"//other/x/../y","http://other/y","network-path reference inherits HTTP and normalizes path");
    check_resolve(base,"//other/path#frag","http://other/path#frag","network-path reference retains its fragment");
    check_resolve(base,"https://secure.example/path","https://secure.example/path","absolute HTTPS link is preserved for the UI");
    check_resolve(secure_base,"g","https://a/b/c/g","relative path inherits HTTPS scheme");
    check_resolve(secure_base,"../g","https://a/b/g","parent path resolves under HTTPS base");
    check_resolve(secure_base,"?y","https://a/b/c/d;p?y","query-only reference preserves HTTPS scheme");
    check_resolve(secure_base,"#new","https://a/b/c/d;p?q#new","fragment-only reference preserves HTTPS query");
    check_resolve(secure_base,"//other/x/../y","https://other/y","network-path reference inherits HTTPS scheme and normalizes path");
    check_resolve(secure_base,"https://secure.example/a/../path","https://secure.example/path","absolute HTTPS path dot segments are normalized");
    check_resolve(base,"sub/a:b","http://a/b/c/sub/a:b","colon after a path slash is not mistaken for a URI scheme");
    {
        char out[128];
        CHECK(!webnet_resolve_url(base,"C:\\AUTOEXEC.BAT",out,sizeof out),"DOS drive path is rejected as an HTML URL reference");
    }
    {
        char long_ref[128], out[32]; memset(long_ref,'x',sizeof long_ref); long_ref[127]=0;
        CHECK(!webnet_resolve_url(base,long_ref,out,sizeof out),"result that exceeds caller's bounded URL capacity is rejected");
    }
}
static void test_tls_partial_input_and_ack_retry(void)
{
    test_work_submit_result=1;test_work_poll_result=1;
    memset(&test_work_reply,0,sizeof test_work_reply);
    test_work_reply.consumed=2;
    secure=1;state=S_HTTP;tls_busy=1;tls_job=CWW_TLS_STEP;
    tls_rx_bytes=tls_rx_sent=4;memcpy(tls_rx,"ABCD",4);
    tls_out_bytes=tls_out_at=0;tls_ready=tls_drain=tls_closed=0;
    test_intr_calls=0;
    tls_poll();
    CHECK(tls_rx_bytes==2 && tls_rx[0]=='C' && tls_rx[1]=='D',"TLS step retains unconsumed ciphertext bytes");
    CHECK(test_work_operation==CWW_TLS_STEP && test_work_bytes==2 &&
          !memcmp(test_work_input,"CD",2),"remaining ciphertext is resubmitted exactly after partial worker consumption");

    tls_tx_bytes=3;tls_tx_seq=0xFFFFFFFEUL;memcpy(tls_tx,"xyz",3);
    tls_ack(0xFFFFFFFDUL);
    CHECK(tls_tx_bytes==3 && tls_tx_seq==0xFFFFFFFEUL,"old ACK before wrapped TLS queue does not alter it");
    tls_ack(0xFFFFFFFFUL);
    CHECK(tls_tx_bytes==2 && tls_tx_seq==0xFFFFFFFFUL && !memcmp(tls_tx,"yz",2),"partial TCP ACK trims TLS ciphertext queue across sequence wrap");
    tls_ack(0xFFFFFFFFUL);
    CHECK(tls_tx_bytes==2 && !memcmp(tls_tx,"yz",2),"duplicate TCP ACK does not discard pending TLS bytes");
    tls_ack(2);
    CHECK(tls_tx_bytes==2 && tls_tx_seq==0xFFFFFFFFUL,"ACK beyond queued TLS range is ignored");
    tls_ack(1);
    CHECK(tls_tx_bytes==0 && tls_tx_seq==1,"cumulative TCP ACK drains remaining wrapped TLS bytes");

    tls_tx_bytes=3;tls_tx_seq=90;memcpy(tls_tx,"abc",3);
    tls_tx_tick=0;HOST.ticks=9;test_intr_calls=0;tls_busy=0;
    tls_poll();
    CHECK(tls_tx_bytes==3 && !memcmp(tls_tx,"abc",3),"failed frame enqueue keeps TLS retransmission bytes available");
    CHECK(test_intr_calls==1 && tls_tx_tick==HOST.ticks,"expired TLS ACK timer attempts one bounded retransmission");
    secure=1;state=S_HTTP;tls_busy=0;tls_started=0;tls_rx_bytes=0;tls_tx_bytes=0;
    tls_out_bytes=tls_out_at=0;tls_ready=1;request_sent=1;tls_fin=1;tls_closed=0;
    headers_done=1;chunked_body=0;has_content_length=1;
    declared_body_bytes=wire_body_bytes=0;
    tls_poll();
    CHECK(state==S_HTTP && tls_busy && test_work_operation==CWW_TLS_CLOSE,
          "complete framed response submits client close_notify before TCP FIN");
    memset(&test_work_reply,0,sizeof test_work_reply);
    tls_poll();
    CHECK(state==S_DONE && tls_close_complete,
          "TCP FIN follows completion of the client close_notify operation");

    state=S_HTTP;tls_fin=1;tls_closed=0;tls_ready=1;tls_started=0;
    tls_close_requested=tls_close_complete=0;tls_busy=0;
    has_content_length=1;declared_body_bytes=1;wire_body_bytes=0;
    tls_poll();
    CHECK(state==S_FAIL && strstr(error_text,"close_notify")!=0,
          "incomplete Content-Length response rejects abrupt TLS TCP FIN");

    state=S_HTTP;tls_fin=1;tls_closed=0;tls_ready=1;tls_started=0;
    tls_close_requested=tls_close_complete=0;tls_busy=0;
    has_content_length=chunked_body=0;declared_body_bytes=wire_body_bytes=0;
    tls_poll();
    CHECK(state==S_FAIL && strstr(error_text,"close_notify")!=0,
          "EOF-delimited TLS body requires authenticated close_notify");

    test_work_close_calls=0;state=S_DONE;tls_started=1;tls_busy=0;
    webnet_cancel();
    CHECK(test_work_close_calls==0,"cancel after completed TLS resource preserves the shared worker process");
    state=S_HTTP;tls_started=1;tls_busy=1;webnet_cancel();
    CHECK(test_work_close_calls==1,"cancel with a pending TLS worker operation stops DMA-owning worker");
    test_work_poll_result=0;test_work_submit_result=0;tls_tx_bytes=0;tls_rx_bytes=0;
    secure=0;state=S_IDLE;tls_fin=tls_closed=0;
}
static void test_custom_http_ports(void)
{
    char url[100];
    CHECK(parse_url("http://127.0.0.1:8080/index.html") &&
          !strcmp(host,"127.0.0.1") && server_port==8080,"localhost HTTP URL accepts a custom port");
    request_len=0;request_sent=0;client_port=50000;str_copy(path,"/index.html");
    (void)request_send();
    CHECK(strstr(http_request,"Host: 127.0.0.1:8080\r\n")!=0,"non-default port is included in the HTTP Host authority");
    CHECK(be16(frame+14+20+2)==8080,"TCP segment targets the parsed custom server port");
    CHECK(parse_url("http://example.test/") && server_port==80,"HTTP URL defaults to port 80");
    request_len=0;request_sent=0;str_copy(path,"/");(void)request_send();
    CHECK(strstr(http_request,"Host: example.test\r\n")!=0,"default port is omitted from the Host authority");
    CHECK(parse_url("http://example.test:65535/") && server_port==65535,"largest valid TCP port is accepted");
    CHECK(!parse_url("http://example.test:0/") && !parse_url("http://example.test:65536/") &&
          !parse_url("http://example.test:80junk/"),"zero, overflowing, and malformed ports are rejected");
    str_copy(url,"http://");mem_set(url+7,'a',64);str_copy(url+71,"/page");
    CHECK(!parse_url(url),"DNS hostname exceeding the 63-byte buffer is rejected");
}
static u8 sink_bytes[4096];
static u32 sink_total;
static int sink_calls,sink_bad;
static int test_sink(const u8 *bytes,u16 length)
{
    u16 i;
    if(length>WEBNET_SINK_CHUNK || sink_total+length>sizeof sink_bytes) { sink_bad=1; return 0; }
    for(i=0;i<length;++i) sink_bytes[sink_total+i]=bytes[i];
    sink_total+=length; ++sink_calls; return 1;
}
static void test_streaming_binary_chunked_body(void)
{
    u8 body[2600]; char *wire=(char*)malloc(3400); int i,n;
    CHECK(wire!=0,"allocate bounded binary streaming fixture"); if(!wire) return;
    for(i=0;i<(int)sizeof body;++i) body[i]=(u8)(i*37);
    body[0]=0; body[1]=0xFF; body[2]=0;
    n=snprintf(wire,3400,"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
    i=0;
    while(i<(int)sizeof body) {
        int amount=(int)sizeof body-i; if(amount>701) amount=701;
        n+=snprintf(wire+n,(size_t)(3400-n),"%X\r\n",amount);
        memcpy(wire+n,body+i,(size_t)amount); n+=amount;
        wire[n++]='\r'; wire[n++]='\n'; i+=amount;
    }
    memcpy(wire+n,"0\r\n\r\n",5); n+=5;
    decoder_init(0,0); sink_mode=1; body_sink=test_sink; max_body_bytes=WEBNET_STREAM_MAX_BYTES;
    sink_total=0; sink_calls=0; sink_bad=0;
    feed((const u8*)wire,n,997);
    CHECK(state==S_HTTP && chunk_state==CH_DONE,"chunked binary body completes in streaming mode");
    CHECK(sink_total==sizeof body && !sink_bad && sink_calls>=3,"stream callback receives bounded body chunks");
    CHECK(!memcmp(sink_bytes,body,sizeof body),"stream preserves zero and high-bit binary bytes exactly");
    CHECK(!strcmp(webnet_content_type(),"application/octet-stream"),"Content-Type MIME is available to the sink client");
    free(wire);
}
static void test_stream_size_limit(void)
{
    static const char header[]="HTTP/1.1 200 OK\r\nContent-Length: 17\r\n\r\n";
    char out[128]; u8 body[32];
    decoder_init(body,sizeof body); sink_mode=1; body_sink=test_sink; max_body_bytes=16;
    feed((const u8*)header,(int)strlen(header),64);
    CHECK(state==S_FAIL && strstr(error_text,"configured download limit")!=0,"oversized stream Content-Length is rejected before body delivery");
    CHECK(webnet_resolve_url("http://host/","x",out,sizeof out),"normal URL resolves with caller-sized output");
    (void)webnet_start_sink("http://host/",test_sink,WEBNET_STREAM_MAX_BYTES+1);
    CHECK(state==S_FAIL,"stream request larger than 4 MiB cap is rejected");
}
int main(void)
{
    webnet_test_host.ticks=1;
    test_split_chunked_html();
    test_large_script_before_body();
    test_visible_content_truncation();
    test_redirect_and_bad_framing();
    test_declared_length_mismatch();
    test_download_limit_and_deadline();
    test_tls_worker_time_is_not_network_retry_time();
    test_redirect_dot_segments();
    test_rfc3986_url_resolution();
    test_tls_partial_input_and_ack_retry();
    test_custom_http_ports();
    test_streaming_binary_chunked_body();
    test_stream_size_limit();
    printf("WebNet HTTP decoder: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
