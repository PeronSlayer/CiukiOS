/* SPDX-License-Identifier: MIT
 * RFC 1952 wrapper around the repository's existing stb DEFLATE compressor.
 * All stb symbols are local; notices: third_party/stb/README.md and LICENSE. */
#include "report.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../third_party/stb/stb_image_write.h"
static void put32(unsigned char *p,uint32_t v)
{ for (unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i)); }
int hr_gzip(const char *text,size_t size,unsigned char **out,size_t *length)
{
    if (!text || !out || !length || size>HR_FULL_LOG_MAX) return -EINVAL;
    *out=NULL; *length=0;
    int bytes=0;
    unsigned char *z=stbi_zlib_compress((unsigned char *)(uintptr_t)text,(int)size,&bytes,5);
    if (!z) return -ENOMEM;
    if (bytes<6) { free(z); return -EIO; }
    size_t total=(size_t)bytes+12; /* replace 2-byte zlib header/4-byte Adler with 10+8 */
    unsigned char *gz=malloc(total);
    if (!gz) { free(z); return -ENOMEM; }
    const unsigned char header[10]={0x1f,0x8b,8,0,0,0,0,0,0,255}; /* no timestamp, name or host metadata */
    memcpy(gz,header,10); memcpy(gz+10,z+2,(size_t)bytes-6); free(z);
    uint32_t crc=0xffffffffu;
    for (size_t i=0;i<size;i++) {
        crc^=(unsigned char)text[i];
        for (unsigned j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    put32(gz+total-8,~crc); put32(gz+total-4,(uint32_t)size);
    *out=gz; *length=total; return 0;
}
