/* SPDX-License-Identifier: MIT
 * Real pinned/patched data fetcher; isolate fetch callbacks and URL transport. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "content/fetchers/data.c"
struct nsurl { const char *text; };
const char *nsurl_access(const nsurl *url) { return url->text; }
size_t nsurl_length(const nsurl *url) { return strlen(url->text); }
nsurl *nsurl_ref(nsurl *url) { return url; }
void nsurl_unref(nsurl *url) { (void)url; }
void fetch_set_http_code(struct fetch *f,long code) { (void)f;(void)code; }
void fetch_remove_from_queues(struct fetch *f) { (void)f; }
void fetch_free(struct fetch *f) { (void)f; }
void fetch_send_callback(const fetch_msg *message,struct fetch *fetch)
{ (void)message;(void)fetch; }
/* No escaped bytes in this fixture; decoding itself remains upstream. */
nserror url_unescape(const char *s,size_t n,size_t *length,char **out)
{ if(!n)n=strlen(s);*out=strndup(s,n);*length=n;return *out?NSERROR_OK:NSERROR_NOMEM; }
void nslog_log(const char *file,const char *func,int line,const char *format,...)
{ (void)file;(void)func;(void)line;(void)format; }
int main(void)
{
    const char *urls[]={"data:x,hello","data:ab,hello","data:abcdef,hello",
        "data:;base6,hello","data:;base64,aGVsbG8=","data:x;base64,aGVsbG8=","data:,hello"};
    for(unsigned i=0;i<sizeof(urls)/sizeof(*urls);i++) {
        struct nsurl url={urls[i]};struct fetch_data_context ctx={.url=&url};
        assert(fetch_data_process(&ctx));
        assert(ctx.datalen==5 && !memcmp(ctx.data,"hello",5));
        assert(ctx.base64==(i==4 || i==5));
        free(ctx.mimetype);free(ctx.data);
    }
    puts("NetSurf data: short MIME lengths 1..6, omitted MIME and base64 passed");
    fflush(stdout);
}
