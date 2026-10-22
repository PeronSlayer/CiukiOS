/* SPDX-License-Identifier: GPL-2.0-only
 * Intel SDM Vol. 2A ch. 2; see docs/design/ndis-containment.md.
 * Authored here, with no imported disassembler implementation. */
#include "scan.h"
static int no(const char **why, const char *reason) { *why=reason; return 0; }
int ndis_instruction(const uint8_t *p,uint32_t bytes,uint32_t *length,const char **why) {
    unsigned n=0,operand=4,address=4,immediate=0;
    bool modrm=false,group3=false;
    *length=0; *why=0;
    if(!p || !bytes) return no(why,"instruction-truncated");
    if(bytes>15) bytes=15;
    for(;;) {
        if(n==bytes) return no(why,"instruction-truncated");
        uint8_t b=p[n];
        if(b==0x66) operand=2;
        else if(b==0x67) address=2;
        else if(b!=0xf0 && b!=0xf2 && b!=0xf3 && b!=0x26 && b!=0x2e &&
                b!=0x36 && b!=0x3e && b!=0x64 && b!=0x65) break;
        n++;
    }
    unsigned op=p[n++], second=256;
    if(op==0xfa) return no(why,"instruction-cli");
    if(op==0xfb) return no(why,"instruction-sti");
    if(op==0xf4) return no(why,"instruction-hlt");
    if(op==0x9d || op==0xcf) return no(why,"instruction-flags-restore");
    if(op==0x9a || op==0xea || op==0xca || op==0xcb)
        return no(why,"instruction-far-transfer");
    if(op==0x07 || op==0x17 || op==0x1f || op==0x8e || op==0xc4 || op==0xc5)
        return no(why,"instruction-segment-load");
    if(op==0xf1 || op==0xce) return no(why,"instruction-int");
    if(op==0xcd) {
        if(n==bytes) return no(why,"instruction-truncated");
        if(p[n]!=3) return no(why,"instruction-int");
        immediate=1;
    } else if(op==0x0f) {
        if(n==bytes) return no(why,"instruction-truncated");
        second=p[n++];
        if(second==0x22) return no(why,"instruction-control-register-write");
        if(second==0x30) return no(why,"instruction-msr-write");
        if(second==0x00 || second==0x01) {
            if(n==bytes) return no(why,"instruction-truncated");
            unsigned ext=(p[n]>>3)&7;
            if(second==0x00 && ext==3) return no(why,"instruction-ltr");
            if(second==0x01 && ext==2) return no(why,"instruction-lgdt");
            if(second==0x01 && ext==3) return no(why,"instruction-lidt");
            return no(why,"instruction-system");
        }
        if(second==0x02 || second==0x03 || (second>=0x40 && second<=0x4f) ||
           (second>=0x90 && second<=0x9f) || second==0xa3 || second==0xa5 ||
           second==0xab || second==0xad || second==0xaf || second==0xb0 ||
           second==0xb1 || second==0xb3 || second==0xb6 || second==0xb7 ||
           second==0xbb || second==0xbc || second==0xbd || second==0xbe ||
           second==0xbf || second==0xc0 || second==0xc1 || second==0xc7) modrm=true;
        else if(second==0xa4 || second==0xac || second==0xba) { modrm=true; immediate=1; }
        else if(second>=0x80 && second<=0x8f) immediate=operand;
        else if(second==0x0b || second==0x31 || second==0xa2 ||
                (second>=0xc8 && second<=0xcf)) { /* UD2, RDTSC, CPUID, BSWAP */ }
        else return no(why,"instruction-outside-i686-policy");
    } else if(op<=0x3f) {
        unsigned low=op&7;
        if(low<=3) modrm=true;
        else if(low==4) immediate=1;
        else if(low==5) immediate=operand;
        /* segment pushes, decimal/ASCII adjust instructions have no tail */
    } else if(op<=0x61) { /* INC/DEC/PUSH/POP registers, PUSHA/POPA */ }
    else if(op==0x62 || op==0x63) modrm=true;
    else if(op==0x68) immediate=operand;
    else if(op==0x69) { modrm=true; immediate=operand; }
    else if(op==0x6a) immediate=1;
    else if(op==0x6b) { modrm=true; immediate=1; }
    else if(op<=0x6f) { /* string port I/O */ }
    else if(op<=0x7f) immediate=1;
    else if(op<=0x8f) {
        modrm=true;
        if(op==0x80 || op==0x82 || op==0x83) immediate=1;
        if(op==0x81) immediate=operand;
    } else if(op<=0x9f) { /* XCHG, conversion, PUSHF, LAHF/SAHF */ }
    else if(op<=0xa3) immediate=address;
    else if(op==0xa8) immediate=1;
    else if(op==0xa9) immediate=operand;
    else if(op<=0xaf) { /* string instructions */ }
    else if(op<=0xb7) immediate=1;
    else if(op<=0xbf) immediate=operand;
    else if(op==0xc0 || op==0xc1 || op==0xc6) { modrm=true; immediate=1; }
    else if(op==0xc2) immediate=2;
    else if(op==0xc3 || op==0xc9 || op==0xcc) { }
    else if(op==0xc7) { modrm=true; immediate=operand; }
    else if(op==0xc8) immediate=3;
    else if(op>=0xd0 && op<=0xd3) modrm=true;
    else if(op==0xd4 || op==0xd5) immediate=1;
    else if(op==0xd7) { }
    else if(op>=0xd8 && op<=0xdf) modrm=true; /* x87 lengths only */
    else if(op>=0xe0 && op<=0xe7) immediate=1;
    else if(op==0xe8 || op==0xe9) immediate=operand;
    else if(op==0xeb) immediate=1;
    else if(op>=0xec && op<=0xef) { /* register port I/O */ }
    else if(op==0xf6 || op==0xf7) { modrm=true; group3=true; }
    else if(op==0xfe || op==0xff) modrm=true;
    else if(op==0xf5 || op==0xf8 || op==0xf9 || op==0xfc || op==0xfd) { }
    else return no(why,"instruction-outside-i686-policy");
    if(modrm) {
        if(n==bytes) return no(why,"instruction-truncated");
        unsigned m=p[n++],mod=m>>6,rm=m&7,ext=(m>>3)&7;
        if(op==0xff && (ext==3 || ext==5)) return no(why,"instruction-far-transfer");
        if(op==0xff && ext==7) return no(why,"instruction-invalid-group");
        if(op==0xfe && ext>1) return no(why,"instruction-invalid-group");
        if((op==0xc6 || op==0xc7 || op==0x8f) && ext)
            return no(why,"instruction-invalid-group");
        if(group3) {
            if(ext==1) return no(why,"instruction-invalid-group");
            if(!ext) immediate=op==0xf6?1:operand;
        }
        if(second==0xba && ext<4) return no(why,"instruction-invalid-group");
        if(second==0xc7 && (ext!=1 || mod==3)) return no(why,"instruction-outside-i686-policy");
        if(mod!=3) {
            if(address==2) n+=mod==0?(rm==6?2:0):mod==1?1:2;
            else {
                if(rm==4) {
                    if(n>=bytes) return no(why,"instruction-truncated");
                    unsigned sib=p[n++];
                    if(!mod && (sib&7)==5) n+=4;
                }
                n+=mod==0?(rm==5?4:0):mod==1?1:4;
            }
        }
    }
    if(n>bytes || immediate>bytes-n) return no(why,"instruction-truncated");
    *length=n+immediate; return 1;
}
int ndis_scan_image(const struct pe_image *p,struct ndis_scan_error *error) {
    *error=(struct ndis_scan_error){0};
    if(!p || !p->image) { error->reason="scan-no-image"; return 0; }
    for(unsigned i=0;i<p->section_count;i++) {
        const struct pe_section *s=&p->sections[i];
        if(!(s->flags&0x20000000)) continue;
        if(s->rva>p->size || s->span>p->size-s->rva) {
            error->rva=s->rva; error->reason="scan-section-bounds"; return 0;
        }
        for(uint32_t offset=0;offset<s->span;) {
            uint32_t length;
            if(!ndis_instruction(p->image+s->rva+offset,s->span-offset,&length,&error->reason)) {
                error->rva=s->rva+offset; return 0;
            }
            offset+=length;
        }
    }
    return 1;
}
