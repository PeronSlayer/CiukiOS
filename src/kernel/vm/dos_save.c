/* Immutable, ordered sector transactions. Wire integers are little endian.
 * A torn or unflushed tail never replaces the last validated frontier.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dos_save.h"
static void save_digest(const struct sha256_ctx *hash, uint8_t digest[32])
{ struct sha256_ctx copy=*hash; sha256_final(&copy,digest); }
static void save_check(uint8_t *block)
{ sha256(block,480,block+480); }
static bool save_valid(const uint8_t *block)
{ uint8_t hash[32]; sha256(block,480,hash); return !memcmp(hash,block+480,32); }
static int save_io(struct dos_save *s,uint32_t offset,void *data,unsigned bytes,bool write)
{
    int e=write ? s->io.write(s->io.arg,offset,data,bytes) : s->io.read(s->io.arg,offset,data,bytes);
    if (e && write) { s->failed=true; s->readonly=true; }
    return e;
}
static uint32_t *save_slot(struct dos_save *s,uint32_t lba,bool create)
{
    if (lba>=DOS_SAVE_SECTORS) return 0;
    unsigned page=lba/1024u;
    if (!s->index[page] && create) {
        s->index[page]=s->alloc ? s->alloc(s->memory) : fs_page_alloc();
        if (s->index[page]) { memset(s->index[page],0,4096); s->index_pages++; }
    }
    return s->index[page] ? s->index[page]+lba%1024u : 0;
}
void dos_save_release(struct dos_save *s)
{
    for (unsigned i=0;i<DOS_SAVE_SECTORS/1024u;i++) if (s->index[i]) {
        if (s->release) s->release(s->memory,s->index[i]); else fs_page_free(s->index[i]);
        s->index[i]=0; s->index_pages--; fs_service();
    }
}
int dos_save_commit(struct dos_save *s,bool seal)
{
    if (s->failed) return -FS_EIO;
    if (!s->pending && s->committed && (!seal || s->sealed)) return 0;
    if (s->end>s->quota-512u) return -FS_ENOSPC;
    /* Both barriers matter: a valid marker cannot precede its data. */
    int e=s->io.flush(s->io.arg);
    uint8_t block[512]={0}; memcpy(block,"CSCOM01",8);
    fs_wr32(block+8,s->end); fs_wr32(block+12,s->committed);
    fs_wr32(block+16,seal ? 1u : 0u); save_digest(&s->hash,block+32); save_check(block);
    if (!e) e=save_io(s,s->end,block,sizeof(block),true);
    if (!e) e=s->io.flush(s->io.arg);
    if (e) { s->failed=true; s->readonly=true; return e; }
    sha256_update(&s->hash,block,sizeof(block));
    s->end+=512; s->committed=s->end; s->pending=0; s->sealed=seal;
    return 0;
}
int dos_save_open(struct dos_save *s,const struct dos_save_io *io,
                  const struct dos_save_identity *id,uint32_t size)
{
    for (unsigned i=0;i<DOS_SAVE_SECTORS/1024u;i++) if (s->index[i]) return -FS_EBUSY;
    s->io=*io; s->end=s->committed=s->rows=s->pending=s->recovered_bytes=0;
    s->index_pages=0;
    s->readonly=s->failed=s->sealed=s->full=false;
    if (!s->quota) s->quota=DOS_SAVE_QUOTA;
    if (s->quota<2048 || s->quota>DOS_SAVE_QUOTA || !id->game || id->kernel>2) return -FS_EINVAL;
    uint8_t header[512]={0},block[512],data[512];
    memcpy(header,"CSAVE01",8); fs_wr32(header+8,1); fs_wr32(header+12,id->game);
    fs_wr32(header+16,id->kernel); fs_wr32(header+20,DOS_SAVE_SECTORS);
    memcpy(header+24,"DFV2-FAT16-1",12); memcpy(header+64,id->base,65); save_check(header);
    sha256_init(&s->hash);
    if (!size) {
        int e=save_io(s,0,header,512,true); if (e) return e;
        sha256_update(&s->hash,header,512); s->end=512;
        return dos_save_commit(s,true);
    }
    if (size<1024 || size>s->quota) return DOS_SAVE_MISMATCH;
    int e=save_io(s,0,block,512,false); if (e) return e;
    if (memcmp(header,block,512)) return DOS_SAVE_MISMATCH;
    sha256_update(&s->hash,header,512);
    struct sha256_ctx committed_hash=s->hash;
    uint32_t at=512;
    /* Validate the complete chain first. No tentative index is published. */
    while (at<=size-512u) {
        e=save_io(s,at,block,512,false); if (e) return e;
        if (!save_valid(block) || fs_rd32(block+8)!=at) break;
        if (!memcmp(block,"CSDAT01",8)) {
            if (size-at<1024 || fs_rd32(block+12)>=DOS_SAVE_SECTORS) break;
            e=save_io(s,at+512,data,512,false); if (e) return e;
            uint8_t digest[32]; sha256(data,512,digest);
            if (memcmp(digest,block+32,32)) break;
            sha256_update(&s->hash,block,512); sha256_update(&s->hash,data,512); at+=1024;
        } else if (!memcmp(block,"CSCOM01",8)) {
            uint8_t digest[32]; save_digest(&s->hash,digest);
            if (fs_rd32(block+12)!=s->committed || fs_rd32(block+16)>1 || memcmp(digest,block+32,32)) break;
            sha256_update(&s->hash,block,512); at+=512;
            s->committed=at; committed_hash=s->hash; s->sealed=fs_rd32(block+16)!=0;
        } else break;
        fs_service();
    }
    if (!s->committed) return DOS_SAVE_MISMATCH;
    s->hash=committed_hash; s->end=s->committed; s->recovered_bytes=size-s->committed;
    /* Replay only records covered by a validated commit. */
    for (at=512;at<s->committed;) {
        e=save_io(s,at,block,512,false); if (e) goto fail;
        if (!memcmp(block,"CSDAT01",8)) {
            uint32_t *slot=save_slot(s,fs_rd32(block+12),true);
            if (!slot) { e=-FS_ENOMEM; goto fail; }
            if (!*slot) s->rows++;
            *slot=at+512; at+=1024;
        } else at+=512;
        fs_service();
    }
    /* Resize only after validation/replay succeeded. Keep the committed prefix. */
    if (size!=s->committed) { e=s->io.resize(s->io.arg,s->committed); if (e) goto fail; }
    return 0;
fail:
    dos_save_release(s); return e;
}
int dos_save_read(struct dos_save *s,uint32_t lba,void *data)
{
    if (lba>=DOS_SAVE_SECTORS) return -FS_EINVAL;
    uint32_t *slot=save_slot(s,lba,false);
    return slot && *slot ? save_io(s,*slot,data,512,false) : -FS_ENOENT;
}
int dos_save_write(struct dos_save *s,uint32_t lba,const void *data)
{
    if (lba>=DOS_SAVE_SECTORS) return -FS_EINVAL;
    if (s->readonly) return -FS_EROFS;
    /* Reserve a data commit and the orderly-close seal. Quota rejection
     * issues no data write and can never prevent sealing a full session. */
    if (s->end>s->quota-2048u) { s->readonly=s->full=true; return -FS_ENOSPC; }
    uint32_t *slot=save_slot(s,lba,true); if (!slot) return -FS_ENOMEM;
    uint8_t block[512]={0}; memcpy(block,"CSDAT01",8);
    fs_wr32(block+8,s->end); fs_wr32(block+12,lba); sha256(data,512,block+32); save_check(block);
    int e=save_io(s,s->end,block,512,true);
    if (!e) e=save_io(s,s->end+512,(void *)data,512,true);
    if (e) { if (e==-FS_ENOSPC) s->full=true; return e; }
    sha256_update(&s->hash,block,512); sha256_update(&s->hash,data,512);
    if (!*slot) s->rows++;
    *slot=s->end+512; s->end+=1024; s->pending++; s->sealed=false;
    return 0;
}
