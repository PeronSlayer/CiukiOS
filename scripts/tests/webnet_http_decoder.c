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
int intr(int n,struct regs *r) { (void)n;(void)r; return -1; }
int dos_open(const char *p,int m) { (void)p;(void)m; return -1; }
int dos_read(int h,void *b,int n) { (void)h;(void)b;(void)n; return -1; }
int dos_close(int h) { (void)h; return 0; }
u16 app_seg(void) { return 0; }

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
    char target[128], oversized[1200]; u8 output[512];
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
    wire_body_bytes=WEBNET_MAX_BODY_BYTES-1;
    html_byte('x');
    CHECK(state==S_HTTP && wire_body_bytes==WEBNET_MAX_BODY_BYTES,"body cap permits exactly 1 MiB");
    html_byte('y');
    CHECK(state==S_FAIL && strstr(error_text,"1 MiB download limit")!=0,"byte beyond body cap fails clearly");

    decoder_init(output,sizeof output);
    n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n",
               (unsigned long)(WEBNET_MAX_BODY_BYTES+1));
    feed((const u8*)header,n,7);
    CHECK(state==S_FAIL && strstr(error_text,"1 MiB download limit")!=0,"oversized Content-Length fails before body receipt");

    fetch_started_tick=65530;
    CHECK(!transfer_deadline_expired((u16)(fetch_started_tick+WEBNET_TIMEOUT_TICKS-1)),"60-second timeout remains clear before deadline across tick wrap");
    CHECK(transfer_deadline_expired((u16)(fetch_started_tick+WEBNET_TIMEOUT_TICKS)),"60-second timeout expires at deadline across tick wrap");
}
static void test_redirect_dot_segments(void)
{
    CHECK(webnet_redirect_target_supported("http://host/a/b"),"ordinary absolute HTTP path is accepted");
    CHECK(!webnet_redirect_target_supported("http://host/a/../b"),"parent dot-segment redirect is rejected");
    CHECK(!webnet_redirect_target_supported("http://host/./b?q=/safe/segment"),"literal dot path is rejected before query");
    CHECK(webnet_redirect_target_supported("http://host/a/b?q=/../safe"),"slashes and dot text in query are not treated as path segments");
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
    test_redirect_dot_segments();
    printf("WebNet HTTP decoder: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
