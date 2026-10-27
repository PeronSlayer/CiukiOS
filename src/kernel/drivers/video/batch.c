/* Full preflight before pixels, and ordered prefix completion.
 * EXA exa.h: clean preparation refusal, synchronization before CPU access.
 * Haiku Accelerant.h: engine ownership survives wait slices.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/display_batch.h>
static bool zero(const void *p, unsigned bytes)
{ const uint8_t *s=p; while (bytes--) if (*s++) return false; return true; }
int display_batch_header(const void *blob, uint32_t bytes, struct ciuki_batch_header *h)
{
    if (bytes>CIUKI_BATCH_MAX_BYTES) return -E2BIG;
    if (!blob || bytes<64 || (bytes&7)) return -EINVAL;
    memcpy(h,blob,64);
    if (h->version!=1) return -EOPNOTSUPP;
    if (h->record_count>64 || h->target_count>128) return -E2BIG;
    if (h->magic!=CIUKI_BATCH_MAGIC || h->header_bytes!=64 || h->total_bytes!=bytes || !h->record_count ||
        h->records_offset!=64 || h->targets_offset!=64+128*h->record_count ||
        h->payload_offset!=h->targets_offset+32*h->target_count ||
        (uint64_t)h->payload_offset+h->payload_bytes!=bytes || h->flags || !zero(h->reserved,sizeof(h->reserved))) return -EINVAL;
    return 0;
}
static bool range(const struct ciuki_batch_header *h, uint32_t start, uint32_t bytes)
{ return bytes ? !(start&7) && start>=h->payload_offset && (uint64_t)start+bytes<=h->total_bytes : start==0; }
void display_batch_command(const struct display_batch *b, unsigned index, struct swdib_command *c)
{
    const struct ciuki_batch_record *r=(const void *)(b->blob+64+index*128);
    *c=(struct swdib_command){r,r->dst ? &b->targets[r->dst-1] : 0,r->src ? &b->targets[r->src-1] : 0,
        r->clip_count ? (const void *)(b->blob+r->clip_offset) : 0,r->data_bytes ? b->blob+r->data_offset : 0};
}
int display_batch_validate(struct display_batch *b)
{
    const struct ciuki_batch_header *h=&b->header;
    uint64_t samples=0;
    uint32_t ranges[128][2]; unsigned range_count=0;
    for (unsigned i=0;i<h->target_count;i++) {
        const struct ciuki_batch_target *t=(const void *)(b->blob+h->targets_offset+i*32);
        if (t->kind<1 || t->kind>3 || t->flags ||
            !zero(t->reserved+(t->kind==CIUKI_TARGET_VIDEO ? 1 : 0),t->kind==CIUKI_TARGET_VIDEO ? 12 : 16)) return -EINVAL;
        if (t->kind==CIUKI_TARGET_VIDEO && t->reserved[0]!=b->targets[i].cache_epoch) return -EAGAIN;
        if (t->kind==1 && (t->handle || t->generation!=h->generation)) return -EINVAL;
        if (t->generation!=b->targets[i].device.generation) return -EAGAIN;
        int e=swdib_target_valid(&b->targets[i]); if (e) return e;
    }
    for (unsigned i=0;i<h->record_count;i++) {
        const struct ciuki_batch_record *r=(const void *)(b->blob+64+i*128);
        if (r->dst>h->target_count || r->src>h->target_count) return -EINVAL;
        if (r->clip_count>16) return -E2BIG;
        if ((uint64_t)r->clip_offset+r->clip_count*16>UINT32_MAX ||
            (uint64_t)r->data_offset+r->data_bytes>UINT32_MAX) return -EOVERFLOW;
        if (!range(h,r->clip_offset,r->clip_count*16) || !range(h,r->data_offset,r->data_bytes)) return -EINVAL;
        struct swdib_command c; display_batch_command(b,i,&c);
        int e=swdib_validate(&c); if (e) return e;
        b->noop[i]=r->opcode<=CIUKI_CMD_LINE && !swdib_footprint(&c);
        if (r->data_bytes) { ranges[range_count][0]=r->data_offset; ranges[range_count++][1]=r->data_offset+r->data_bytes; }
        if (r->clip_count) { ranges[range_count][0]=r->clip_offset; ranges[range_count++][1]=r->clip_offset+r->clip_count*16; }
        if (r->opcode==CIUKI_CMD_LINE) {
            int64_t x=(int64_t)r->sx-r->dx, y=(int64_t)r->sy-r->dy;
            x=x<0 ? -x : x; y=y<0 ? -y : y; samples+=x>y ? x : y;
        } else if (r->opcode!=CIUKI_CMD_SYNC && r->opcode<CIUKI_CMD_CURSOR_MOVE) samples+=(uint64_t)r->dw*r->dh;
        if (samples>CIUKI_BATCH_MAX_SAMPLES) return -E2BIG;
        if (b->preflight_pause) b->preflight_pause();
    }
    /* Normalize read-only ranges once: padding validation is linear in blob
     * bytes, rather than rescanning 64 records for every uploaded byte. */
    for (unsigned i=1;i<range_count;i++) {
        uint32_t start=ranges[i][0], end=ranges[i][1]; unsigned j=i;
        while (j && ranges[j-1][0]>start) { ranges[j][0]=ranges[j-1][0]; ranges[j][1]=ranges[j-1][1]; j--; }
        ranges[j][0]=start; ranges[j][1]=end;
    }
    uint32_t cursor=h->payload_offset;
    for (unsigned i=0;i<=range_count;i++) {
        uint32_t start=i<range_count ? ranges[i][0] : h->total_bytes;
        while (cursor<start) {
            unsigned n=start-cursor; if (n>4096) n=4096;
            if (!zero(b->blob+cursor,n)) return -EINVAL;
            cursor+=n;
            if (b->preflight_pause) b->preflight_pause();
        }
        if (i<range_count && cursor<ranges[i][1]) cursor=ranges[i][1];
    }
    return 0;
}
static bool terminal(struct display_batch *b, const struct display_batch_ops *ops)
{
    if (!ops->recover(ops->ctx)) { b->reply.state=CIUKI_FENCE_RECOVERING; b->reply.flags&=~CIUKI_FENCE_CPU_SAFE; return false; }
    b->reply.flags|=CIUKI_FENCE_CPU_SAFE;
    b->reply.state=b->reply.error ? CIUKI_FENCE_STOPPED : CIUKI_FENCE_SUCCEEDED;
    ops->end(ops->ctx);
    return true;
}
bool display_batch_recover(struct display_batch *b, const struct display_batch_ops *ops)
{
    if (b->reply.state==CIUKI_FENCE_RECOVERING) return terminal(b,ops);
    return (b->reply.state==CIUKI_FENCE_SUCCEEDED || b->reply.state==CIUKI_FENCE_STOPPED) &&
        (b->reply.flags&CIUKI_FENCE_CPU_SAFE);
}
int display_batch_execute(struct display_batch *b, const struct display_batch_ops *ops)
{
    struct ciuki_batch_reply *q=&b->reply;
    int e=ops->begin(ops->ctx,b); if (e) return e;
    q->state=CIUKI_FENCE_RUNNING; q->admitted=b->header.record_count; q->generation=b->header.generation;
    for (unsigned i=0;i<b->header.record_count;i++) {
        q->first_uncompleted=i;
        if (b->cancel || (ops->cancelled && ops->cancelled(ops->ctx))) {
            b->cancel=true; q->flags|=CIUKI_FENCE_CANCEL_REQUESTED; e=b->interrupt_error ? b->interrupt_error : -ECANCELED; break;
        }
        struct swdib_command c; display_batch_command(b,i,&c);
        unsigned op=c.record->opcode;
        bool native=op!=CIUKI_CMD_SYNC;
        if (b->noop[i]) { e=0; native=false; }
        else if (op==CIUKI_CMD_SYNC) e=ops->sync(ops->ctx);
        else if ((op==CIUKI_CMD_CURSOR_MOVE || op==CIUKI_CMD_CURSOR_SHOW) && !b->cursor_shape) { e=-EINVAL; break; }
        else {
            e=ops->native(ops->ctx,&c);
            if (e==-EOPNOTSUPP && op<CIUKI_CMD_SYNC) {
                native=false;
                /* A clean refusal is the ONLY replay path. Synchronize both
                 * endpoints (the full synchronous engine), never guess a map. */
                if (!c.dst->device.mapped || (c.src && !c.src->device.mapped)) { e=-EAGAIN; break; }
                e=ops->sync(ops->ctx);
                if (!e) swdib_execute(&c,ops->tile,ops->ctx);
            } else if (e==-EOPNOTSUPP) break;
        }
        if (e) {
            /* sync may itself fail without a submission by this record; the
             * conservative dirty flag never authorizes automatic replay. */
            q->flags|=CIUKI_FENCE_PARTIAL_DIRTY;
            e=e==-ETIMEDOUT ? e : -EIO; break;
        }
        if (op==CIUKI_CMD_CURSOR_MONO_SET || op==CIUKI_CMD_CURSOR_ARGB_SET) b->cursor_shape=true;
        if ((e=ops->sync(ops->ctx))) { q->flags|=CIUKI_FENCE_PARTIAL_DIRTY; e=e==-ETIMEDOUT ? e : -EIO; break; }
        q->completed=i+1;
        if (ops->count) ops->count(ops->ctx,op,native);
        if (ops->tile) ops->tile(ops->ctx);
    }
    if (b->cancel) q->flags|=CIUKI_FENCE_CANCEL_REQUESTED;
    q->error=e;
    q->first_uncompleted=e ? q->completed : UINT32_MAX;
    terminal(b,ops);
    return e;
}
