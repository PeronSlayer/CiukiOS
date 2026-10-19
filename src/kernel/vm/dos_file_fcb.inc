/* FCB services over the owned descriptions. Included after handle helpers.
 * The reserved guest bytes never grant authority. See f3-50 source register. */
static bool dos_char_in(const char *set,uint8_t c) {
    while (*set) if ((uint8_t)*set++==c) return true;
    return false;
}
static uint8_t dos_upper(uint8_t c) {
    int n=path_cp437(path_upper(path_from_cp437(c))); return n<0 ? c : (uint8_t)n;
}
static bool fcb_function(unsigned ah) {
    return (ah>=0x0f && ah<=0x17) || (ah>=0x21 && ah<=0x24) || ah==0x27 || ah==0x28;
}
static int fcb_path(struct dos_file *d,const uint8_t *b,char path[FS_PATH_BYTES]) {
    unsigned drive=b[0] ? b[0]-1u : d->drive,at=2;
    if (drive>=26 || !d->view[drive].table) return BAD_DRIVE;
    path[0]=(char)('A'+drive); path[1]=':';
    for (unsigned field=0;field<2;field++) {
        unsigned start=field ? 9 : 1,end=field ? 12 : 9;
        while (end>start && b[end-1]==' ') end--;
        if (field && end>start) path[at++]='.';
        for (unsigned i=start;i<end;i++) {
            uint16_t ch=path_from_cp437(b[i]); char out[4];
            if (b[i]<' ' || dos_char_in("/\\:\"<>|",b[i])) return -FS_EINVAL;
            if (path_from_ucs2(&ch,1,out,sizeof(out))) return -FS_EILSEQ;
            size_t n=strlen(out); memcpy(path+at,out,n); at+=(unsigned)n;
        }
    }
    path[at]=0; return at==2 ? -FS_ENOENT : 0;
}
static struct dos_file_fcb *fcb_mapping(struct dos_file *d,uint16_t psp,uint32_t identity,bool allocate) {
    struct dos_file_fcb *slot=0;
    for (unsigned i=0;i<DOS_FILE_FCBS;i++) {
        struct dos_file_fcb *m=&d->fcb[i];
        if (m->used && m->psp==psp && m->identity==identity) return m;
        if (!m->used && !slot) slot=m;
    }
    if (allocate && slot) { memset(slot,0,sizeof(*slot)); slot->used=true; slot->closed=true;
        slot->psp=psp; slot->identity=identity; slot->index=255; return slot; }
    return 0;
}
static int fcb_release(struct dos_file *d,struct dos_file_fcb *m) {
    if (m->closed) return 0;
    struct dos_file_description *s=&d->sft[m->index]; int e=0;
    if (!s->refs) return -FS_EBADF;
    if (s->kind==DOS_FILE_REGULAR && s->refs==1) {
        e=vfs_close(s->table,s->handle);
        if (e && s->table->handles[s->handle]) return e;
    }
    if (!--s->refs) memset(s,0,sizeof(*s)); m->closed=true; return e;
}
static void fcb_seq(uint8_t *b,uint32_t record) {
    fs_wr16(b+12,(uint16_t)(record/128)); b[32]=(uint8_t)(record&127);
}
static uint32_t fcb_random(const uint8_t *b) {
    uint32_t n=fs_rd32(b+33); return fs_rd16(b+14)<64 ? n : n&0xffffffu;
}
static void fcb_set_random(uint8_t *b,uint32_t n) {
    if (fs_rd16(b+14)<64) fs_wr32(b+33,n);
    else { b[33]=(uint8_t)n; b[34]=(uint8_t)(n>>8); b[35]=(uint8_t)(n>>16); }
}
static int fcb_find(struct dos_file *d,struct dos_file_process *p,struct dos_file_fcb *m,
                    uint8_t *b,uint8_t attr,bool extended,bool first,struct fat_entry *entry) {
    struct dos_file_cursor *c=0; int e;
    for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) if (d->cursor[i].used && d->cursor[i].id==m->search_id && d->cursor[i].psp==p->psp) c=&d->cursor[i];
    if (first) {
        if (c) c->used=false;
        c=0; for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) if (!d->cursor[i].used) { c=&d->cursor[i]; break; }
        if (!c) return -FS_EMFILE;
        char path[FS_PATH_BYTES],native[FS_PATH_BYTES]; unsigned drive; struct vfs_table *t;
        if ((e=fcb_path(d,b,path)) || (e=route(d,path,&t,native,&drive,false))) return e;
        struct vfs_find find;
        e=vfs_find_first(t,native,(uint8_t)(attr|VFS_FIND_DOS),&find,entry);
        if (e) return e;
        *c=(struct dos_file_cursor){.used=true,.id=++d->search_sequence,.psp=p->psp,.drive=(uint8_t)drive,.find=find,.table=t};
        m->search_id=c->id;
    } else {
        if (!c) return NO_MORE;
        c->table->request=&d->request; c->table->owner=owner(d,p->psp); d->active_drive=c->drive;
        e=vfs_find_next(c->table,&c->find,entry); if (e) { c->used=false; return e; }
    }
    if (!entry) return 0;
    uint8_t out[44]={0}; unsigned prefix=extended ? 7 : 0;
    if (extended) memcpy(out,b-7,7);
    uint8_t *f=out+prefix; f[0]=(uint8_t)(c->drive+1); short_fcb(entry->alias[0] ? entry->alias : entry->name,f+1);
    /* Find returns drive + the 32-byte DOS directory image, not an opened FCB. */
    f[12]=entry->attr; fs_wr16(f+23,entry->times.write_time); fs_wr16(f+25,entry->times.write_date);
    fs_wr16(f+27,(uint16_t)entry->first); fs_wr32(f+29,entry->size);
    write_guest(d,p->dta,out,33+prefix); return 0;
}
static __attribute__((noinline)) int fcb_mutate(struct dos_file *d,struct dos_file_process *p,uint8_t *b,uint8_t attr,bool rename) {
    char path[FS_PATH_BYTES],native[FS_PATH_BYTES]; struct vfs_table *t; unsigned drive; int e;
    if ((e=fcb_path(d,b,path)) || (e=route(d,path,&t,native,&drive,true))) return e;
    struct vfs_find find; struct fat_entry entry;
    e=vfs_find_first(t,native,(uint8_t)(attr|VFS_FIND_DOS),&find,&entry); if (e) return e;
    do {
        uint8_t name[37]={0}; name[0]=(uint8_t)(drive+1);
        short_fcb(entry.alias[0] ? entry.alias : entry.name,name+1);
        if ((e=fcb_path(d,name,path)) || (e=route(d,path,&t,native,&drive,true))) return e;
        if (rename) {
            for (unsigned i=0;i<11;i++) if (b[17+i]!='?') name[1+i]=b[17+i];
            char dest[FS_PATH_BYTES];
            if ((e=fcb_path(d,name,path))) return e;
            struct vfs_table *other; unsigned gd;
            if ((e=route(d,path,&other,dest,&gd,true))) return e;
            e=vfs_rename(t,native,dest);
        } else e=vfs_delete(t,native);
        if (e) return e;
        e=vfs_find_next(t,&find,&entry);
    } while (!e);
    (void)p; return e==-FS_ENOENT ? 0 : e;
}
static __attribute__((noinline)) int fcb_service(struct dos_file *d,struct dos_file_process *p,struct dos_file_frame *f) {
    unsigned ah=f->ax>>8,status=0; int e=0; uint8_t bytes[44]={0},*b=bytes; bool loaded=false;
    struct dos_file_far pointer={f->dx,f->ds}; unsigned prefix=0;
    if (!check(d,pointer,1,false)) { e=BAD_POINTER; goto fail; }
    read_guest(d,pointer,b,1); prefix=b[0]==255 ? 7 : 0;
    if (!check(d,pointer,37+prefix,true)) { e=BAD_POINTER; goto fail; }
    read_guest(d,pointer,bytes,37+prefix); b=bytes+prefix; loaded=true;
    uint32_t identity=address(d,pointer);
    struct dos_file_fcb *m=fcb_mapping(d,p->psp,identity,false);
    uint8_t attr=prefix ? bytes[6] : 0;
    if (ah==0x0f || ah==0x16) {
        char path[FS_PATH_BYTES]; unsigned outcome;
        if ((e=fcb_path(d,b,path))) goto fail;
        if (m && !m->closed) { e=-FS_EEXIST; goto fail; }
        if (!m) m=fcb_mapping(d,p->psp,identity,true);
        if (!m) { e=-FS_EMFILE; goto fail; }
        int h=open_file(d,p,path,2,ah==0x16 ? VFS_CREATE|VFS_TRUNCATE : 0,attr,&outcome);
        if (h<0) { e=h; goto fail; }
        struct dos_file_description *s=get_handle(d,p,(unsigned)h); struct fat_entry entry={0};
        if (s->kind==DOS_FILE_REGULAR) e=vfs_handle_stat(s->table,s->handle,&entry,0);
        if (e) { (void)release_slot(d,p,(unsigned)h); goto fail; }
        m->index=p->handles[h]; p->handles[h]=255; m->closed=false; m->generation=s->generation;
        b[0]=(uint8_t)(s->drive+1); fs_wr16(b+12,0); fs_wr16(b+14,s->kind==DOS_FILE_REGULAR ? 128 : 0);
        fs_wr32(b+16,entry.size); fs_wr16(b+20,entry.times.write_date); fs_wr16(b+22,entry.times.write_time);
        memcpy(m->name,b,12); goto done;
    }
    if (ah==0x11 || ah==0x12) {
        if (!check(d,p->dta,37+prefix,true)) { e=BAD_POINTER; goto fail; }
        if (!m) m=fcb_mapping(d,p->psp,identity,true);
        if (!m) { e=-FS_EMFILE; goto fail; }
        struct fat_entry entry;
        e=fcb_find(d,p,m,b,attr,prefix!=0,ah==0x11,&entry); if (e) goto fail; goto done;
    }
    if (ah==0x13 || ah==0x17) { e=fcb_mutate(d,p,b,prefix ? attr : 0x27,ah==0x17); if (e) goto fail; goto done; }
    unsigned size=fs_rd16(b+14);
    uint32_t sequential=(uint32_t)fs_rd16(b+12)*128+b[32];
    if (ah==0x24) { fcb_set_random(b,sequential); goto done; }
    if (ah==0x23) {
        char path[FS_PATH_BYTES],native[FS_PATH_BYTES]; struct vfs_table *t; unsigned drive; struct fat_entry entry;
        if (!size) { e=-FS_EINVAL; goto fail; }
        if ((e=fcb_path(d,b,path)) || (e=route(d,path,&t,native,&drive,false)) || (e=vfs_stat(t,native,&entry))) goto fail;
        fcb_set_random(b,(uint32_t)(((uint64_t)entry.size+size-1)/size)); goto done;
    }
    if (m && m->closed && ah==0x10) goto done;
    if (!m || m->closed || memcmp(m->name,b,12)) { e=-FS_EBADF; goto fail; }
    struct dos_file_description *s=&d->sft[m->index];
    if (!s->refs || s->generation!=m->generation || (e=handle_live(s))) { if (!e) e=-FS_EBADF; goto fail; }
    if (s->table) { s->table->owner=owner(d,p->psp); s->table->request=&d->request; }
    if (ah==0x10) {
        if (s->kind==DOS_FILE_REGULAR) e=vfs_handle_times(s->table,s->handle,fs_rd16(b+20),fs_rd16(b+22));
        if (!e) e=fcb_release(d,m); if (e) goto fail; goto done;
    }
    bool block=ah==0x27 || ah==0x28,random=block || ah==0x21 || ah==0x22;
    bool writing=ah==0x15 || ah==0x22 || ah==0x28;
    unsigned count=block ? f->cx : 1; uint32_t record=random ? fcb_random(b) : sequential;
    if (block) f->cx=0;
    if (!size || b[32]>=128 || s->kind!=DOS_FILE_REGULAR) { e=-FS_EINVAL; goto io_fail; }
    uint64_t offset=(uint64_t)record*size;
    if (offset>UINT32_MAX || (uint64_t)count*size>UINT32_MAX-offset) { e=-FS_EFBIG; goto io_fail; }
    uint32_t total=(uint32_t)count*size;
    if (total>65535 || total>65536u-p->dta.off) { status=2; e=BAD_POINTER; goto io_fail; }
    if (total && !check(d,p->dta,total,!writing)) { status=2; e=BAD_POINTER; goto io_fail; }
    if (writing && d->view[s->drive].readonly) { e=-FS_EACCES; goto io_fail; }
    d->active_drive=s->drive;
    uint64_t position; e=vfs_seek(s->table,s->handle,(int64_t)offset,VFS_SEEK_SET,&position); if (e) goto io_fail;
    if (writing && !count) e=vfs_truncate(s->table,s->handle,offset);
    uint32_t transferred=0;
    while (!e && transferred<total) {
        size_t n=total-transferred,done=0; if (n>sizeof(d->transfer)) n=sizeof(d->transfer);
        if (writing) { read_guest(d,plus(p->dta,transferred),d->transfer,n); e=vfs_write(s->table,s->handle,d->transfer,n,&done); }
        else { e=vfs_read(s->table,s->handle,d->transfer,n,&done); if (done) write_guest(d,plus(p->dta,transferred),d->transfer,done); }
        transferred+=(uint32_t)done; if (done<n) break;
    }
    d->transferred=transferred;
    unsigned actual=writing ? transferred/size : (transferred+size-1)/size;
    if (block) f->cx=(uint16_t)actual;
    if (writing) {
        struct fat_entry entry; int stat=vfs_handle_stat(s->table,s->handle,&entry,0);
        if (!stat) fs_wr32(b+16,entry.size); else if (!e) e=stat;
        if (transferred) s->written=true;
    }
    if (!writing && transferred%size) {
        unsigned padding=size-transferred%size; memset(d->transfer,0,sizeof(d->transfer));
        while (padding) { unsigned n=padding>sizeof(d->transfer) ? sizeof(d->transfer) : padding;
            write_guest(d,plus(p->dta,transferred),d->transfer,n); transferred+=n; padding-=n; }
        status=3;
    } else if (transferred<total) status=1;
    if (random) {
        if (block) { fcb_set_random(b,record+actual); fcb_seq(b,record+actual); }
        else fcb_seq(b,record);
    } else if (actual) fcb_seq(b,sequential+actual);
    if (e) goto io_fail;
    if (status) p->error=(struct dos_file_error){writing ? 39 : 38,writing ? 1 : 8,4,2};
    goto done;
io_fail:
    if (!status) status=1;
    dos_file_error_from_fs(&p->error,e,writing); goto done;
fail:
    status=255; dos_file_error_from_fs(&p->error,e,false);
done:
    if (loaded) write_guest(d,pointer,bytes,37+prefix);
    f->ax=(uint16_t)((f->ax&0xff00)|status); return 0;
}
static bool fcb_separator(uint8_t c) { return c<=' ' || dos_char_in("/\\\"[]<>|.:;,=+",c)!=0; }
static int fcb_parse(struct dos_file *d,struct dos_file_process *p,struct dos_file_frame *f) {
    struct dos_file_far out={f->di,f->es}; uint8_t b[37],input[256]; unsigned n=0,at=0,status=0,options=f->ax&255;
    if (!check(d,out,37,true)) return BAD_POINTER;
    read_guest(d,out,b,37);
    /* Parse at most one component; no mutation until all consumed bytes are checked. */
    while (n<sizeof(input)) {
        struct dos_file_far in={0,f->ds}; if (n>65535u-f->si) return BAD_POINTER;
        in.off=(uint16_t)(f->si+n); if (!check(d,in,1,false)) return BAD_POINTER;
        read_guest(d,in,input+n,1); if (!input[n++]) break;
    }
    if (n==sizeof(input) && input[n-1]) return BAD_POINTER;
    if (options&1) while (input[at] && dos_char_in(":;,=+ \t",input[at])) at++;
    while (input[at]==' ' || input[at]=='\t') at++;
    if (!fcb_separator(input[at]) && input[at+1]==':') {
        unsigned drive=dos_upper(input[at])-'A'; b[0]=(uint8_t)(drive+1); at+=2;
        if (drive>=26 || !d->view[drive].table) status=255;
    } else if (!(options&2)) b[0]=0;
    fs_wr16(b+12,0); fs_wr16(b+14,0);
    if (!(options&4)) memset(b+1,' ',8); if (!(options&8)) memset(b+9,' ',3);
    if (input[at]=='.') {
        b[1]='.'; at++;
        if (input[at]=='.') { b[2]='.'; at++; }
        goto parsed;
    }
    for (unsigned field=0;field<2;field++) {
        unsigned width=field ? 3 : 8,start=field ? 9 : 1,used=0;
        if (field) { if (input[at]!='.') break; at++; }
        if (!fcb_separator(input[at])) {
            memset(b+start,' ',width);
            while (used<width && !fcb_separator(input[at])) {
                uint8_t c=input[at++]; if (c=='*') { memset(b+start+used,'?',width-used); if (!status) status=1; break; }
                b[start+used++]=dos_upper(c); if (c=='?' && !status) status=1;
            }
        }
    }
parsed:
    if (status==255) p->error=(struct dos_file_error){15,8,3,2};
    write_guest(d,out,b,37); f->si=(uint16_t)(f->si+at); f->ax=(uint16_t)((f->ax&0xff00)|status); return 0;
}
