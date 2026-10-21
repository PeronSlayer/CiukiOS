/* Included by upstream utils/utf8.c in place of its iconv conversion section. */
#include "framebuffer/ciuki/charset.h"
nserror utf8_finalise(void) { return NSERROR_OK; }
nserror utf8_to_enc(const char *s, const char *encoding, size_t n, char **out)
{ return ciuki_charset(s,n?n:strlen(s),"UTF-8",encoding,out,NULL); }
nserror utf8_from_enc(const char *s, const char *encoding, size_t n, char **out, size_t *length)
{ return ciuki_charset(s,n?n:strlen(s),encoding,"UTF-8",out,length); }
nserror utf8_to_html(const char *s, const char *encoding, size_t n, char **out)
{
    char *escaped, *write;
    if (!n) n=strlen(s);
    if (n>(SIZE_MAX-1)/12) return NSERROR_BAD_SIZE;
    write=escaped=malloc(n*12+1);
    if (!escaped) return NSERROR_NOMEM;
    for (size_t at=0; at<n;) {
        uint32_t cp=utf8_to_ucs4(s+at,n-at);
        size_t next=utf8_next(s,n,at);
        char *one=NULL;
        nserror err=ciuki_charset(s+at,next-at,"UTF-8",encoding,&one,NULL);
        free(one);
        if (cp=='&' || cp=='<' || cp=='>' || err==NSERROR_BAD_ENCODING) {
            write+=sprintf(write,"&#x%06x;",(unsigned)cp);
        } else if (err!=NSERROR_OK) { free(escaped); return err; }
        else { memcpy(write,s+at,next-at); write+=next-at; }
        at=next;
    }
    *write=0;
    nserror err=ciuki_charset(escaped,(size_t)(write-escaped),"UTF-8",encoding,out,NULL);
    free(escaped); return err;
}
