/* H50: production service calls with guarded guest memory and actual FAT/VFS. */
static void h50_far(uint8_t *p,unsigned off,unsigned seg) { fs_wr16(p,(uint16_t)off); fs_wr16(p+2,(uint16_t)seg); }
static void h50_nls(void) {
    uint8_t rom[4096]={0}; dos_file_hook_rom(rom); memcpy(guest+0xf1000,rom,sizeof(rom));
    bridge.personality.country=(struct dos_file_far){DOS_HOOK_COUNTRY,0xf000};
    bridge.personality.upper=bridge.personality.filename=(struct dos_file_far){DOS_HOOK_UPPER,0xf000};
    bridge.personality.lower=(struct dos_file_far){DOS_HOOK_LOWER,0xf000};
    bridge.personality.terminator=(struct dos_file_far){DOS_HOOK_TERMINATOR,0xf000};
    bridge.personality.collating=(struct dos_file_far){DOS_HOOK_COLLATING,0xf000};
    bridge.personality.dbcs=(struct dos_file_far){DOS_HOOK_DBCS,0xf000};
    struct dos_file_frame f;
    for (unsigned al=1;al<=7;al++) {
        memset(guest+0x50800,0xa7,45); f=frame(0x6500|al); f.bx=437; f.dx=1; f.cx=41; f.di=0x801;
        f=call(f); CHECK(!(f.flags&1) && f.cx==(al==1 ? 41 : 5));
        CHECK(guest[0x50800]==0xa7 && guest[0x50801+f.cx]==0xa7 && guest[0x50801]==al);
        if (al==1) CHECK(fs_rd16(guest+0x50802)==38 && fs_rd16(guest+0x50804)==1 && fs_rd16(guest+0x50806)==437);
        else {
            unsigned off=fs_rd16(guest+0x50802),seg=fs_rd16(guest+0x50804);
            CHECK(seg==0xf000 && off>=DOS_HOOK_UPPER && off<DOS_HOOK_END);
            if (al==7) CHECK(!fs_rd32(guest+seg*16+off));
        }
        f=frame(0x6500|al); f.bx=437; f.dx=1; f.di=0x801; f.cx=4; f=call(f); CHECK((f.flags&1) && f.ax==87);
        f=frame(0x6500|al); f.bx=850; f.dx=1; f.di=0x801; f.cx=41; f=call(f); CHECK(f.flags&1);
        f=frame(0x6500|al); f.bx=437; f.dx=39; f.di=0x801; f.cx=41; f=call(f); CHECK(f.flags&1);
        f=frame(0x6500|al); f.bx=f.dx=65535; f.di=0xfffd; f.cx=41; f=call(f); CHECK(f.flags&1);
    }
    for (unsigned length=5;length<=45;length++) {
        memset(guest+0x50800,0xa7,46); f=frame(0x6501); f.bx=437; f.dx=1; f.cx=(uint16_t)length; f.di=0x801;
        f=call(f); CHECK(!(f.flags&1) && f.cx==(length<41 ? length : 41));
        CHECK(guest[0x50800]==0xa7 && guest[0x50801+f.cx]==0xa7 && fs_rd16(guest+0x50802)==38);
    }
    for (unsigned byte=0;byte<256;byte++) {
        int upper=path_cp437(path_upper(path_from_cp437((uint8_t)byte))); if (upper<0) upper=(int)byte;
        f=frame(0x6520); f.dx=(uint16_t)(0x5a00|byte); f=call(f); CHECK(!(f.flags&1) && f.dx==(0x5a00|upper));
        if (byte>=128) CHECK(guest[0xf0000+DOS_HOOK_UPPER+2+byte-128]==upper);
        guest[0x50a00+byte]=(uint8_t)byte;
    }
    f=frame(0x6521); f.dx=0xa00; f.cx=256; f=call(f); CHECK(!(f.flags&1));
    for (unsigned byte=0;byte<256;byte++) {
        int upper=path_cp437(path_upper(path_from_cp437((uint8_t)byte))); CHECK(guest[0x50a00+byte]==(upper<0 ? byte : (unsigned)upper));
    }
    f=frame(0x6521); f.dx=65535; f.cx=0; f=call(f); CHECK(!(f.flags&1));
    f.cx=2; f=call(f); CHECK(f.flags&1);
    memcpy(guest+0x50a00,"ab\x81\0after",9); f=frame(0x6522); f.dx=0xa00; f=call(f);
    CHECK(!(f.flags&1) && !memcmp(guest+0x50a00,"AB\x9a\0after",9));
    guest[0x5ffff]='x'; f=frame(0x6522); f.dx=65535; f=call(f); CHECK(f.flags&1);
    const char choices[]="YyNn?"; const unsigned answers[]={1,1,0,0,2};
    for (unsigned i=0;i<5;i++) { f=frame(0x6523); f.dx=(uint8_t)choices[i]; f=call(f); CHECK(f.ax==answers[i] && !(f.flags&1)); }
    f=call(frame(0x6601)); CHECK(f.bx==437 && f.dx==437);
    f=frame(0x6602); f.bx=f.dx=437; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x6602); f.bx=f.dx=850; f=call(f); CHECK(f.flags&1);
    f=frame(0x3800); f.dx=0x800; f=call(f); CHECK(!(f.flags&1) && f.ax==1 && f.bx==1 && guest[0x50802]=='$');
    f=frame(0x3802); f.dx=0x800; f=call(f); CHECK(f.flags&1);
    puts("H50-04 NLS: PASS subfunctions=11 bytes=256 guards/count/NUL/country/page/38/66");
}
static struct dos_file_frame h50_fcb(unsigned ah,unsigned off,unsigned count) {
    struct dos_file_frame f=frame(ah<<8); f.dx=(uint16_t)off; f.cx=(uint16_t)count; return call(f);
}
static void h50_fcb_name(unsigned off,const char name[11],bool extended) {
    uint8_t *p=guest+0x50000+off; memset(p,0,extended ? 44 : 37);
    if (extended) { p[0]=255; p[6]=FAT_ATTR_HIDDEN; p+=7; }
    p[0]=3; memcpy(p+1,name,11);
}
static void h50_fcbs(void) {
    uint64_t seq=bridge.sequence; struct dos_file_frame f=frame(0x1a00); f.dx=0x1000; f=call(f); CHECK(!(f.flags&1));
    h50_fcb_name(0x800,"FCB     DAT",false); uint8_t *b=guest+0x50800;
    b[32]=7; fs_wr32(b+33,0x12345678); f=h50_fcb(0x16,0x800,0);
    CHECK((f.ax&255)==0 && f.flags==frame(0).flags && fs_rd16(b+14)==128 && b[32]==7 && fs_rd32(b+33)==0x12345678);
    for (unsigned i=5;i<20;i++) CHECK(guest[0x20018+i]==255); /* FCB consumes no JFT slot. */
    memset(guest+0x51000,'F',512); b[32]=127; fs_wr16(b+12,0); f=h50_fcb(0x15,0x800,0);
    CHECK((f.ax&255)==0 && fs_rd16(b+12)==1 && b[32]==0 && fs_rd32(b+16)==16384);
    fs_wr16(b+14,1); fs_wr32(b+33,0); f=h50_fcb(0x28,0x800,255);
    CHECK((f.ax&255)==0 && f.cx==255 && fs_rd32(b+33)==255 && fs_rd16(b+12)==1 && b[32]==127);
    f=h50_fcb(0x24,0x800,0); CHECK((f.ax&255)==0 && fs_rd32(b+33)==255);
    fs_wr32(b+33,5); f=h50_fcb(0x28,0x800,0); CHECK((f.ax&255)==0 && !f.cx && fs_rd32(b+16)==5 && fs_rd32(b+33)==5);
    fs_wr16(b+14,128); fs_wr32(b+33,0x5a000000); b[32]=42;
    memset(guest+0x51000,0xa7,130); f=h50_fcb(0x21,0x800,0);
    CHECK((f.ax&255)==3 && fs_rd32(b+33)==0x5a000000 && fs_rd16(b+12)==0 && b[32]==0);
    CHECK(guest[0x51000]=='F' && guest[0x51004]=='F' && guest[0x51005]==0 && guest[0x5107f]==0 && guest[0x51080]==0xa7);
    fs_wr32(b+33,1); f=h50_fcb(0x21,0x800,0); CHECK((f.ax&255)==1 && b[32]==1 && fs_rd32(b+33)==1);
    fs_wr16(b+12,0); b[32]=0; f=h50_fcb(0x14,0x800,0); CHECK((f.ax&255)==3 && b[32]==1);
    fs_wr32(b+33,0); f=h50_fcb(0x27,0x800,3); CHECK((f.ax&255)==3 && f.cx==1 && fs_rd32(b+33)==1 && b[32]==1);
    fs_wr16(b+14,0); f=h50_fcb(0x14,0x800,0); CHECK((f.ax&255)==1);
    f=frame(0x5900); f.bx=0; f=call(f); CHECK(f.ax==87);
    fs_wr16(b+14,128); f=frame(0x1a00); f.dx=0xffe0; f=call(f); CHECK(f.flags&1); /* 43-byte handle DTA precondition */
    bridge.process[0].dta=(struct dos_file_far){0xfff0,0x5000};
    f=h50_fcb(0x14,0x800,0); CHECK((f.ax&255)==2);
    CHECK(guest[0x5ffff]=='x');
    bridge.process[0].dta=(struct dos_file_far){0x1000,0x5000};
    fs_wr16(b+14,65535); f=h50_fcb(0x27,0x800,2); CHECK((f.ax&255)==2 && f.cx==0);
    fs_wr16(b+14,63); fs_wr32(b+33,0xffffffff); f=h50_fcb(0x22,0x800,0); CHECK((f.ax&255)==1);
    fs_wr16(b+14,128); f=h50_fcb(0x10,0x800,0); CHECK((f.ax&255)==0);
    f=h50_fcb(0x10,0x800,0); CHECK((f.ax&255)==0);
    f=h50_fcb(0x0f,0x800,0); CHECK((f.ax&255)==0 && fs_rd16(b+14)==128 && fs_rd32(b+16)==5);
    fs_wr16(b+14,3); f=h50_fcb(0x23,0x800,0); CHECK((f.ax&255)==0 && fs_rd32(b+33)==2);
    b[24]=0x7f; b[25]=0xa5; fs_wr32(b+26,0xdeadbeef); /* reserved data grants no authority */
    f=h50_fcb(0x10,0x800,0); CHECK((f.ax&255)==0);
    h50_fcb_name(0x900,"SECOND  DAT",true); uint8_t *ext=guest+0x50907;
    f=h50_fcb(0x16,0x900,0); CHECK((f.ax&255)==0);
    CHECK((f=h50_fcb(0x0f,0x800,0),!(f.ax&255)));
    ext[1]='X'; f=h50_fcb(0x15,0x900,0); CHECK((f.ax&255)==255); ext[1]='S';
    f=h50_fcb(0x10,0x900,0); CHECK((f.ax&255)==0);
    f=h50_fcb(0x10,0x800,0); CHECK((f.ax&255)==0);
    h50_fcb_name(0xa00,"????????DAT",true); guest[0x50a06]=FAT_ATTR_HIDDEN;
    memset(guest+0x50fff,0xa7,46); f=h50_fcb(0x11,0xa00,0); CHECK((f.ax&255)==0 && guest[0x51000]==255 && guest[0x50fff]==0xa7 && guest[0x5102c]==0xa7);
    unsigned found=1;
    while (!(f=h50_fcb(0x12,0xa00,0),f.ax&255)) { CHECK(++found<=2); }
    CHECK(found==2 && (f.ax&255)==255);
    h50_fcb_name(0xa00,"FCB     DAT",false); f=h50_fcb(0x11,0xa00,0); CHECK(!(f.ax&255) && !memcmp(guest+0x51001,"FCB     DAT",11));
    memcpy(guest+0x50a11,"NEW     ???",11); f=h50_fcb(0x17,0xa00,0); CHECK(!(f.ax&255));
    h50_fcb_name(0xa00,"NEW     DAT",false); f=h50_fcb(0x0f,0xa00,0); CHECK(!(f.ax&255));
    f=h50_fcb(0x10,0xa00,0); CHECK(!(f.ax&255));
    h50_fcb_name(0xa00,"????????DAT",true); guest[0x50a06]=FAT_ATTR_HIDDEN;
    f=h50_fcb(0x13,0xa00,0); CHECK(!(f.ax&255));
    f=h50_fcb(0x11,0xa00,0); CHECK((f.ax&255)==255);
    h50_fcb_name(0xb00,"BAD     DAT",false); guest[0x50b00]=27;
    f=h50_fcb(0x0f,0xb00,0); CHECK((f.ax&255)==255);
    f=frame(0x5900); f.bx=0; f=call(f); CHECK(f.ax==15);
    /* Parser preserves ES:DI and selected fields; counted CP437 case agrees. */
    memset(guest+0x50c00,0xa7,37); memcpy(guest+0x50d00," ; c:ab*.\x81? rest",17);
    f=frame(0x2901); f.si=0xd00; f.di=0xc00; uint16_t flags=f.flags; f=call(f);
    CHECK((f.ax&255)==1 && f.si==0xd0b && f.di==0xc00 && f.es==0x5000 && f.flags==flags);
    CHECK(guest[0x50c00]==3 && !memcmp(guest+0x50c01,"AB??????",8) && guest[0x50c09]==0x9a && guest[0x50c0a]=='?');
    CHECK(guest[0x50c20]==0xa7 && fs_rd16(guest+0x50c0c)==0 && fs_rd16(guest+0x50c0e)==0);
    memcpy(guest+0x50d00,"\0",1); f=frame(0x290e); f.si=0xd00; f.di=0xc00; f=call(f);
    CHECK(!(f.ax&255) && guest[0x50c00]==3 && !memcmp(guest+0x50c01,"AB??????",8));
    memcpy(guest+0x50d00,"Z:a\0",4); f=frame(0x2900); f.si=0xd00; f.di=0xc00; f=call(f); CHECK((f.ax&255)==255 && f.si==0xd03);
    printf("H50-03 FCB: PASS selectors=0F-17,21-24,27/28,29 calls=%" PRIu64 " AL/CX/FCB/DTA checked; sector API entries=0\n",bridge.sequence-seq);
}
static void h50_internal(void) {
    unsigned h=create_file("C:/INTERNAL.DAT"); unsigned sft=guest[0x20018+h];
    struct dos_file_frame f=frame(0x1203);
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && f.ds==0x8000);
    f=frame(0x1216); f.bx=(uint16_t)sft;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && !(f.flags&1) && f.es==0x8200 && f.di==6+59*sft);
    CHECK(fs_rd16(guest+0x82000+f.di)==1);
    fs_wr16(guest+0x82000+f.di,0xffff); /* mirror forgery doesn't acquire a reference */
    f=frame(0x1220); f.bx=(uint16_t)h;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && f.es==0x2000 && f.di==0x18+h);
    CHECK(bridge.sft[sft].refs==1 && fs_rd16(guest+0x82006+59*sft)==1);
    f=frame(0x1216); f.bx=128; CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && (f.flags&1) && f.ax==6);
    f=frame(0x122d); CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && f.ax==6);
    f=frame(0x1220); f.bx=233; CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && (f.ax&255)==6);
    path_set("C:/INTERNAL.DAT"); f=frame(0x1226); f.dx=0x100; f.cx=0;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && (f.flags&1));
    bridge.personality.live=true; bridge.personality.stack_seg=0x6000; bridge.personality.stack_off=0x7000; bridge.personality.stack_bytes=0x2000;
    f=frame(0x1226); f.dx=0x100; f.cx=0;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_DONE && !(f.flags&1)); close_file(f.ax);
    bridge.personality.live=false;
    for (unsigned ax=0x1100;ax<=0x1aff;ax+=0x100) { f=frame(ax); struct dos_file_frame before=f;
        CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x2f)==DOS_FILE_CHAIN && !memcmp(&f,&before,sizeof(f))); }
    bridge.dispatch_depth=1; f=frame(0x3e00); f.bx=(uint16_t)h;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x21)==DOS_FILE_DONE && (f.flags&1) && f.ax==5);
    CHECK(bridge.sft[sft].refs==1); bridge.dispatch_depth=0; close_file(h);
    CHECK(guest[0x80001]==0 && guest[0x83000]==0);
    puts("H50-02 internals: PASS 1203/1216/1220/1226/122D mirror-forgery/stack/recursion guards");
}
static struct dos_file candidate;
static void h50_hook(void) {
    struct dos_file_frame f=frame(0xc148),before;
    f.bx=0x4644; f.cx=1; f.dx=0x564d; before=f;
    CHECK(dos_file_hook_discover(&f) && f.ax==0 && f.cx==1 && f.es==0xf000 && f.di==DOS_HOOK_DESCRIPTOR);
    for (unsigned field=0;field<4;field++) {
        f=before; if (field==0) f.ax--; if (field==1) f.bx--; if (field==2) f.cx--; if (field==3) f.dx--;
        struct dos_file_frame unchanged=f; CHECK(!dos_file_hook_discover(&f) && !memcmp(&f,&unchanged,sizeof(f)));
    }
    uint8_t raw[68]={0}; fs_wr16(raw,1); fs_wr16(raw+2,68); h50_far(raw+4,0x100,0x8000);
    h50_far(raw+8,0,0x8000); fs_wr16(raw+12,0x30); h50_far(raw+14,0,0x8100);
    h50_far(raw+18,0,0x8200); fs_wr16(raw+22,59); fs_wr16(raw+24,128);
    h50_far(raw+26,0,0x8600); h50_far(raw+30,0x10,0x8600); h50_far(raw+34,0x200,0x8000);
    h50_far(raw+38,0x201,0x8000); h50_far(raw+42,0,0x8400); h50_far(raw+46,0x202,0x8000);
    h50_far(raw+50,0x300,0x8000); h50_far(raw+54,0,0x8800); h50_far(raw+58,0,0x9000);
    fs_wr16(raw+62,0x5000); fs_wr16(raw+64,0x8000); fs_wr16(raw+66,0x800);
    for (unsigned i=0;i<4;i++) h50_far(guest+0x80300+4*i,0x20*i,0x8700);
    fs_wr16(guest+0x20000,0x20cd);
    f=frame(0); f.si=0x1000; f.bx=f.cx=0x2000;
    for (unsigned fault=0;fault<14;fault++) {
        uint8_t bad[68]; memcpy(bad,raw,68);
        switch (fault) {
        case 0: fs_wr16(bad,0); break;
        case 1: fs_wr16(bad+2,67); break;
        case 2: fs_wr16(bad+22,58); break;
        case 3: fs_wr16(bad+24,129); break;
        case 4: fs_wr16(bad+12,31); break;
        case 5: h50_far(bad+18,0xfffe,0x8200); break;
        case 6: h50_far(bad+58,0xff00,0x9000); break;
        case 7: h50_far(bad+34,0,0x8000); break; /* overlaps SDA */
        case 8: h50_far(bad+4,0,0x8000); break;
        case 9: fs_wr16(bad+64,0x7000); break;
        case 10: fs_wr16(bad+66,0); break;
        case 11: h50_far(bad+50,0xfffe,0x8000); break;
        case 12: h50_far(bad+26,0xffff,0x8000); break;
        case 13: h50_far(bad+4,0x100,0x7000); break;
        }
        dos_file_init(&candidate,50,1,&ops); OK(dos_file_map(&candidate,2,&table,2,false));
        memcpy(guest+0x51000,bad,68);
        CHECK(dos_file_hook_bind(&candidate,&f,0x8000)<0 && !candidate.bound && !candidate.owning && !pins);
    }
    const unsigned pointer_fields[]={8,14,18,26,30,34,38,42,46,58};
    for (unsigned n=0;n<sizeof(pointer_fields)/sizeof(pointer_fields[0]);n++) {
        uint8_t bad[68]; memcpy(bad,raw,68); h50_far(bad+pointer_fields[n],0x1800,0xf000);
        dos_file_init(&candidate,50,1,&ops); OK(dos_file_map(&candidate,2,&table,2,false));
        memcpy(guest+0x51000,bad,68);
        CHECK(dos_file_hook_bind(&candidate,&f,0x8000)<0 && !candidate.bound && !candidate.owning && !pins);
    }
    memcpy(guest+0x51000,raw,68); dos_file_init(&candidate,50,1,&ops); OK(dos_file_map(&candidate,2,&table,2,false));
    OK(dos_file_hook_bind(&candidate,&f,0x8000)); CHECK(candidate.bound && candidate.personality.live && candidate.owning==0x2000);
    CHECK(dos_file_hook_bind(&candidate,&f,0x8000)<0 && candidate.owning==0x2000);
    uint8_t event[8]={1,0,8,0,0,0,0,0}; memcpy(guest+0x51000,event,8);
    for (unsigned event_code=1;event_code<=8;event_code++) for (unsigned fault=0;fault<3;fault++) {
        uint8_t invalid[8]; memcpy(invalid,event,8);
        fs_wr16(invalid+(fault==0 ? 0 : fault==1 ? 2 : 6),fault==0 ? 0 : fault==1 ? 7 : 1);
        memcpy(guest+0x51000,invalid,8); f.ax=(uint16_t)event_code;
        CHECK(dos_file_hook_event(&candidate,&f,0x8000)<0 && candidate.bound && candidate.owning==0x2000 && candidate.selected==0x2000);
        for (unsigned i=0;i<DOS_FILE_SFT;i++) CHECK(candidate.sft[i].refs==(i<5 ? 1u : 0u));
    }
    memcpy(guest+0x51000,event,8);
    f.ax=3; f.bx=0x3000; f.cx=0x2000;
    CHECK(dos_file_hook_event(&candidate,&f,0x7000)<0); OK(dos_file_hook_event(&candidate,&f,0x8000));
    CHECK(candidate.owning==0x2000 && candidate.process[1].provisional);
    f.ax=4; OK(dos_file_hook_event(&candidate,&f,0x8000)); CHECK(candidate.owning==0x3000);
    f.ax=2; f.bx=0x2000; OK(dos_file_hook_event(&candidate,&f,0x8000)); CHECK(candidate.owning==0x3000 && candidate.selected==0x2000);
    f.ax=3; f.bx=0x4000; f.cx=0x2000; CHECK(dos_file_hook_event(&candidate,&f,0x8000)<0);
    f.cx=0x3000; OK(dos_file_hook_event(&candidate,&f,0x8000));
    f.ax=5; OK(dos_file_hook_event(&candidate,&f,0x8000)); CHECK(candidate.owning==0x3000);
    f.ax=7; f.bx=0x3000; OK(dos_file_hook_event(&candidate,&f,0x8000)); CHECK(candidate.owning==0x2000 && candidate.process[1].resident);
    f.ax=1; f.bx=0x5000; f.cx=0x2000; fs_wr16(guest+0x51004,0x55); OK(dos_file_hook_event(&candidate,&f,0x8000));
    CHECK(candidate.selected==0x5000 && candidate.owning==0x2000);
    f.ax=6; fs_wr16(guest+0x51004,0); OK(dos_file_hook_event(&candidate,&f,0x8000));
    f.ax=1; f.bx=0x6000; f.cx=0x2000; fs_wr16(guest+0x51004,0x26); OK(dos_file_hook_event(&candidate,&f,0x8000));
    CHECK(candidate.selected==0x2000 && !fs_rd16(guest+0x60016));
    fs_wr16(guest+0x51004,0); f.ax=8; OK(dos_file_hook_event(&candidate,&f,0x8000)); CHECK(candidate.dead);
    puts("H50-01 binding/lifetime: PASS malformed_bindings=24 malformed_events=24 signature/duplicate/ancestry/55/26/rollback/TSR/destroy");
}

static void h50_partial_retry(void) {
    unsigned h=create_file("C:/PARTIAL.DAT");
    memset(guest+0x50400,'A',4096); memset(guest+0x51400,'B',4096);
    struct dos_file_frame f=transfer_file(0x4000,h,8192); CHECK(!(f.flags&1) && f.ax==8192);
    f=seek_file(h,0,0); CHECK(!(f.flags&1));
    unsigned index=bridge.process[0].handles[h];
    struct vfs_description *description=table.handles[bridge.sft[index].handle];
    uint32_t first=volume.data_sector+(description->node->entry.first-2)*volume.spc;
    uint8_t sector[512];
    for (unsigned lba=4000;lba<4300;lba++) OK(cache_read(&cache,&device,lba,sector));
    memset(guest+0x50400,0xa7,8193); fail_lba=first+8;
    f=frame(0x3f00); f.bx=(uint16_t)h; f.cx=8192; f.dx=0x400;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_HOOK,0x21)==DOS_FILE_PENDING);
    CHECK(!fail_lba && bridge.transferred==4096 && description->position==4096);
    CHECK(guest[0x50400]=='A' && guest[0x513ff]=='A' && guest[0x51400]==0xa7);
    guest[0x5040d]='Z'; /* A callback may touch memory already returned. */
    CHECK(dos_file_complete(&bridge,callback_token,1,&f)==DOS_FILE_DONE && !(f.flags&1) && f.ax==8192);
    CHECK(description->position==8192 && guest[0x5040d]=='Z' && guest[0x51400]=='B' && guest[0x523ff]=='B' && guest[0x52400]==0xa7);
    close_file(h);
    puts("H50-02 continuation: PASS partial read prefix=4096 resume=4096 bytes=8192 replay=0");
}
