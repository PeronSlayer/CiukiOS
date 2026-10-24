/* SPDX-License-Identifier: MIT
 * G27B: generation-checked G29 format service; no write before explicit YES. */
#include "terminal.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
int term_format_command(struct terminal *t,int n,char **a)
{
    if (n<2) {
        term_write(t,"FORMAT drive: [/Q] [/FS:FAT12|FAT16|FAT32] [/V:label] YES\nOmit /Q for a full surface check. ALL data will be erased.\n"); return 0;
    }
    if (strlen(a[1])!=2 || a[1][1]!=':' || !isalpha((unsigned char)a[1][0])) return -EINVAL;
    unsigned drive=(unsigned)(toupper((unsigned char)a[1][0])-'A');
    unsigned flags=0,family=0; const char *label=NULL; int confirmed=0;
    for (int i=2;i<n;i++) {
        if (!strcasecmp(a[i],"/Q")) flags=CIUKI_FORMAT_QUICK;
        else if (!strncasecmp(a[i],"/FS:FAT",7)) {
            char *end; unsigned long value=strtoul(a[i]+7,&end,10);
            if (!a[i][7] || *end || (value!=12 && value!=16 && value!=32)) return -EINVAL;
            family=(unsigned)value;
        } else if (!strncasecmp(a[i],"/V:",3)) label=a[i]+3;
        else if (!strcmp(a[i],"YES")) confirmed=1;
        else return -EINVAL;
    }
    if (drive>=26 || drive==2 || (label && strlen(label)>11)) return -EINVAL;
    struct ciuki_media_format f={.version=CIUKI_MEDIA_VERSION,.size=sizeof(f),.drive=drive};
    int e=t->host.format ? t->host.format(t->host.ctx,0,&f) : -ENOSYS;
    if (e) return e;
    if (f.version!=CIUKI_MEDIA_VERSION || f.size!=sizeof(f) || f.drive!=drive || !f.sectors) return -EIO;
    if (family) f.filesystem=family;
    unsigned bit=f.filesystem==12 ? CIUKI_FORMAT_FAT12 : f.filesystem==16 ? CIUKI_FORMAT_FAT16 : f.filesystem==32 ? CIUKI_FORMAT_FAT32 : 0;
    if (!bit || !(f.supported_filesystems & bit)) return -EINVAL;
    term_printf(t,"Drive %c:  %llu bytes  FAT%u\nWARNING: Formatting erases ALL data on this drive.\n",'A'+drive,(unsigned long long)f.sectors*512,f.filesystem);
    if (!confirmed || t->capture) { term_write(t,"No changes written. Repeat at the prompt with YES to confirm.\n"); return -ECANCELED; }
    if (t->interrupted || t->quit) return -ECANCELED;
    f.flags=flags ? flags : drive<2 ? CIUKI_FORMAT_LOW_LEVEL : 0;
    if (label) memcpy(f.label,label,strlen(label)+1);
    e=t->host.format(t->host.ctx,1,&f);
    if (!e) term_printf(t,"Format complete. %llu bytes, %u bytes per allocation unit. Label: %.11s\n",(unsigned long long)f.sectors*512,f.cluster_bytes,f.label);
    return e;
}
