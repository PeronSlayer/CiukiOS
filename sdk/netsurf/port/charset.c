/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <parserutils/charset/codec.h>
#include "charset.h"

static const char *encoding(const char *name)
{
    if (!name) return NULL;
    if (!strcasecmp(name,"utf-8") || !strcasecmp(name,"utf8")) return "UTF-8";
    if (!strcasecmp(name,"iso-8859-1") || !strcasecmp(name,"latin1")) return "ISO-8859-1";
    if (!strcasecmp(name,"iso-8859-15") || !strcasecmp(name,"latin9")) return "ISO-8859-15";
    if (!strcasecmp(name,"windows-1252") || !strcasecmp(name,"cp1252")) return "Windows-1252";
    return NULL;
}

nserror ciuki_charset(const char *input, size_t length, const char *from, const char *to,
                     char **output, size_t *outlength)
{
    parserutils_charset_codec *decoder=NULL, *encoder=NULL;
    const uint8_t *in;
    uint8_t *ucs=NULL, *result=NULL, *pos;
    size_t left, capacity, used;
    nserror status=NSERROR_BAD_ENCODING;
    if (!output || !input) return NSERROR_BAD_PARAMETER;
    *output=NULL;
    if (outlength) *outlength=0;
    from=encoding(from); to=encoding(to);
    if (!from || !to) return NSERROR_BAD_ENCODING;
    if (length>8u*1024u*1024u || length>(SIZE_MAX-16)/4) return NSERROR_BAD_SIZE;
    capacity=length*4+16;
    ucs=malloc(capacity); result=malloc(capacity);
    if (!ucs || !result) { status=NSERROR_NOMEM; goto done; }
    if (parserutils_charset_codec_create(from,&decoder)!=PARSERUTILS_OK ||
        parserutils_charset_codec_create(to,&encoder)!=PARSERUTILS_OK) { status=NSERROR_NOMEM; goto done; }
    parserutils_charset_codec_optparams strict={.error_mode={PARSERUTILS_CHARSET_CODEC_ERROR_STRICT}};
    parserutils_charset_codec_setopt(decoder,PARSERUTILS_CHARSET_CODEC_ERROR_MODE,&strict);
    parserutils_charset_codec_setopt(encoder,PARSERUTILS_CHARSET_CODEC_ERROR_MODE,&strict);
    in=(const uint8_t *)input; left=capacity; pos=ucs;
    size_t remaining=length;
    if (parserutils_charset_codec_decode(decoder,&in,&remaining,&pos,&left)!=PARSERUTILS_OK || remaining)
        goto done;
    used=(size_t)(pos-ucs);
    /* Strict UTF-8 validation also rejects a truncated trailing sequence:
     * parserutils keeps that in its streaming decoder until the next chunk. */
    if (!strcmp(from,"UTF-8")) {
        size_t i=0;
        while (i<length) {
            unsigned c=(unsigned char)input[i++], count;
            uint32_t value, min;
            if (c<128) continue;
            if (c>=0xc2 && c<=0xdf) { count=1; value=c&31; min=128; }
            else if (c>=0xe0 && c<=0xef) { count=2; value=c&15; min=2048; }
            else if (c>=0xf0 && c<=0xf4) { count=3; value=c&7; min=65536; }
            else goto done;
            if (count>length-i) goto done;
            while(count--) { c=(unsigned char)input[i++]; if ((c&0xc0)!=0x80) goto done; value=value*64+(c&63); }
            if (value<min || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) goto done;
        }
    }
    in=ucs; remaining=used; left=capacity-4; pos=result;
    if (parserutils_charset_codec_encode(encoder,&in,&remaining,&pos,&left)!=PARSERUTILS_OK || remaining)
        goto done;
    used=(size_t)(pos-result); memset(pos,0,4);
    *output=(char *)result; result=NULL;
    if (outlength) *outlength=used;
    status=NSERROR_OK;
done:
    if (decoder) parserutils_charset_codec_destroy(decoder);
    if (encoder) parserutils_charset_codec_destroy(encoder);
    free(ucs); free(result);
    return status;
}
