/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/scan.h"
static void refused(const uint8_t *p,unsigned size,const char *reason) {
    uint32_t length=99; const char *why=0;
    assert(!ndis_instruction(p,size,&length,&why)); assert(!length);
    assert(why && !strcmp(why,reason));
}
#define NO(reason,...) do { const uint8_t p[]={__VA_ARGS__}; refused(p,sizeof(p),reason); } while(0)
#define YES(...) do { const uint8_t p[]={__VA_ARGS__}; uint32_t n=0; const char *w=0; \
    assert(ndis_instruction(p,sizeof(p),&n,&w) && n==sizeof(p) && !w); } while(0)
int main(void) {
    NO("instruction-cli",0xfa); NO("instruction-sti",0xfb); NO("instruction-hlt",0xf4);
    NO("instruction-cli",0x66,0x67,0xf3,0xfa);
    NO("instruction-lgdt",0x0f,0x01,0x10); NO("instruction-lidt",0x0f,0x01,0x18);
    NO("instruction-ltr",0x0f,0x00,0xd8);
    NO("instruction-control-register-write",0x0f,0x22,0xc0);
    NO("instruction-msr-write",0x0f,0x30);
    NO("instruction-int",0xcd,0x80); NO("instruction-int",0xce); NO("instruction-int",0xf1);
    NO("instruction-flags-restore",0x9d); NO("instruction-flags-restore",0xcf);
    NO("instruction-system",0x0f,0x01,0x38);
    NO("instruction-outside-i686-policy",0x0f,0x34);
    NO("instruction-outside-i686-policy",0x0f,0x10,0xc0);
    NO("instruction-far-transfer",0xff,0x18);
    NO("instruction-segment-load",0x8e,0xd0);
    NO("instruction-invalid-group",0xf7,0xc8);
    NO("instruction-truncated",0xb8,0x00); NO("instruction-truncated",0x8b,0x04);
    YES(0xcc); YES(0xcd,3); YES(0x0f,0x0b);
    YES(0xe4,0xfa); YES(0xe5,0xfb); YES(0xe6,0xf4); YES(0xe7,0xcd);
    YES(0xec); YES(0xed); YES(0xee); YES(0xef);
    YES(0x6c); YES(0xf3,0x6f);
    YES(0xb8,0xfa,0xfb,0xf4,0xcd); /* immediate bytes are not opcodes */
    YES(0x8b,0x84,0x8d,0xfa,0xfb,0xf4,0xcd); /* SIB + disp32 */
    YES(0x67,0x8b,0x86,0xfa,0xfb); /* disp16 */
    YES(0x66,0x66,0xb8,0xfa,0xfb); /* repeated override is not a toggle */
    YES(0xf7,0xc0,1,2,3,4); YES(0xf7,0xf1);
    YES(0x0f,0x84,1,2,3,4); YES(0x66,0x0f,0x84,1,2);
    YES(0x0f,0x44,0x04,0x25,1,2,3,4); YES(0x0f,0xba,0x28,7);
    uint8_t prefixes[16]; memset(prefixes,0x66,sizeof(prefixes)); prefixes[15]=0x90;
    refused(prefixes,sizeof(prefixes),"instruction-truncated"); prefixes[14]=0x90;
    uint32_t length; const char *why;
    assert(ndis_instruction(prefixes,15,&length,&why) && length==15);
    uint8_t data[]={0x90,0xb8,0xfa,0xfb,0xf4,0xcd,0xfa};
    struct pe_image image={.image=data,.size=sizeof(data),.section_count=1,
        .sections={{.span=sizeof(data),.flags=0x20000000}}};
    struct ndis_scan_error error;
    assert(!ndis_scan_image(&image,&error) && error.rva==6 && !strcmp(error.reason,"instruction-cli"));
    image.sections[0].flags=0; assert(ndis_scan_image(&image,&error));
    /* Admission limitation, deliberately not a claimed containment proof:
     * MOV's immediate holds CLI/JMP -3; the final JMP enters that immediate.
     * A length scan accepts it. Production entry MUST remain closed. */
    uint8_t alternate_entry[]={0xb8,0xfa,0xeb,0xfd,0x00,0xeb,0xfa};
    image.image=alternate_entry; image.size=sizeof(alternate_entry);
    image.sections[0].span=sizeof(alternate_entry); image.sections[0].flags=0x20000000;
    assert(ndis_scan_image(&image,&error));
    image.sections[0].flags=0x20000000; image.sections[0].span=999;
    assert(!ndis_scan_image(&image,&error) && !strcmp(error.reason,"scan-section-bounds"));
    /* Every bounded input must terminate without reading past its extent. */
    uint32_t random=17;
    for(unsigned i=0;i<50000;i++) {
        uint8_t bytes[16];
        for(unsigned j=0;j<sizeof(bytes);j++) { random=random*1664525u+1013904223u; bytes[j]=random>>24; }
        for(unsigned n=0;n<=16;n++) {
            int ok=ndis_instruction(bytes,n,&length,&why);
            assert(ok?(length && length<=n && length<=15 && !why):(!length && why));
        }
    }
    puts("PASS instruction admission lengths bounds dangerous instructions and port I/O");
    fflush(stdout);
}
