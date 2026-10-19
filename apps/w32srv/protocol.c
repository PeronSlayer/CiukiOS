/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "registry.h"
#include <stdlib.h>
#include <string.h>
void wr_session_close(struct wr_session *s)
{
    free(s->input);free(s->output);memset(s,0,sizeof(*s));
}
int wr_packet(struct wr_server *server,struct wr_session *s,const struct w32r_packet *in,struct w32r_packet *out)
{
    memset(out,0,sizeof(*out));out->version=W32R_VERSION;out->id=in->id;out->kind=in->kind;
    if(in->version!=W32R_VERSION||in->error||!in->id){out->error=87;return 0;}
    if(in->kind==W32R_BEGIN){
        uint32_t pid=s->pid;wr_session_close(s);s->pid=pid;
        if(in->offset||in->total<sizeof(struct w32r_request)+2||in->total>W32R_MAX){out->error=87;return 0;}
        s->input=malloc(in->total);if(!s->input){out->error=8;return 0;}s->total=in->total;s->id=in->id;
    }
    if(in->kind==W32R_WRITE||in->kind==W32R_BEGIN){
        if(!s->input||in->id!=s->id||in->total!=s->total||in->offset!=s->received||s->received>=s->total){out->error=87;return 0;}
        uint32_t n=s->total-s->received;if(n>sizeof(in->bytes))n=sizeof(in->bytes);
        memcpy(s->input+s->received,in->bytes,n);s->received+=n;out->offset=s->received;
        if(s->received==s->total){
            s->output=malloc(W32R_MAX);if(!s->output){out->error=8;return 0;}
            uint32_t e=wr_dispatch(server,s->pid,s->input,s->total,s->output,W32R_MAX,&s->output_bytes);
            free(s->input);s->input=NULL;if(e){out->error=e;free(s->output);s->output=NULL;return 0;}
            out->total=(uint32_t)s->output_bytes;
        }
    } else if(in->kind==W32R_READ){
        if(in->id!=s->id||!s->output||in->total!=s->output_bytes||in->offset>=s->output_bytes){out->error=87;return 0;}
        size_t n=s->output_bytes-in->offset;if(n>sizeof(out->bytes))n=sizeof(out->bytes);
        memcpy(out->bytes,s->output+in->offset,n);out->total=(uint32_t)s->output_bytes;out->offset=in->offset+(uint32_t)n;
        if(out->offset==out->total){free(s->output);s->output=NULL;s->output_bytes=0;}
    } else out->error=87;
    return 0;
}
