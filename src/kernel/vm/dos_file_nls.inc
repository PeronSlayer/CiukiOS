/* Country 1, CP437. Shared Unicode/alias mapping is in fs/path.c. */
static int nls_service(struct dos_file *d,struct dos_file_process *p,struct dos_file_frame *f) {
    (void)p; unsigned ah=f->ax>>8,al=f->ax&255;
    if (ah==0x66) {
        if (al==1) { f->bx=f->dx=437; return 0; }
        if (al==2 && f->bx==437 && f->dx==437) return 0;
        return -FS_EINVAL;
    }
    if (ah==0x38) {
        if ((al && al!=1 && !(al==255 && f->bx==1)) || f->dx==65535) return -FS_EINVAL;
        uint8_t country[34];
        if (!check(d,d->personality.country,34,false) || !check(d,(struct dos_file_far){f->dx,f->ds},34,true)) return BAD_POINTER;
        read_guest(d,d->personality.country,country,34); write_guest(d,(struct dos_file_far){f->dx,f->ds},country,34);
        f->ax=1; f->bx=1; return 0;
    }
    if (al>=1 && al<=7) {
        if ((f->bx!=437 && f->bx!=65535) || (f->dx!=1 && f->dx!=65535)) return -FS_EINVAL;
        unsigned size=al==1 ? 41 : 5;
        if (f->cx<5) return -FS_EINVAL;
        if (size>f->cx) size=f->cx;
        struct dos_file_far dest={f->di,f->es}; if (!check(d,dest,size,true)) return BAD_POINTER;
        uint8_t result[41]={0}; result[0]=(uint8_t)al;
        if (al==1) {
            if (!check(d,d->personality.country,34,false)) return BAD_POINTER;
            fs_wr16(result+1,38); fs_wr16(result+3,1); fs_wr16(result+5,437);
            read_guest(d,d->personality.country,result+7,34);
        } else {
            struct dos_file_far tables[]={d->personality.upper,d->personality.lower,d->personality.filename,
                d->personality.terminator,d->personality.collating,d->personality.dbcs};
            struct dos_file_far table=tables[al-2]; if (!check(d,table,2,false)) return BAD_POINTER;
            far_write(result+1,table);
        }
        write_guest(d,dest,result,size); f->cx=(uint16_t)size; return 0;
    }
    if (al==0x20) { f->dx=(uint16_t)((f->dx&0xff00)|dos_upper((uint8_t)f->dx)); return 0; }
    if (al==0x23) { uint8_t c=dos_upper((uint8_t)f->dx); f->ax=c=='Y' ? 1 : c=='N' ? 0 : 2; return 0; }
    if (al==0x21 || al==0x22) {
        struct dos_file_far data={f->dx,f->ds}; unsigned size=f->cx;
        if (al==0x22) {
            size=0;
            for (;;) {
                if (size>=65536u-data.off) return BAD_POINTER;
                uint8_t c; struct dos_file_far at=plus(data,size);
                if (!check(d,at,1,false)) return BAD_POINTER;
                read_guest(d,at,&c,1); if (!c) break; size++;
            }
        }
        if (size && !check(d,data,size,true)) return BAD_POINTER;
        for (unsigned offset=0;offset<size;) {
            unsigned n=size-offset; if (n>sizeof(d->transfer)) n=sizeof(d->transfer);
            read_guest(d,plus(data,offset),d->transfer,n);
            for (unsigned i=0;i<n;i++) d->transfer[i]=dos_upper(d->transfer[i]);
            write_guest(d,plus(data,offset),d->transfer,n); offset+=n;
        }
        return 0;
    }
    return -FS_EOPNOTSUPP;
}
static __attribute__((noinline)) int dos_internal(struct dos_file *d,struct dos_file_process *p,struct dos_file_frame *f) {
    switch (f->ax) {
    case 0x1000: f->ax=0x10ff; return 0;
    case 0x1203: f->ds=d->binding.sda.seg; return 0;
    case 0x1216:
        if (f->bx>=d->binding.sft_count) return -FS_EBADF;
        f->es=d->binding.sft.seg; f->di=(uint16_t)(d->binding.sft.off+6+59*f->bx); return 0;
    case 0x1220:
        if (f->bx>=p->count) return -FS_EBADF;
        f->es=p->jft.seg; f->di=(uint16_t)(p->jft.off+f->bx); return 0;
    case 0x122d: f->ax=p->error.code; return 0;
    case 0x1226: {
        if (!d->personality.live || f->ss!=d->personality.stack_seg ||
            f->sp<d->personality.stack_off || f->sp-d->personality.stack_off>=d->personality.stack_bytes) return -FS_EACCES;
        char path[FS_PATH_BYTES]; int e=guest_string(d,f->ds,f->dx,path); if (e) return e;
        unsigned status; e=open_file(d,p,path,f->cx&255,0,0,&status);
        if (e<0) return e; f->ax=(uint16_t)e; return 0;
    }
    default: return -FS_EOPNOTSUPP;
    }
}
