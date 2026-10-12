/* SPDX-License-Identifier: GPL-2.0-only
 * Run the production ELF admission function on files emitted by TinyCC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/process.h>
struct process *proc_current(void) { return NULL; }
void proc_launch_image_read(uint32_t n) { (void)n; }
static int take(void *ctx,uint32_t off,void *out,uint32_t n)
{ memcpy(out,(unsigned char *)ctx+off,n); return 0; }
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    FILE *f=fopen(argv[1],"rb"); if (!f) return 2;
    fseek(f,0,SEEK_END); long bytes=ftell(f); rewind(f);
    if (bytes<52 || bytes>CIUKI_ELF_BYTES_MAX) return 2;
    unsigned char *b=malloc((size_t)bytes); if (!b) return 2;
    if (fread(b,1,(size_t)bytes,f)!=(size_t)bytes) return 2;
    fclose(f);
    struct ciuki_file file={.cookie=b,.bytes=(uint32_t)bytes,.read=take}; struct elf_image image;
    int r=elf_validate(&file,&image);
    if (r || image.count!=3) { fprintf(stderr,"ELF admission: error %d, segments %u\n",r,image.count); free(b); return 1; }
    free(b); puts("TinyCC ELF: production loader admission PASS"); return 0;
}
