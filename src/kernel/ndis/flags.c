/* Intel SDM Vol. 2 PUSHF/POPF/IRET/INT3; PE base relocations.
 * See docs/design/ndis-user-host.md. SPDX-License-Identifier: GPL-2.0-only */
#include "flags.h"
#include "scan.h"
#include "userhost.h"
#include <ciuki/kernel.h>
static int operands(struct pe_image *p,struct ndis_flag_workspace *w)
{
    w->count=0;
    struct pe_directory d=p->dirs[5];
    if(d.rva>p->size || d.size>p->size-d.rva) return 0;
    for(uint32_t off=0;off<d.size;) {
        if(d.size-off<8) return 0;
        const uint8_t *b=p->image+d.rva+off;
        uint32_t page=pe_u32(b),n=pe_u32(b+4);
        if(n<8 || (n&1) || n>d.size-off) return 0;
        for(uint32_t i=8;i<n;i+=2) {
            unsigned e=pe_u16(b+i),kind=e>>12,bytes=kind==3?4:2;
            if(!kind) continue;
            uint32_t rva=page+(e&4095);
            if(kind>4 || page>rva || rva>p->size || bytes>p->size-rva ||
               w->count==NDIS_RELOC_OPERANDS) return 0;
            if(kind==4) { if(n-i<4) return 0;i+=2; }
            unsigned j=w->count++;
            while(j && w->operand[j-1].rva>rva) { w->operand[j]=w->operand[j-1];j--; }
            w->operand[j].rva=rva;w->operand[j].bytes=bytes;
        }
        off+=n;
    }
    for(unsigned i=1;i<w->count;i++) if(w->operand[i].rva<
        w->operand[i-1].rva+w->operand[i-1].bytes) return 0;
    return 1;
}
int ndis_flags_patch(struct pe_image *p,struct ndis_flag_table *t,
                     struct ndis_flag_workspace *w,const uint8_t hash[32])
{
    if(!p || !p->image || !t || !w || !hash || t->sealed || !operands(p,w)) return 0;
    t->count=0;t->base=p->base;t->size=p->size;memcpy(t->image_hash,hash,32);
    for(unsigned i=0;i<p->section_count;i++) {
        const struct pe_section *s=&p->sections[i];
        if(!(s->flags&0x20000000)) continue;
        if(s->rva>p->size || s->span>p->size-s->rva || (s->flags&0x80000000)) return 0;
        unsigned operand=0;
        for(uint32_t at=s->rva;at<s->rva+s->span;) {
            while(operand<w->count && w->operand[operand].rva+w->operand[operand].bytes<=at) operand++;
            if(operand<w->count && w->operand[operand].rva<=at) {
                if(w->operand[operand].rva!=at || w->operand[operand].bytes>s->rva+s->span-at) return 0;
                at+=w->operand[operand].bytes;continue;
            }
            uint32_t length=0;const char *why;
            if(!ndis_instruction_length(p->image+at,s->rva+s->span-at,&length,&why)) return 0;
            unsigned prefix=p->image[at]==0x66?1:0;
            uint8_t op=p->image[at+prefix];
            if(op==0x9c || op==0x9d || op==0xcf) {
                if(length!=prefix+1 || t->count==NDIS_FLAG_SITES ||
                   (operand<w->count && w->operand[operand].rva<at+length)) return 0;
                t->site[t->count++]=(struct ndis_flag_site){at+prefix,op,prefix?2:4,prefix,0};
            } else {
                /* Other prefix combinations around a flag instruction refuse:
                 * accepting them would leave a transparent IF hole. */
                unsigned k=0;
                while(k<length && (p->image[at+k]==0x66 || p->image[at+k]==0x67 ||
                      p->image[at+k]==0xf0 || p->image[at+k]==0xf2 || p->image[at+k]==0xf3 ||
                      p->image[at+k]==0x26 || p->image[at+k]==0x2e || p->image[at+k]==0x36 ||
                      p->image[at+k]==0x3e || p->image[at+k]==0x64 || p->image[at+k]==0x65)) k++;
                if(k<length && (p->image[at+k]==0x9c || p->image[at+k]==0x9d || p->image[at+k]==0xcf)) return 0;
            }
            at+=length;
        }
    }
    /* Validate the entire sweep before mutating any code byte. */
    for(unsigned i=0;i<t->count;i++) p->image[t->site[i].rva]=0xcc;
    t->sealed=true;return 1;
}
int ndis_flags_breakpoint(struct ndis_user_host *h,struct trap_frame *tf,const struct ndis_flag_access *a)
{
    if(!ndis_host_live(h) || !tf || tf->vector!=3 || (tf->cs&3)!=3 ||
       !a || !a->read || !a->write || !tf->eip) return 0;
    const struct ndis_flag_table *t=h->image->platform.flag_table;
    if(!t || !t->sealed || t->base!=h->image->pe.base || t->size!=h->image->pe.size ||
       memcmp(t->image_hash,h->image->hash,32) || tf->eip-1<t->base) return 0;
    uint32_t rva=tf->eip-1-t->base;
    const struct ndis_flag_site *s=0;
    for(unsigned i=0;i<t->count;i++) if(t->site[i].rva==rva) { s=&t->site[i];break; }
    if(!s || rva>=t->size || h->image->pe.image[rva]!=0xcc ||
       (s->prefix && (!rva || h->image->pe.image[rva-1]!=0x66))) return 0;
    uint8_t code[2];
    if(!a->read(a->context,tf->eip-1-s->prefix,code,1+s->prefix) ||
       code[s->prefix]!=0xcc || (s->prefix && code[0]!=0x66)) return 0;
    unsigned width=s->width;
    if(width!=2 && width!=4) return 0;
    uint8_t frame[12]={0};uint32_t flags=ndis_host_flags(h,tf->eflags);
    if(s->opcode==0x9c) {
        if(tf->user_esp<0x10000u+width) return 0;
        pe_put32(frame,flags&~0x30000u); /* PUSHFD clears RF and VM in saved copy */
        if(!a->write(a->context,tf->user_esp-width,frame,width)) return 0;
        tf->user_esp-=width;ndis_host_retired(h,tf);return 1;
    }
    unsigned bytes=s->opcode==0xcf?width*3:width;
    if(tf->user_esp<0x10000 || tf->user_esp>=0xc0000000u || bytes>0xc0000000u-tf->user_esp ||
       !a->read(a->context,tf->user_esp,frame,bytes)) return 0;
    uint32_t saved=width==2?pe_u16(frame+bytes-width):pe_u32(frame+bytes-width);
    if(saved&(0x3000u|0x4000u|0x20000u)) return 0; /* no IOPL, NT, VM */
    uint32_t next=tf->eip;
    if(s->opcode==0xcf) {
        next=width==2?pe_u16(frame):pe_u32(frame);
        uint32_t cs=width==2?pe_u16(frame+width):pe_u32(frame+width);
        if(cs!=tf->cs || !ntimage_executable(h->image,next)) return 0;
    } else if(s->opcode!=0x9d) return 0;
    if(!ndis_host_restore_flags(h,saved)) return 0;
    uint32_t mask=width==2?0x0dd5u:0x240dd5u;
    if(s->opcode==0xcf && width==4) mask|=0x10000u; /* IRETD restores RF */
    tf->eflags=((tf->eflags&~mask)|(saved&mask))&~(0x3000u|0x4000u|0x20000u);
    if(h->sti_shadow) h->shadow_tf=!!(saved&0x100u);
    tf->eflags|=0x202u;tf->eip=next;tf->user_esp+=bytes;ndis_host_retired(h,tf);return 1;
}
