/* User-session startup: private USER state, no raw authoritative hive I/O.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "registry.h"
#if defined(__CIUKIOS__) && !defined(__linux__)
#include "../../sdk/libreg/transport.h"
#include "../../sdk/private/registry_protocol.h"
#include <ciuki/security.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static uint32_t user_get32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void user_put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static struct reg_remote compat_link={.fd=-1};
static uint32_t compat_rpc(const void *in,size_t n,void *out,size_t cap,size_t *used)
{
    if(reg_remote_win32(&compat_link,in,n,out,cap,used))return errno==EACCES?5:1062;return 0;
}
/* The failing step of a session start, for the serial log beside LOGON's
 * "session start failed" record. */
static int start_fail(const char *step,int error)
{
    fprintf(stderr,"[win32] registry session-start step=%s error=%d\n",step,error);
    return 0;
}
int wr_user_start(struct wr_server *s,struct wr_io *io,char directory[160],uint64_t now)
{
    struct ciuki_credential c;if(ciuki_security_self(&c)||!c.uid||!c.session_id)return start_fail("credential",errno);
    struct reg_remote link={.fd=-1,.service=2};uint8_t request[312]={0},reply[272];size_t n=0;
    user_put32(request,1);user_put32(request+36,c.uid);
    /* PROFILE accepts expected generation 0 for this pure read. */
    int e=reg_registry_rpc(&link,8,request,sizeof(request),reply,sizeof(reply),&n);reg_remote_close(&link);
    if(e)return start_fail("profile-rpc",errno);
    if(n!=272||user_get32(reply)!=1||user_get32(reply+4)!=272||user_get32(reply+8)!=c.uid||user_get32(reply+12)||
       !memchr(reply+32,0,64))return start_fail("profile-reply",(int)n);
    const char *root=(const char *)reply+32;
    if(strncmp(root,"/Windows/Profiles/",18)||strstr(root,"/../")||strstr(root,"/./"))return start_fail("profile-root",0);
    int written=snprintf(directory,160,"%s/Settings/Win32",root);if(written<0||written>=160)return 0;
    if(mkdir(directory,0700)&&errno!=EEXIST)return start_fail("settings-mkdir",errno);wr_posix_io(io,directory);
    memset(s,0,sizeof(*s));s->io=*io;s->now=now;s->session_only=1;s->subject.uid=c.uid;s->subject.session=c.session_id;
    s->subject.group_count=c.group_count;memcpy(s->subject.groups,c.groups,sizeof(c.groups));
    memcpy(s->user_path,root,strlen(root)+1);s->compat_rpc=compat_rpc;
    for(unsigned h=0;h<4;h++)s->roots[h]=wr_add_key(s,NULL,h==0?"HKLM":h==1?"HKU":h==2?"HKCC":"HKDD",h);
    if(!s->roots[0]||!s->roots[1]||!s->roots[2]||!s->roots[3]||!wr_directory_init(s)){wr_destroy(s);return start_fail("directory-init",errno);}
    return 1;
}
#else
int wr_user_start(struct wr_server *s,struct wr_io *io,char directory[160],uint64_t now)
{(void)s;(void)io;(void)directory;(void)now;return 0;}
#endif
