/* Execute the pinned MBR/SYS FAT16 loader and K2043's initial relocation.
 * Sparse synthetic sectors, BIOS reads only; no DOS/filesystem service model.
 * Stops at upstream's trace '3', before FreeDOSmain. This is not a VM boot.
 * SPDX-License-Identifier: MIT */
#include <x86emu.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SECTORS=262144, START=63, RESERVED=73, FATS=128, ROOT=32, SPC=8,
       ROOT_LBA=START+RESERVED+2*FATS, DATA_LBA=ROOT_LBA+ROOT };
static uint8_t mbr[512],vbr[512],*kernel;
static size_t kernel_size;
static bool loaded, relocated, bad;
static bool packed_reference;

static bool sector(uint32_t lba,uint8_t *out)
{
    memset(out,0,512);
    if(lba>=SECTORS) return false;
    if(lba==0) memcpy(out,mbr,512);
    else if(lba==START) memcpy(out,vbr,512);
    else if(lba==ROOT_LBA) {
        memcpy(out,"KERNEL  SYS",11);out[11]=0x20;out[26]=2;
        for(unsigned i=0;i<4;i++) out[28+i]=(uint8_t)(kernel_size>>(8*i));
    } else if(lba>=START+RESERVED && lba<ROOT_LBA) {
        unsigned first=((lba-START-RESERVED)%FATS)*256;
        unsigned clusters=(unsigned)((kernel_size+SPC*512-1)/(SPC*512));
        for(unsigned i=0;i<256;i++) {
            unsigned c=first+i,value=c==0 ? 0xfff8 : c==1 ? 0xffff :
                c<2+clusters ? (c==1+clusters ? 0xffff : c+1) : 0;
            out[2*i]=(uint8_t)value;out[2*i+1]=(uint8_t)(value>>8);
        }
    } else if(lba>=DATA_LBA) {
        size_t off=(lba-DATA_LBA)*512u;
        if(off<kernel_size) memcpy(out,kernel+off,kernel_size-off<512 ? kernel_size-off : 512);
    }
    return true;
}

static int interrupt(x86emu_t *e,u8 vector,unsigned type)
{
    (void)type;
    if(vector==0x10) {
        if(e->x86.R_AX==0x0e31) {
            loaded=true;
            for(size_t i=0;!packed_reference && i<kernel_size;i++)
                if(x86emu_read_byte(e,0x600+(unsigned)i)!=kernel[i]) { bad=true;break; }
        }
        if(e->x86.R_AX==0x0e33) {
            uint32_t pc=((uint32_t)e->x86.R_CS<<4)+e->x86.R_IP;
            uint32_t sp=((uint32_t)e->x86.R_SS<<4)+e->x86.R_SP;
            relocated=loaded && !bad && pc>=0x70000 && pc<0xa0000 && sp>=0x70000 && sp<0xa0000;
            x86emu_stop(e);
        }
        return 1;
    }
    if(vector==0x12) { e->x86.R_AX=640;return 1; }
    if(vector==0x13) {
        unsigned ah=e->x86.R_AH;
        e->x86.R_EFLG&=~1u;
        if(ah==0) return 1;
        if(ah==0x41) { e->x86.R_BX=0xaa55;e->x86.R_CX=1;e->x86.R_AH=0;return 1; }
        uint32_t lba,dest;unsigned count;
        if(ah==0x42) {
            unsigned p=((unsigned)e->x86.R_DS<<4)+e->x86.R_SI;
            if(x86emu_read_byte(e,p)!=16 || x86emu_read_dword(e,p+12)) goto fail;
            count=x86emu_read_word(e,p+2);lba=x86emu_read_dword(e,p+8);
            dest=((unsigned)x86emu_read_word(e,p+6)<<4)+x86emu_read_word(e,p+4);
        } else if(ah==2) {
            unsigned cyl=e->x86.R_CH+((e->x86.R_CL&0xc0u)<<2),s=e->x86.R_CL&63u;
            if(!s || e->x86.R_DH>=16) goto fail;
            count=e->x86.R_AL;lba=(cyl*16u+e->x86.R_DH)*63u+s-1;
            dest=((unsigned)e->x86.R_ES<<4)+e->x86.R_BX;
        } else goto fail;
        if(!count || count>127 || dest+count*512u>0xa0000 || lba>=SECTORS || count>SECTORS-lba) goto fail;
        for(unsigned n=0;n<count;n++) {
            uint8_t data[512];if(!sector(lba+n,data)) goto fail;
            for(unsigned i=0;i<512;i++) x86emu_write_byte(e,dest+n*512+i,data[i]);
        }
        e->x86.R_AH=0;return 1;
    }
fail:
    bad=true;x86emu_stop(e);return 1;
}

static size_t read_input(const char *path,void *data,size_t bound)
{
    FILE *f=fopen(path,"rb");if(!f) return 0;
    size_t n=fread(data,1,bound,f);int extra=fgetc(f),error=ferror(f);fclose(f);
    return extra==EOF && !error ? n : 0;
}
int main(int argc,char **argv)
{
    if(argc!=4 && (argc!=5 || strcmp(argv[4],"packed-reference"))) return 2;
    packed_reference=argc==5;
    kernel=malloc(134*1024);if(!kernel) return 3;
    kernel_size=read_input(argv[3],kernel,134*1024);
    if(read_input(argv[1],mbr,512)!=512 || read_input(argv[2],vbr,512)!=512 || kernel_size<1024 ||
       mbr[510]!=0x55 || mbr[511]!=0xaa || vbr[510]!=0x55 || vbr[511]!=0xaa) { free(kernel);return 4; }
    x86emu_t *e=x86emu_new(X86EMU_PERM_RWX,0);if(!e) { free(kernel);return 5; }
    for(unsigned i=0;i<512;i++) x86emu_write_byte(e,0x7c00+i,mbr[i]);
    x86emu_write_word(e,0x413,640);
    x86emu_set_seg_register(e,e->x86.seg+R_CS_INDEX,0);
    e->x86.R_IP=0x7c00;e->x86.R_SP=0x7c00;e->x86.R_DL=0x80;e->x86.R_EFLG=0x202;
    e->max_instr=2000000;x86emu_set_intr_handler(e,interrupt);x86emu_run(e,0);
    bool ok=loaded && relocated && !bad;
    printf("H55-03 CPU MBR/FAT16 load=%u raw_checked=%u relocation=%u bad=%u (stopped before FreeDOSmain)\n",loaded,!packed_reference,relocated,bad);
    fflush(stdout);
    x86emu_done(e);free(kernel);return ok ? 0 : 6;
}
