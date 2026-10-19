/* Packed private DFVM protocol. All extents checked before publishing BIND. */
void dos_file_hook_rom(uint8_t *page) {
    uint8_t *rom=page; /* protected page at F100:0000; offsets are F000-relative */
    const unsigned gates[]={DOS_HOOK_DISPATCH,DOS_HOOK_EVENT,DOS_HOOK_CRITICAL};
    for (unsigned i=0;i<3;i++) { uint8_t *g=rom+(gates[i]-0x1000u); g[0]=0xcd; g[1]=0xfe; g[2]=i==2 ? 0xf4 : 0xcb; g[3]=(uint8_t)(i==0 ? 0xd1 : i==1 ? 0xe1 : 0xc1); }
    uint8_t *b=rom+(DOS_HOOK_DESCRIPTOR-0x1000u);
    memcpy(b,"DFVM",4); fs_wr16(b+4,1); fs_wr16(b+6,20); fs_wr32(b+8,7);
    far_write(b+12,(struct dos_file_far){DOS_HOOK_DISPATCH,DOS_HOOK_ROM_SEG});
    far_write(b+16,(struct dos_file_far){DOS_HOOK_EVENT,DOS_HOOK_ROM_SEG});
    static const uint8_t casemap[]={0x53,0x3c,0x61,0x72,0x08,0x3c,0x7a,0x77,0x04,0x2c,0x20,0xeb,0x09,
        0x3c,0x80,0x72,0x05,0xbb,(DOS_HOOK_UPPER+2-128)&255,(DOS_HOOK_UPPER+2-128)>>8,0x2e,0xd7,0x5b,0xcb};
    memcpy(rom+(DOS_HOOK_CASEMAP-0x1000u),casemap,sizeof(casemap));
    b=rom+(DOS_HOOK_COUNTRY-0x1000u); memset(b,0,34); b[2]='$'; b[7]=','; b[9]='.'; b[11]='-'; b[13]=':'; b[16]=2; b[22]=','; far_write(b+18,(struct dos_file_far){DOS_HOOK_CASEMAP,DOS_HOOK_ROM_SEG});
    b=rom+(DOS_HOOK_UPPER-0x1000u); fs_wr16(b,128);
    for (unsigned i=0;i<128;i++) b[2+i]=dos_upper((uint8_t)(i+128));
    b=rom+(DOS_HOOK_LOWER-0x1000u); fs_wr16(b,256);
    for (unsigned i=0;i<256;i++) {
        b[i+2]=(uint8_t)i;
        if (i>='A' && i<='Z') b[i+2]=(uint8_t)(i+32);
        else for (unsigned j=128;j<256;j++) if (j!=i && dos_upper((uint8_t)j)==i) { b[i+2]=(uint8_t)j; break; }
    }
    b=rom+(DOS_HOOK_TERMINATOR-0x1000u); memset(b,0,32); fs_wr16(b,22); b[2]=1; b[3]=0x20; b[4]=255;
    b[6]=0x7f; b[7]=0x7f; b[8]=2; b[9]=14; memcpy(b+10,".\"/\\[]:|<>+=;,",14);
    b=rom+(DOS_HOOK_COLLATING-0x1000u); fs_wr16(b,256); for (unsigned i=0;i<256;i++) b[i+2]=dos_upper((uint8_t)i);
    memset(rom+(DOS_HOOK_DBCS-0x1000u),0,4);
}
bool dos_file_hook_discover(struct dos_file_frame *f) {
    if (f->ax!=0xc148 || f->bx!=0x4644 || f->cx!=1 || f->dx!=0x564d) return false;
    f->ax=0; f->cx=1; f->es=DOS_HOOK_ROM_SEG; f->di=DOS_HOOK_DESCRIPTOR; return true;
}
static bool hook_distinct(struct dos_file *d,struct dos_file_far a,unsigned an,struct dos_file_far b,unsigned bn) {
    uint32_t x=address(d,a),y=address(d,b); return x+an<=y || y+bn<=x;
}
int dos_file_hook_bind(struct dos_file *d,struct dos_file_frame *f,uint16_t caller_cs) {
    struct dos_file_far arg={f->si,f->ds}; uint8_t raw[DOS_HOOK_BIND_BYTES],device[16];
    int e=BAD_POINTER;
    if (d->bound || d->dead || f->bx!=f->cx || !f->bx || !check(d,arg,sizeof(raw),false)) goto done;
    read_guest(d,arg,raw,sizeof(raw));
    if (fs_rd16(raw)!=1 || fs_rd16(raw+2)!=sizeof(raw) || fs_rd16(raw+22)!=59) goto done;
    struct dos_file_binding b={.sda=far_read(raw+8),.sda_bytes=fs_rd16(raw+12),.lists=far_read(raw+14),
        .sft=far_read(raw+18),.sft_count=fs_rd16(raw+24),.selected_psp=far_read(raw+26),.dta=far_read(raw+30),
        .indos=far_read(raw+38),.media=far_read(raw+42),.critical_return={DOS_HOOK_CRITICAL,DOS_HOOK_ROM_SEG}};
    struct dos_file_personality personality={.old21=far_read(raw+4),.result=far_read(raw+34),.drive=far_read(raw+46),
        .expanded=far_read(raw+58),.code_seg=caller_cs,.stack_off=fs_rd16(raw+62),.stack_seg=fs_rd16(raw+64),.stack_bytes=fs_rd16(raw+66)};
    struct dos_file_far drivers=far_read(raw+50),dpb=far_read(raw+54);
    if (personality.old21.seg!=caller_cs || b.sda.seg!=caller_cs || personality.result.seg!=caller_cs ||
        personality.stack_seg!=caller_cs || !personality.stack_bytes || b.sft_count<5 || b.sft_count>DOS_FILE_SFT || b.sda_bytes<32 ||
        !check(d,personality.old21,3,false) || !check(d,b.sda,b.sda_bytes,true) ||
        !check(d,b.sft,6+59*b.sft_count,true) || !check(d,b.lists,8,true) ||
        !check(d,b.selected_psp,2,true) || !check(d,b.dta,4,true) || !check(d,b.indos,1,true) ||
        !check(d,b.media,26,true) || !check(d,personality.result,1,true) || !check(d,personality.drive,1,true) ||
        !check(d,personality.expanded,DOS_FILE_PSPS*DOS_FILE_JFT,true) || !check(d,drivers,16,false) || !check(d,dpb,0x21,false) ||
        !check(d,(struct dos_file_far){personality.stack_off,personality.stack_seg},personality.stack_bytes,true) ||
        !check(d,(struct dos_file_far){0,f->bx},256,true)) goto done;
    /* Writable control records cannot overwrite the code, result or each other. */
    struct dos_file_far spans[]={b.sda,b.sft,b.lists,personality.expanded,{0,f->bx},personality.result};
    unsigned lengths[]={b.sda_bytes,6+59*b.sft_count,8,DOS_FILE_PSPS*DOS_FILE_JFT,256,1};
    for (unsigned i=0;i<6;i++) {
        if (!hook_distinct(d,spans[i],lengths[i],personality.old21,3)) goto done;
        for (unsigned j=0;j<i;j++) if (!hook_distinct(d,spans[i],lengths[i],spans[j],lengths[j])) goto done;
    }
    uint8_t psp[2]; read_guest(d,(struct dos_file_far){0,f->bx},psp,2); if (fs_rd16(psp)!=0x20cd) goto done;
    read_guest(d,drivers,device,sizeof(device));
    static const char *names[]={"CON","AUX","PRN","NUL"};
    for (unsigned i=0;i<4;i++) {
        uint8_t header[18]; struct dos_file_far at=far_read(device+4*i);
        if (!check(d,at,sizeof(header),false)) goto done;
        read_guest(d,at,header,sizeof(header));
        if (!(fs_rd16(header+4)&0x8000) || memcmp(header+10,names[i],strlen(names[i]))) goto done;
    }
    personality.country=(struct dos_file_far){DOS_HOOK_COUNTRY,DOS_HOOK_ROM_SEG};
    personality.upper=personality.filename=(struct dos_file_far){DOS_HOOK_UPPER,DOS_HOOK_ROM_SEG};
    personality.lower=(struct dos_file_far){DOS_HOOK_LOWER,DOS_HOOK_ROM_SEG};
    personality.terminator=(struct dos_file_far){DOS_HOOK_TERMINATOR,DOS_HOOK_ROM_SEG};
    personality.collating=(struct dos_file_far){DOS_HOOK_COLLATING,DOS_HOOK_ROM_SEG};
    personality.dbcs=(struct dos_file_far){DOS_HOOK_DBCS,DOS_HOOK_ROM_SEG};
    /* All fallible guest validation is finished. Native preparation has no I/O. */
    b.critical_device=far_read(device);
    d->binding=b; d->personality=personality;
    for (unsigned i=0;i<4;i++) {
        struct dos_file_device *dev=&d->device[i]; memset(dev,0,sizeof(*dev));
        memcpy(dev->name,names[i],strlen(names[i])); dev->header=far_read(device+4*i); dev->info=i==3 ? 0x84 : 0x83; dev->used=true;
    }
    d->dpb[2]=dpb; d->bound=true;
    e=dos_file_lifecycle(d,DOS_FILE_PSP_CREATE,f->bx,0,personality.expanded);
    if (e) { dos_file_cancel(d); d->bound=false; goto done; }
    uint8_t pointer[4]; far_write(pointer,b.sft); write_guest(d,plus(b.lists,4),pointer,4);
    d->personality.live=true;
done:
    d->ops.release(d->ops.arg); return e;
}
int dos_file_hook_event(struct dos_file *d,struct dos_file_frame *f,uint16_t caller_cs) {
    int e=BAD_POINTER; uint8_t args[DOS_HOOK_EVENT_BYTES]; struct dos_file_far at={f->si,f->ds};
    if (!d->bound || !d->personality.live || caller_cs!=d->personality.code_seg || !check(d,at,sizeof(args),false)) goto done;
    read_guest(d,at,args,sizeof(args));
    if (fs_rd16(args)!=1 || fs_rd16(args+2)!=sizeof(args) || fs_rd16(args+6)) goto done;
    unsigned kind=fs_rd16(args+4),event=f->ax; struct dos_file_far expanded={0,0};
    if (event<1 || event>8 || (kind && !(event==DOS_FILE_PSP_CREATE && (kind==0x26 || kind==0x55)))) goto done;
    if (event==DOS_FILE_EXEC_BEGIN && f->cx!=d->owning) goto done;
    /* AH55 can replace a PSP previously copied by AH26 at the same address. */
    struct dos_file_process *prior=process(d,f->bx);
    if (event==DOS_FILE_PSP_CREATE && kind==0x55 && prior && prior->copy_psp) {
        if (!check(d,(struct dos_file_far){0x18,f->bx},0x22,true)) goto done;
        e=release_process(d,prior); if (e) goto done;
    }
    if (event==DOS_FILE_PSP_CREATE || event==DOS_FILE_EXEC_BEGIN) {
        unsigned slot=0; for (;slot<DOS_FILE_PSPS;slot++) if (!d->process[slot].used) break;
        if (slot==DOS_FILE_PSPS) { e=-FS_EMFILE; goto done; }
        expanded=plus(d->personality.expanded,slot*DOS_FILE_JFT);
    }
    e=dos_file_lifecycle(d,(enum dos_file_event)event,f->bx,f->cx,expanded);
    if (!e && event==DOS_FILE_PSP_CREATE && kind==0x26) { process(d,f->bx)->copy_psp=true; uint8_t zero[2]={0}; write_guest(d,(struct dos_file_far){0x16,f->bx},zero,2); }
    if (!e && event==DOS_FILE_PSP_CREATE && kind==0x55) e=dos_file_lifecycle(d,DOS_FILE_PSP_SELECT,f->bx,0,expanded);
done:
    d->ops.release(d->ops.arg); return e;
}
