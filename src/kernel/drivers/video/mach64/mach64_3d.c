/* Mach64 setup-engine 3D backend (f7-27). Independent implementation from
 * the pinned sources cited in docs/design/f7-family-ati-mach64.md; no
 * upstream code is vendored. CPU MMIO only: no bus-master DMA, no AGP.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include "mach64_3d.h"
#include "registers3d.h"

uint32_t mach64_3d_fixed_ieee(uint32_t value,unsigned fractional)
{
    if (!value || fractional>31) return 0;
    unsigned bit=31; while (!(value&(1u<<bit))) bit--;
    uint32_t mantissa;
    if (bit<=23) mantissa=value<<(23-bit);
    else {
        unsigned shift=bit-23;
        mantissa=value>>shift;
        uint32_t rem=value&((1u<<shift)-1),half=1u<<(shift-1);
        if (rem>half || (rem==half && (mantissa&1))) mantissa++;
        if (mantissa==0x1000000u) { mantissa>>=1; bit++; }
    }
    return ((bit+127-fractional)<<23)|(mantissa&0x7fffffu);
}
static uint32_t signed_ieee(int32_t v)
{ return mach64_3d_fixed_ieee(v<0 ? (uint32_t)-(int64_t)v : (uint32_t)v,16)|(v<0 ? 0x80000000u : 0); }
/* Correctly rounded (ties to even) positive n/d, both below 2^40. */
uint32_t mach64_3d_ratio_ieee(uint64_t n,uint64_t d)
{
    if (!n || !d || n>=(1ull<<40) || d>=(1ull<<40)) return 0;
    uint32_t exponent=127;
    while (n<d) { n<<=1; exponent--; }
    while (n>=2*d) { d<<=1; exponent++; }
    uint64_t mantissa=1,remainder=n-d;
    for (unsigned i=0;i<23;i++) {
        remainder<<=1; mantissa<<=1;
        if (remainder>=d) { remainder-=d; mantissa++; }
    }
    if (remainder*2>d || (remainder*2==d && (mantissa&1))) mantissa++;
    if (mantissa==0x1000000u) { mantissa>>=1; exponent++; }
    return (exponent<<23)|(uint32_t)(mantissa&0x7fffffu);
}
/* X_Y carries two fractional bits (Mesa mach64_tris.c:434, 531-538;
 * mach64_native_vbtmp.h:47-48). Only exactly representable positions are
 * admitted, so coverage never depends on a rounding choice. */
bool mach64_3d_on_grid(const struct render3d_vertex *v)
{
    return v && v->x>=0 && v->y>=0 && v->x<=1024*65536 && v->y<=1024*65536 &&
        !(v->x&0x3fff) && !(v->y&0x3fff);
}
static bool uv_range(int32_t a) { return a>=-16*65536 && a<=16*65536; }
/* Mesa native vertex order (mach64_tris.c:92-113, 422-492): per vertex the
 * optional SECONDARY_S/T/W group, then S,T,W,SPEC_ARGB,Z,ARGB,X_Y; the
 * ONE_OVER_AREA write after vertex 3 starts the triangle. Z is 16.16 with
 * the depth value in the integer part (mach64_native_vbtmp.h:49,
 * mach64_state.c:1048-1051). S/T are the unprojected coordinates with
 * TEX_ST_MULT_W (mach64_native_vbtmp.h:238-241). */
int mach64_3d_triangle(const struct render3d_request *q,unsigned first,bool second,
                       struct m64_write out[MACH64_3D_TRIANGLE_WRITES],unsigned *count)
{
    if (!q || !out || !count || first%3 || q->count>R3D_VERTICES || q->count%3 ||
        first>=q->count || q->count-first<3) return -EINVAL;
    const struct render3d_vertex *v=q->vertices+first;
    int32_t xq[3],yq[3];
    for (unsigned i=0;i<3;i++) {
        if (!mach64_3d_on_grid(&v[i]) || v[i].z>65535 || v[i].rhw<256 || v[i].rhw>16777216 ||
            !uv_range(v[i].u) || !uv_range(v[i].v) || !uv_range(q->uv1[first+i][0]) ||
            !uv_range(q->uv1[first+i][1])) return -EINVAL;
        xq[i]=v[i].x>>14; yq[i]=v[i].y>>14;
    }
    *count=0;
    int64_t a=(int64_t)(xq[0]-xq[2])*(yq[1]-yq[2])-(int64_t)(yq[0]-yq[2])*(xq[1]-xq[2]);
    if (!a) return 0; /* no sample is covered; the scalar reference agrees */
    uint32_t ooa=mach64_3d_ratio_ieee(16,(uint64_t)(a<0 ? -a : a))|(a<0 ? 0x80000000u : 0);
    static const uint32_t secondary[3]={M64_VERTEX_1_SECONDARY,M64_VERTEX_2_SECONDARY,M64_VERTEX_3_SECONDARY};
    unsigned n=0;
    for (unsigned i=0;i<3;i++) {
        uint32_t w=mach64_3d_fixed_ieee(v[i].rhw,16),base=M64_VERTEX_1_S+i*M64_VERTEX_STRIDE;
        uint32_t colour=q->state.flat ? q->flat_color[first/3] : v[i].color;
        if (second) {
            out[n++]=(struct m64_write){secondary[i],signed_ieee(q->uv1[first+i][0])};
            out[n++]=(struct m64_write){secondary[i]+4,signed_ieee(q->uv1[first+i][1])};
            out[n++]=(struct m64_write){secondary[i]+8,w};
        }
        out[n++]=(struct m64_write){base,signed_ieee(v[i].u)};
        out[n++]=(struct m64_write){base+4,signed_ieee(v[i].v)};
        out[n++]=(struct m64_write){base+8,w};
        out[n++]=(struct m64_write){base+0xc,0};
        out[n++]=(struct m64_write){base+0x10,v[i].z<<16};
        out[n++]=(struct m64_write){base+0x14,colour};
        out[n++]=(struct m64_write){base+0x18,((uint32_t)xq[i]<<16)|(uint32_t)yq[i]};
    }
    out[n++]=(struct m64_write){M64_ONE_OVER_AREA,ooa};
    *count=n; return 0;
}

/* Linear surfaces: pitch in pixels/8 << 22, offset in bytes/8
 * (mach64_state.c:752-753, 1068-1072). Textures are tightly packed rows of
 * 2^l2w texels at one level (mach64_texmem.c:117-123, 191-230). */
uint32_t mach64_3d_pitch(const struct render3d_resource *r)
{
    if (!r) return 0;
    if (r->kind==R3D_TEXTURE) return r->width*4;
    return ((r->width+7)&~7u)*(r->kind==R3D_D16 ? 2u : 4u);
}
uint32_t mach64_3d_padded_bytes(const struct render3d_resource *r)
{ return (mach64_3d_pitch(r)*r->height+4095)&~4095u; }
static bool overlaps(struct render3d_extent a,struct render3d_extent b)
{ return (uint64_t)a.first+a.bytes>b.first && (uint64_t)b.first+b.bytes>a.first; }
static bool fullsize(const struct mach64_3d_device *d)
{ return d && (d->fullsize_qualified || d->fullsize_testing) && d->mode_width && d->mode_height; }
static bool dimensions(const struct mach64_3d_device *d,const struct render3d_resource *r)
{ return fullsize(d) ? r->width<=d->mode_width && r->height<=d->mode_height : r->width<=64 && r->height<=64; }
static int layout(void *device,struct render3d_resource *r)
{
    const struct mach64_3d_device *d=device;
    if (!d || !r || !r->width || !r->height || r->kind<R3D_COLOR || r->kind>R3D_LINEAR_COLOR ||
        (r->kind==R3D_TEXTURE ? (r->width>MACH64_3D_MAX_TEXTURE || r->height>MACH64_3D_MAX_TEXTURE) : !dimensions(d,r)))
        return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE && render3d_texture_shape(r)) return -EINVAL;
    r->reserved=mach64_3d_padded_bytes(r)+8192u;
    return 0;
}
static uint32_t pool_first(const struct mach64_3d_device *d)
{ return d->heap.first+d->heap.bytes-(d->texture_bytes ? d->texture_bytes : MACH64_3D_TEXTURE_MIN); }
static bool extent(const struct mach64_3d_device *d,const struct render3d_resource *r)
{
    return r && r->kind>=R3D_COLOR && r->kind<=R3D_LINEAR_COLOR && r->hardware && r->shadow &&
        r->width && r->height && (r->kind==R3D_TEXTURE ? !render3d_texture_shape(r) &&
        r->width<=MACH64_3D_MAX_TEXTURE && r->height<=MACH64_3D_MAX_TEXTURE : dimensions(d,r)) &&
        r->offset>=d->heap.first && !(r->offset&4095u) &&
        (uint64_t)r->offset+r->reserved<=(uint64_t)d->heap.first+d->heap.bytes &&
        (r->kind==R3D_TEXTURE ? r->offset>=pool_first(d) : (uint64_t)r->offset+r->reserved<=pool_first(d)) &&
        r->reserved==mach64_3d_padded_bytes(r)+8192u && r->pitch==r->width*(r->kind==R3D_D16 ? 2u : 4u) &&
        (r->kind==R3D_TEXTURE || r->bytes==r->pitch*r->height) &&
        /* Surface offsets are 8-byte units below 8 MiB (20-bit field). */
        (uint64_t)r->offset+r->reserved<=0x800000u;
}
static bool alpha_format(uint32_t f)
{ return f==R3D_ARGB8888 || f==R3D_RGBA8888 || f==R3D_ARGB4444 || f==R3D_A8; }
/* Mesa mach64_texstate.c:111-284 and mach64_tex.c:70-94: stage 0 REPLACE,
 * MODULATE or ALPHA_DECAL; the composite stage only MODULATE; one level. */
static int sampler_admission(const struct mach64_3d_device *d,const struct render3d_request *q,
                             unsigned stage,const struct render3d_resource *t)
{
    const struct render3d_sampler *s=&q->state.sampler[stage];
    unsigned limit=d->perspective_qualified || d->fullsize_testing ? MACH64_3D_QUALIFIED_TEXTURE : 64u;
    if (t->width>limit || t->height>limit) return -EOPNOTSUPP;
    if (s->filter!=R3D_NEAREST && s->filter!=R3D_BILINEAR) return -EOPNOTSUPP;
    if (stage) return s->combine==R3D_MODULATE ? 0 : -EOPNOTSUPP;
    if (s->combine==R3D_ADD) return -EOPNOTSUPP;
    if (t->format==R3D_A8 && s->combine!=R3D_MODULATE) return -EOPNOTSUPP;
    /* Two units select TEX_BLEND_FCN_TRILINEAR for unit 0 (texstate.c:338). */
    if ((q->flags&R3D_TEXTURE1) && s->filter!=R3D_BILINEAR) return -EOPNOTSUPP;
    return 0;
}
/* The written alpha is ALPHA_TST_CNTL's ALPHA_DST selection, not a blended
 * value (mach64_reg.h:84-90, mach64_state.c:1077-1080; X.Org
 * atimach64render.c:71-76). Only pairs whose reference alpha is one of
 * those selections independently of the data are expressible. */
static int blend_admission(const struct mach64_3d_device *d,const struct render3d_state *s)
{
    if (!s->blend || (s->source==R3D_ONE && s->destination==R3D_ZERO)) return 0;
    if (s->source!=R3D_ZERO || (s->destination!=R3D_ONE && s->destination!=R3D_ZERO)) return -EOPNOTSUPP;
    return d->blend_qualified || d->fullsize_testing ? 0 : -EOPNOTSUPP;
}
/* ALPHA_TST_SRC_TEXEL tests the unit-0 texel alpha (mach64_state.c:1079).
 * It equals the fragment alpha only for an opaque incoming colour. */
static int alpha_admission(const struct mach64_3d_device *d,const struct render3d_request *q,
                           struct render3d_resource *const r[R3D_REFS])
{
    if (!q->state.alpha_test) return 0;
    if ((!d->alpha_qualified && !d->fullsize_testing) || !(q->flags&R3D_TEXTURED) ||
        (q->flags&R3D_TEXTURE1) || !alpha_format(r[2]->format) ||
        (q->state.sampler[0].combine!=R3D_MODULATE && q->state.sampler[0].combine!=R3D_REPLACE)) return -EOPNOTSUPP;
    for (unsigned i=0;i<q->count;i++) {
        uint32_t c=q->state.flat ? q->flat_color[i/3] : q->vertices[i].color;
        if ((c>>24)!=255) return -EOPNOTSUPP;
    }
    return 0;
}
static int perspective_admission(const struct mach64_3d_device *d,const struct render3d_request *q)
{
    if (!(q->flags&R3D_TEXTURED)) return 0;
    for (unsigned first=0;first<q->count;first+=3) {
        const struct render3d_vertex *v=q->vertices+first;
        if (v[0].rhw==v[1].rhw && v[0].rhw==v[2].rhw) continue;
        if (!d->perspective_qualified && !d->fullsize_testing) return -EOPNOTSUPP;
        uint32_t lo=UINT32_MAX,hi=0;
        for (unsigned i=0;i<3;i++) {
            if (v[i].rhw<32768 || v[i].rhw>16777216) return -EOPNOTSUPP;
            if (v[i].rhw<lo) lo=v[i].rhw;
            if (v[i].rhw>hi) hi=v[i].rhw;
        }
        if ((uint64_t)hi>4ull*lo) return -EOPNOTSUPP;
    }
    return 0;
}
/* The +-1 D16 tolerance is provable only while the scalar reference's
 * 16-bit barycentric weights move its plane by less than half a level:
 * |z0-z2|+|z1-z2| < 32768 (render3d_scalar.c fraction/interpolate). */
static int depth_admission(const struct render3d_request *q)
{
    if (!(q->flags&(R3D_DEPTH_TEST|R3D_DEPTH_WRITE))) return 0;
    for (unsigned first=0;first<q->count;first+=3) {
        const struct render3d_vertex *v=q->vertices+first;
        int32_t a=(int32_t)v[0].z-(int32_t)v[2].z,b=(int32_t)v[1].z-(int32_t)v[2].z;
        if ((a<0 ? -a : a)+(b<0 ? -b : b)>=32768) return -EOPNOTSUPP;
    }
    return 0;
}
static int validate(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct mach64_3d_device *d=device;
    if (!d || !d->engine || !d->heap.first || !d->heap.bytes ||
        d->heap.first>UINT32_MAX-d->heap.bytes || (d->heap.first&4095u) || (d->heap.bytes&4095u) ||
        (uint64_t)d->heap.first+d->heap.bytes>0x800000u || d->heap.bytes<=MACH64_3D_TEXTURE_MIN ||
        (d->texture_bytes && ((d->texture_bytes&4095u) || d->texture_bytes<MACH64_3D_TEXTURE_MIN ||
        d->texture_bytes>=d->heap.bytes)) ||
        !q || !r || !q->flags || (q->flags&~R3D_ALL_FEATURES) || render3d_validate_state(q) ||
        q->count>(fullsize(d) ? R3D_VERTICES : 3u) || q->count%3 || q->clear_depth>65535 ||
        !extent(d,r[0]) || r[0]->kind!=R3D_COLOR) return -EINVAL;
    for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) {
        if (!extent(d,r[i])) return -EINVAL;
        for (unsigned j=0;j<i;j++) if (r[j] && r[i]!=r[j] && overlaps((struct render3d_extent){r[i]->offset,r[i]->reserved},
            (struct render3d_extent){r[j]->offset,r[j]->reserved})) return -EINVAL;
    }
    if ((q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) &&
        (!r[1] || r[1]->kind!=R3D_D16 || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height)) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) && (!r[2] || r[2]->kind!=R3D_TEXTURE)) return -EINVAL;
    if ((q->flags&R3D_TEXTURE1) && (!(q->flags&R3D_TEXTURED) || !r[4] || r[4]->kind!=R3D_TEXTURE)) return -EINVAL;
    if (!(q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) && r[1]) return -EINVAL;
    if (!(q->flags&R3D_TEXTURED) && r[2]) return -EINVAL;
    if (!(q->flags&R3D_COPY) && r[3]) return -EINVAL;
    if (!(q->flags&R3D_TEXTURE1) && r[4]) return -EINVAL;
    if ((q->flags&R3D_COPY) && (!r[3] || (r[3]->kind!=R3D_LINEAR_COLOR && r[3]->kind!=R3D_COLOR) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height)) return -EINVAL;
    for (unsigned i=0;i<q->count;i++) {
        const struct render3d_vertex *v=&q->vertices[i];
        if (v->x<0 || v->y<0 || v->x>(int32_t)(r[0]->width<<16) || v->y>(int32_t)(r[0]->height<<16) ||
            v->z>65535 || v->rhw<256 || v->rhw>16777216 || !uv_range(v->u) || !uv_range(v->v) ||
            !uv_range(q->uv1[i][0]) || !uv_range(q->uv1[i][1])) return -EINVAL;
    }
    /* Everything below is a capability refusal: AUTO selects software for
     * the whole context and nothing is emitted. */
    if ((q->flags&R3D_COPY) && r[3]->kind==R3D_COLOR) return -EOPNOTSUPP;
    for (unsigned i=0;i<q->count;i++) if (!mach64_3d_on_grid(&q->vertices[i])) return -EOPNOTSUPP;
    for (unsigned stage=0;stage<2;stage++) if (q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) {
        int e=sampler_admission(d,q,stage,r[stage ? 4 : 2]); if (e) return e;
    }
    int e=blend_admission(d,&q->state); if (e) return e;
    e=alpha_admission(d,q,r); if (e) return e;
    e=perspective_admission(d,q); if (e) return e;
    e=depth_admission(q); if (e) return e;
    struct m64_write probe[MACH64_3D_TRIANGLE_WRITES]; unsigned n;
    for (unsigned first=0;first<q->count;first+=3) {
        e=mach64_3d_triangle(q,first,(q->flags&R3D_TEXTURE1)!=0,probe,&n); if (e) return e;
    }
    return 0;
}

/* ---- Register save/restore. Readable state is the set X.Org saves with
 * inm() (atimach64.c:441-522) and re-tests in TestRegisterCachingDP
 * (atimach64accel.c:293-334); ATIDRISync restores it after DRI use
 * (atimach64accel.c:171-207), writing Z_CNTL and ALPHA_TST_CNTL as zero. */
static const uint32_t saved_registers[MACH64_3D_SAVED-1]={
    M64_DST_OFF_PITCH,M64_DST_CNTL,M64_SRC_OFF_PITCH,M64_SCALE_3D_CNTL,
    M64_SC_LEFT,M64_SC_RIGHT,M64_SC_TOP,M64_SC_BOTTOM,M64_DP_FRGD_CLR,
    M64_DP_WRITE_MASK,M64_DP_PIX_WIDTH,M64_DP_MIX,M64_DP_SRC,M64_CLR_CMP_CNTL,
    M64_TEX_SIZE_PITCH,M64_TEX_CNTL
};
static uint64_t now(struct mach64_3d_device *d) { return d->engine->io.now(d->engine->io.ctx); }
static void window(struct mach64_3d_device *d) { d->budget=(struct m64_budget){now(d),0}; }
static int read_register(struct mach64_3d_device *d,uint32_t reg,uint32_t *value)
{
    int e=m64_render3d_read(d->engine,reg,value);
    if (e) { d->failed_register=reg; d->actual=*value; }
    return e;
}
static int save_state(struct mach64_3d_device *d)
{
    struct mach64_3d_saved *s=&d->saved;
    uint32_t value=0;
    s->valid=false;
    /* Dead window, FIFO error, block 1 disabled or a busy engine: refuse
     * before the first write. BUS_CNTL is never written by this backend. */
    if (read_register(d,M64_FIFO_STAT,&value) || (value&0x80000000u) || (value&0xffffu)) return -EIO;
    if (read_register(d,M64_GUI_STAT,&value) || (value&1u)) return -EIO;
    if (read_register(d,M64_BUS_CNTL,&value)) return -EIO;
    if (!(value&M64_BUS_EXT_REG_EN)) { d->failed_register=M64_BUS_CNTL; d->actual=value; return -ENODEV; }
    for (unsigned i=0;i<MACH64_3D_SAVED-1;i++) if (read_register(d,saved_registers[i],&s->value[i])) return -EIO;
    for (unsigned i=0;i<11;i++) if (read_register(d,M64_TEX_0_OFF+i*4,&s->texture[i])) return -EIO;
    s->level=(s->value[14]>>4)&15u;
    s->value[16]=s->level<=10 ? s->texture[s->level] : 0;
    s->valid=true;
    return 0;
}
static int emit(struct mach64_3d_device *d,const struct m64_write *w,unsigned n)
{
    int e=m64_render3d_write(d->engine,w,n,&d->budget);
    if (!e) d->packets++;
    return e;
}
static int restore_state(struct mach64_3d_device *d)
{
    const struct mach64_3d_saved *s=&d->saved;
    if (!s->valid) return -EIO;
    struct m64_write *w=d->writes; unsigned n=0;
    /* Lead decision D-F2 (contract B.4): the 2D driver never writes the 3D
     * state, so the write-only registers are restored by re-emitting this
     * backend's shadowed baseline: Z, alpha test and setup off (X.Org's 2D
     * values, atimach64accel.c:196-197, atimach64render.c:737) and the
     * Z/secondary-texture pointers parked on the claimed heap's first page,
     * never on scanout. Vertex registers are inputs, not 2D state
     * (atimach64render.c:89-94), and are not re-emitted. */
    w[n++]=(struct m64_write){M64_Z_CNTL,0};
    w[n++]=(struct m64_write){M64_ALPHA_TST_CNTL,0};
    w[n++]=(struct m64_write){M64_SETUP_CNTL,0};
    w[n++]=(struct m64_write){M64_Z_OFF_PITCH,(1u<<22)|(d->heap.first>>3)};
    w[n++]=(struct m64_write){M64_SECONDARY_TEX_OFF,d->heap.first};
    for (unsigned i=0;i<11;i++) if (d->written_textures&(1u<<i))
        w[n++]=(struct m64_write){M64_TEX_0_OFF+i*4,s->texture[i]};
    for (unsigned i=0;i<MACH64_3D_SAVED-1;i++) {
        uint32_t reg=saved_registers[i];
        if (reg==M64_SC_LEFT || reg==M64_SC_RIGHT || reg==M64_SC_TOP || reg==M64_SC_BOTTOM) continue;
        w[n++]=(struct m64_write){reg,s->value[i]};
    }
    if (s->level<=10) w[n++]=(struct m64_write){M64_TEX_0_OFF+s->level*4,s->value[16]};
    w[n++]=(struct m64_write){M64_SC_LEFT_RIGHT,(s->value[4]&0xffffu)|(s->value[5]<<16)};
    w[n++]=(struct m64_write){M64_SC_TOP_BOTTOM,(s->value[6]&0xffffu)|(s->value[7]<<16)};
    int e=emit(d,w,n);
    if (!e) e=m64_render3d_idle(d->engine,&d->budget);
    for (unsigned i=0;!e && i<MACH64_3D_SAVED;i++) {
        uint32_t reg=i<MACH64_3D_SAVED-1 ? saved_registers[i] : M64_TEX_0_OFF+s->level*4,value=0;
        if (i==MACH64_3D_SAVED-1 && s->level>10) break;
        e=read_register(d,reg,&value);
        uint32_t mask=reg==M64_TEX_CNTL ? ~M64_TEX_CACHE_FLUSH : UINT32_MAX;
        if (!e && (value&mask)!=(s->value[i]&mask)) {
            d->failed_register=reg; d->expected=s->value[i]; d->actual=value; e=-EIO;
        }
    }
    for (unsigned i=0;!e && i<11;i++) if (d->written_textures&(1u<<i)) {
        uint32_t reg=M64_TEX_0_OFF+i*4,value=0;
        e=read_register(d,reg,&value);
        if (!e && value!=s->texture[i]) {
            d->failed_register=reg; d->expected=s->texture[i]; d->actual=value; e=-EIO;
        }
    }
    return e;
}

/* ---- VRAM transfers through the owner's claimed heap mapping. */
static uint32_t base(const struct render3d_resource *r) { return r->offset+4096u; }
static uint32_t vram_read(struct mach64_3d_device *d,uint32_t off,unsigned bytes)
{ return d->memory.read(d->memory.ctx,off,bytes); }
static void vram_write(struct mach64_3d_device *d,uint32_t off,uint32_t value,unsigned bytes)
{ d->memory.write(d->memory.ctx,off,value,bytes); }
#define GUARD_WORD 0xa55ac33cu
static int guards(struct mach64_3d_device *d,const struct render3d_resource *r,bool initialize)
{
    uint64_t start=0;
    for (unsigned side=0;side<2;side++) for (unsigned i=0;i<4096;i+=4) {
        if (!(i%512)) start=now(d);
        uint32_t off=r->offset+(side ? r->reserved-4096u : 0)+i;
        if (initialize) vram_write(d,off,GUARD_WORD,4);
        else {
            uint32_t value=vram_read(d,off,4);
            if (value!=GUARD_WORD) { d->expected=GUARD_WORD; d->actual=value; d->guard_offset=off; return -EIO; }
            if (i%512==508 && now(d)-start>=1000) return -ETIMEDOUT;
        }
    }
    return 0;
}
static int upload(struct mach64_3d_device *d,struct render3d_resource *r)
{
    if (r->resident) return guards(d,r,false);
    (void)guards(d,r,true);
    uint32_t pitch=mach64_3d_pitch(r);
    if (r->kind==R3D_COLOR || r->kind==R3D_D16) {
        unsigned bytes=r->kind==R3D_D16 ? 2 : 4;
        for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
            uint32_t value=0; memcpy(&value,(const uint8_t *)r->shadow+y*r->pitch+x*bytes,bytes);
            vram_write(d,base(r)+y*pitch+x*bytes,value,bytes);
        }
    } else if (r->kind==R3D_TEXTURE) {
        /* Canonical ARGB8888 level 0; RGB-only formats carry opaque alpha,
         * alpha-only formats white RGB (render3d.c texture_decode). */
        const uint32_t *texels=r->shadow;
        for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++)
            vram_write(d,base(r)+y*pitch+x*4,texels[y*r->width+x],4);
    }
    d->engine->io.drain(d->engine->io.ctx);
    r->resident=true; return 0;
}
static int readback(struct mach64_3d_device *d,struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return guards(d,r,false);
    unsigned bytes=r->kind==R3D_D16 ? 2 : 4;
    uint32_t pitch=mach64_3d_pitch(r);
    for (unsigned y=0;y<r->height;y++) {
        /* Boot qualification samples every 16th and the last row; ordinary
         * jobs publish complete canonical attachments. */
        if (d->fullsize_testing && y%16 && y!=r->height-1) continue;
        for (unsigned first=0;first<r->width;first+=128) {
            uint64_t start=now(d);
            unsigned end=first+128<r->width ? first+128 : r->width;
            for (unsigned x=first;x<end;x++) {
                uint32_t value=vram_read(d,base(r)+y*pitch+x*bytes,bytes);
                memcpy((uint8_t *)r->shadow+y*r->pitch+x*bytes,&value,bytes);
            }
            if (now(d)-start>=1000) return -ETIMEDOUT;
        }
    }
    return guards(d,r,false);
}

/* ---- Emission. */
static uint32_t surface(const struct render3d_resource *r)
{ return ((mach64_3d_pitch(r)/(r->kind==R3D_D16 ? 2u : 4u)/8u)<<22)|(base(r)>>3); }
static unsigned log2u(unsigned v) { unsigned n=0; while ((1u<<n)<v) n++; return n; }
/* DRM clear (FreeBSD 8.4 mach64_state.c:274-354) with the 2D driver's
 * qualified DST_Y_X/DST_HEIGHT_WIDTH launch; D16 uses the RGB565 datatype. */
static int clear_resource(struct mach64_3d_device *d,const struct render3d_resource *r,uint32_t value)
{
    uint32_t type=r->kind==R3D_D16 ? M64_DATATYPE_RGB565 : M64_DATATYPE_ARGB8888;
    const struct m64_write w[]={
        {M64_Z_CNTL,0},{M64_SCALE_3D_CNTL,0},
        {M64_SC_LEFT_RIGHT,(r->width-1)<<16},{M64_SC_TOP_BOTTOM,(r->height-1)<<16},
        {M64_CLR_CMP_CNTL,0},{M64_DST_CNTL,M64_DST_2D},
        {M64_DP_PIX_WIDTH,type|type<<4|type<<8|type<<16|type<<28},
        {M64_DP_FRGD_CLR,r->kind==R3D_D16 ? value&0xffffu : value},{M64_DP_WRITE_MASK,UINT32_MAX},
        {M64_DP_MIX,M64_DP_MIX_COPY},{M64_DP_SRC,M64_DP_SRC_FILL},
        {M64_DST_OFF_PITCH,surface(r)},{M64_DST_Y_X,0},
        {M64_DST_HEIGHT_WIDTH,(r->width<<16)|r->height}
    };
    int e=emit(d,w,ARRAY_SIZE(w));
    if (!e) d->clears++;
    return e;
}
/* DRM swap (mach64_state.c:391-426) with the 2D driver's copy registers. */
static int copy_resource(struct mach64_3d_device *d,const struct render3d_resource *src,
                         const struct render3d_resource *dst)
{
    const uint32_t t=M64_DATATYPE_ARGB8888;
    const struct m64_write w[]={
        {M64_Z_CNTL,0},{M64_SCALE_3D_CNTL,0},
        {M64_SC_LEFT_RIGHT,(src->width-1)<<16},{M64_SC_TOP_BOTTOM,(src->height-1)<<16},
        {M64_CLR_CMP_CNTL,0},{M64_DST_CNTL,M64_DST_2D},
        {M64_DP_PIX_WIDTH,t|t<<4|t<<8|t<<16|t<<28},{M64_DP_WRITE_MASK,UINT32_MAX},
        {M64_DP_MIX,M64_DP_MIX_COPY},{M64_DP_SRC,M64_DP_SRC_BLIT},
        {M64_SRC_OFF_PITCH,surface(src)},{M64_DST_OFF_PITCH,surface(dst)},
        {M64_SRC_Y_X,0},{M64_SRC_HEIGHT1_WIDTH1,(src->width<<16)|src->height},
        {M64_DST_Y_X,0},{M64_DST_HEIGHT_WIDTH,(src->width<<16)|src->height}
    };
    int e=emit(d,w,ARRAY_SIZE(w));
    if (!e) d->copies++;
    return e;
}
static const uint8_t compare_code[8]={1,0,7,3,2,5,6,4}; /* render3d order -> Z/alpha TEST */
static const uint8_t blend_code[10]={1,0,4,5,2,2,3,3,6,7}; /* ONE..INV_DST_ALPHA */
/* DRM emit order (mach64_state.c:157-205, then the scissor at :132-133);
 * register values follow Mesa mach64DDInitState and the update routines. */
static unsigned state_writes(struct mach64_3d_device *d,const struct render3d_request *q,
                             struct render3d_resource *const r[R3D_REFS],struct m64_write *w)
{
    unsigned n=0;
    bool textured=q->flags&R3D_TEXTURED,second=q->flags&R3D_TEXTURE1;
    uint32_t scale=M64_SCALE_BASE|(textured ? M64_FCN_TEXTURE : M64_FCN_SHADE);
    if (q->state.blend && !(q->state.source==R3D_ONE && q->state.destination==R3D_ZERO))
        scale|=M64_ALPHA_FOG_EN_ALPHA|((uint32_t)blend_code[q->state.source]<<16)|((uint32_t)blend_code[q->state.destination]<<19);
    else scale|=M64_BLEND_SRC_ONE;
    uint32_t tex=M64_TEX_CNTL_BASE,size=0;
    if (textured) {
        const struct render3d_resource *t=r[2];
        const struct render3d_sampler *s=&q->state.sampler[0];
        bool alpha=alpha_format(t->format),rgb=t->format==R3D_RGB565 || t->format==R3D_L8;
        if (s->combine==R3D_REPLACE || (s->combine==R3D_DECAL && rgb)) scale|=M64_TEX_LIGHT_REPLACE;
        else if (s->combine==R3D_DECAL) scale|=M64_TEX_LIGHT_DECAL;
        else scale|=M64_TEX_LIGHT_MODULATE;
        if (second) scale|=M64_TEX_BLEND_TRILINEAR|M64_TEX_CACHE_SPLIT;
        else if (s->filter==R3D_BILINEAR) scale|=M64_TEX_BLEND_LINEAR;
        if (s->filter==R3D_BILINEAR) scale|=M64_BILINEAR_TEX_EN;
        if (alpha) scale|=M64_TEX_MAP_AEN;
        if (!s->repeat_u) tex|=M64_TEXTURE_CLAMP_S;
        if (!s->repeat_v) tex|=M64_TEXTURE_CLAMP_T;
        unsigned lw=log2u(t->width),lh=log2u(t->height);
        size=lw|(lw>lh ? lw : lh)<<4|lh<<8;
        if (second) {
            const struct render3d_resource *u=r[4];
            const struct render3d_sampler *s1=&q->state.sampler[1];
            unsigned l1=log2u(u->width),h1=log2u(u->height);
            size|=(l1|(l1>h1 ? l1 : h1)<<4|h1<<8)<<16;
            tex|=M64_TEXTURE_COMPOSITE|M64_SECONDARY_STW;
            if (s1->filter==R3D_BILINEAR) tex|=M64_COMP_BLEND_BILINEAR|M64_COMP_FILTER_BILINEAR;
            if (alpha_format(u->format)) tex|=M64_COMP_ALPHA;
            if (!s1->repeat_u) tex|=M64_SEC_TEX_CLAMP_S;
            if (!s1->repeat_v) tex|=M64_SEC_TEX_CLAMP_T;
        }
    }
    uint32_t z=0;
    if (q->flags&(R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) {
        z=M64_Z_EN|((uint32_t)((q->flags&R3D_DEPTH_TEST) ? compare_code[q->state.depth_func] : 7u)<<4);
        if (q->flags&R3D_DEPTH_WRITE) z|=M64_Z_MASK_EN;
    }
    uint32_t alpha=M64_ALPHA_DST_SRCALPHA;
    if (q->state.blend && q->state.source==R3D_ZERO && q->state.destination==R3D_ONE) alpha=M64_ALPHA_DST_DSTALPHA;
    if (q->state.blend && q->state.source==R3D_ZERO && q->state.destination==R3D_ZERO) alpha=M64_ALPHA_DST_ZERO;
    if (q->state.alpha_test) alpha|=M64_ALPHA_TEST_EN|((uint32_t)compare_code[q->state.alpha_func]<<4)|(q->state.alpha_ref<<16);
    else alpha|=7u<<4;
    const uint32_t a=M64_DATATYPE_ARGB8888;
    w[n++]=(struct m64_write){M64_DP_MIX,M64_DP_MIX_3D};
    w[n++]=(struct m64_write){M64_DP_SRC,M64_DP_SRC_3D};
    w[n++]=(struct m64_write){M64_CLR_CMP_CNTL,0};
    w[n++]=(struct m64_write){M64_DST_CNTL,M64_DST_3D};
    w[n++]=(struct m64_write){M64_DST_OFF_PITCH,surface(r[0])};
    if (r[1]) w[n++]=(struct m64_write){M64_Z_OFF_PITCH,surface(r[1])};
    w[n++]=(struct m64_write){M64_Z_CNTL,z};
    w[n++]=(struct m64_write){M64_ALPHA_TST_CNTL,alpha};
    w[n++]=(struct m64_write){M64_SCALE_3D_CNTL,scale};
    w[n++]=(struct m64_write){M64_DP_WRITE_MASK,UINT32_MAX};
    w[n++]=(struct m64_write){M64_DP_PIX_WIDTH,a|a<<4|a<<8|a<<16|a<<28};
    w[n++]=(struct m64_write){M64_SETUP_CNTL,0};
    if (textured) {
        unsigned level=(size>>4)&15u;
        w[n++]=(struct m64_write){M64_TEX_SIZE_PITCH,size};
        w[n++]=(struct m64_write){M64_TEX_CNTL,tex};
        if (second) w[n++]=(struct m64_write){M64_SECONDARY_TEX_OFF,base(r[4])};
        w[n++]=(struct m64_write){M64_TEX_0_OFF+level*4,base(r[2])};
        d->written_textures|=1u<<level;
    }
    w[n++]=(struct m64_write){M64_SC_LEFT_RIGHT,(r[0]->width-1)<<16};
    w[n++]=(struct m64_write){M64_SC_TOP_BOTTOM,(r[0]->height-1)<<16};
    return n;
}
static int draw(struct mach64_3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    unsigned n=state_writes(d,q,r,d->writes);
    bool second=q->flags&R3D_TEXTURE1;
    for (unsigned first=0;first<q->count;first+=3) {
        unsigned count=0;
        int e=mach64_3d_triangle(q,first,second,d->writes+n,&count); if (e) return e;
        n+=count; if (count) d->triangles++;
    }
    return emit(d,d->writes,n);
}
static void quarantine(void *device);
static int execute(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct mach64_3d_device *d=device;
    int e=validate(d,q,r); if (e) return e; /* no access on invalid extents */
    if (!d->locked || (!d->qualified && !d->probing) || d->lost) return -ENODEV;
    if (!d->engine->active || d->engine->quarantined || !d->engine->cpu_safe) return -EIO;
    uint64_t before=d->engine->render3d_writes;
    window(d); d->written_textures=0;
    d->failed_phase=1; e=save_state(d); if (e) return e; /* reads only */
    d->failed_phase=2;
    for (unsigned i=0;!e && i<R3D_REFS;i++) if (r[i]) e=upload(d,r[i]);
    /* CPU transfers are separate from the engine window below. */
    window(d);
    if (!e) { d->failed_phase=3; if (q->flags&R3D_CLEAR_COLOR) e=clear_resource(d,r[0],q->clear_color); }
    if (!e && (q->flags&R3D_CLEAR_DEPTH)) e=clear_resource(d,r[1],q->clear_depth);
    if (!e && q->count) { d->failed_phase=4; e=draw(d,q,r); }
    if (!e && (q->flags&R3D_COPY)) { d->failed_phase=5; e=copy_resource(d,r[0],r[3]); }
    if (!e && d->engine->render3d_writes!=before) { d->failed_phase=6; e=m64_render3d_idle(d->engine,&d->budget); }
    if (!e) {
        d->failed_phase=8;
        for (unsigned i=0;i<R3D_REFS && !e;i++) if (r[i]) e=readback(d,r[i]);
    }
    /* Any register write needs an idle proof and a verified restoration;
     * a writer that cannot be proven idle is never written again. */
    if (d->engine->render3d_writes!=before || !d->engine->cpu_safe) {
        uint32_t phase=d->failed_phase;
        if (!d->engine->cpu_safe) {
            window(d); d->failed_phase=6;
            if (d->engine->quarantined || m64_render3d_idle(d->engine,&d->budget)) {
                quarantine(d); return e ? e : -EIO;
            }
        }
        window(d); d->failed_phase=7;
        int restored=restore_state(d);
        if (restored) { quarantine(d); return restored; }
        d->failed_phase=phase;
    }
    if (!e) d->failed_phase=0;
    return e;
}
static bool qualified(void *device,uint32_t generation)
{
    struct mach64_3d_device *d=device;
    return d && d->qualified && !d->lost && d->generation==generation && d->engine &&
        d->engine->active && !d->engine->quarantined && d->engine->render3d_qualified &&
        d->engine->fb.generation==generation && d->engine->fb.width==d->mode_width &&
        d->engine->fb.height==d->mode_height && m64_format(&d->engine->fb);
}
static int enter(void *device)
{
    struct mach64_3d_device *d=device;
    if (!d || d->locked || d->lost || !d->memory.enter) return -EBUSY;
    int e=d->memory.enter(d->memory.ctx,d->generation);
    if (!e) d->locked=true;
    return e;
}
static void leave(void *device)
{
    struct mach64_3d_device *d=device;
    d->locked=false; d->memory.leave(d->memory.ctx);
}
static int idle(void *device)
{
    struct mach64_3d_device *d=device;
    return d->engine->cpu_safe && !d->engine->quarantined ? 0 : -EIO;
}
static void quarantine(void *device)
{
    struct mach64_3d_device *d=device;
    if (!d || d->lost) return;
    d->qualified=d->minimum_qualified=d->fullsize_qualified=d->perspective_qualified=false;
    d->alpha_qualified=d->blend_qualified=false; d->lost=true;
    d->engine->render3d_qualified=false;
    if (d->locked) m64_quarantine(d->engine,"render3d_fault");
    else {
        /* A failed enter/loss callback owns no presenter guard. Close both
         * domains without even a cursor MMIO write; the normal guarded 2D
         * stop proof may hide it later. CPU access remains forbidden. */
        d->engine->active=false; d->engine->quarantined=true;
        d->engine->cpu_safe=false; d->engine->reason="render3d_fault";
    }
    if (d->lost_notify) d->lost_notify(d->memory.ctx);
}
/* The owner unit is linked into the kernel; host backend fixtures omit it. */
extern int mach64_3d_owner_attach(struct render3d_service *,const struct render3d_host *,uint32_t) __attribute__((weak));
static void detach(struct render3d_service *s,void *owner)
{
    struct mach64_3d_device *d=owner;
    if (d && d->consumer==s) d->consumer=0;
}
static void query_caps(void *owner,struct ciuki_r3d_caps *out)
{
    const struct mach64_3d_device *d=owner;
    memset(out,0,sizeof(*out));
    out->version=CIUKI_R3D_VERSION;
    if (!d || !qualified(owner,d->generation)) return;
    *out=*mach64_3d_backend()->capabilities;
    if (d->alpha_qualified) out->alpha_functions=255;
    if (d->blend_qualified) out->blend_factors=(1u<<R3D_ONE)|(1u<<R3D_ZERO);
    if (d->fullsize_qualified) {
        out->max_size=d->mode_width>d->mode_height ? d->mode_width : d->mode_height;
        out->max_width=d->mode_width; out->max_height=d->mode_height;
        out->max_vertices=R3D_VERTICES;
        /* One mode-sized quad, the work Q2 completed inside the window. */
        out->max_pixel_work=2*d->mode_width*d->mode_height;
        out->sampling_features=CIUKI_R3D_AFFINE_EXACT;
        if (d->perspective_qualified) {
            out->sampling_features|=CIUKI_R3D_PROJECTIVE_BILINEAR;
            out->max_texture_size=MACH64_3D_QUALIFIED_TEXTURE;
        }
    }
}
static uint32_t texture_pool_bytes(void *owner)
{
    const struct mach64_3d_device *d=owner;
    return d && d->texture_bytes ? d->texture_bytes : MACH64_3D_TEXTURE_MIN;
}
const struct render3d_backend *mach64_3d_backend(void)
{
    /* Implemented subset only: D16, ARGB8888 targets, two stages of
     * NEAREST/BILINEAR MODULATE/REPLACE/DECAL, no mip filters, no ADD.
     * Alpha/blend bits appear only after their Q3 groups pass. */
    static const struct ciuki_r3d_caps capabilities={
        .version=CIUKI_R3D_VERSION,.features=R3D_ALL_FEATURES,.max_size=64,
        .max_vertices=3,.texture_units=2,.max_texture_size=64,
        .depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888,
        .texture_formats=63,.depth_functions=255,.alpha_functions=0,
        .blend_factors=1u<<R3D_ONE,
        .combine_modes=(1u<<R3D_MODULATE)|(1u<<R3D_REPLACE)|(1u<<R3D_DECAL),
        .filters=(1u<<R3D_NEAREST)|(1u<<R3D_BILINEAR),.max_pixel_work=4096};
    static const struct render3d_backend backend={.features=R3D_ALL_FEATURES,.qualified=qualified,
        .layout=layout,.validate=validate,.enter=enter,.leave=leave,.execute=execute,
        .idle=idle,.quarantine=quarantine,.attach=mach64_3d_owner_attach,.max_size=64,.detach=detach,
        .texture_pool_bytes=texture_pool_bytes,.capabilities=&capabilities,.query_caps=query_caps,
        .resident_bytes=(sizeof(struct mach64_3d_device)+4095u)&~4095u,
        .whole_context_fallback=true};
    return &backend;
}

/* ---- Mode heap: above the scanout lease, below the 2D cursor page (or the
 * top-of-aperture register alias the 2D driver already avoids). */
int mach64_3d_mode_heap(const struct m64_engine *s,const struct fb_device *fb,
                        struct render3d_extent *heap,uint32_t *minimum,uint32_t *full)
{
    if (!s || !fb || !heap || !minimum || !full || !m64_format(fb) || !fb->width || !fb->height ||
        fb->width>1024 || fb->height>1024 || !s->vram_bytes || s->vram_bytes>16u*1024*1024) return -EOPNOTSUPP;
    uint32_t top=s->vram_bytes;
    if (top>0x7ff000u) top=0x7ff000u;
    if (s->cursor && s->cursor_offset && s->cursor_offset<top) top=s->cursor_offset;
    uint64_t first=((uint64_t)fb->size+4095u)&~4095ull;
    uint32_t end=top&~4095u;
    if (first>=end) return -ENOMEM;
    /* Q1: 64-square colour, D16 and linear copy (each with two guards),
     * plus the minimum texture pool. Q2: the same at mode size. */
    uint32_t cp=((fb->width+7)&~7u)*4,dp=((fb->width+7)&~7u)*2;
    *minimum=3*8192u+16384u+8192u+16384u+MACH64_3D_TEXTURE_MIN;
    *full=2*(((cp*fb->height+4095)&~4095u)+8192u)+((dp*fb->height+4095)&~4095u)+8192u+MACH64_3D_FULL_TEXTURES;
    *heap=(struct render3d_extent){(uint32_t)first,end-(uint32_t)first};
    return heap->bytes<*minimum ? -ENOMEM : 0;
}

/* ---- Q1: minimum qualification on the owner's 64-square scratch. */
/* Containment policy for qualification: engine, guard and restoration
 * faults (-EIO) and a writer that cannot be proven idle are quarantined;
 * a refusal before the first write or a restored numeric/budget miss is
 * not. execute() already quarantined if restoration itself failed. */
static void probe_failure(struct mach64_3d_device *d,int e)
{
    if (e==-EIO || (e==-ETIMEDOUT && idle(d))) quarantine(d);
}
static int probe_job(struct mach64_3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    int e=validate(d,q,r); if (e) return e;
    e=enter(d); if (e) return e;
    e=execute(d,q,r);
    if (e) probe_failure(d,e); /* quarantine while presenter/engine are held */
    leave(d);
    return e;
}
/* D-F2: after a probe that wrote registers, exercise the actual 2D owner's
 * fill/copy entry points with its restored pixel format and baseline state.
 * Only the two surface addresses/pitches are redirected into guarded scratch;
 * no scanout byte is used. X.Org's DRI-to-2D restore is atimach64accel.c:171-207;
 * the 2D operations and their bounded waits are engine.c:fill/copy. */
static int check_2d(struct mach64_3d_device *d,struct render3d_resource *const r[R3D_REFS])
{
    int e=enter(d);
    if (e) { d->selfcheck_error=e; quarantine(d); return e; }
    struct m64_engine *s=d->engine;
    const unsigned bytes=s->fb.bpp/8u;
    const uint32_t pattern=bytes==2 ? 0x56a9u : 0xff56a98cu;
    uint32_t pitch=((r[0]->width+7)&~7u)*bytes;
    const struct fb_rect small={0,0,8,8};
    d->failed_phase=11;
    e=save_state(d);
    if (!e) {
        for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
            vram_write(d,base(r[0])+y*pitch+x*bytes,0,bytes);
            vram_write(d,base(r[3])+y*pitch+x*bytes,0,bytes);
        }
        s->io.drain(s->io.ctx);
        const struct m64_write redirect[]={
            {M64_DST_OFF_PITCH,((pitch/bytes/8u)<<22)|(base(r[0])>>3)},
            {M64_SRC_OFF_PITCH,((pitch/bytes/8u)<<22)|(base(r[0])>>3)}
        };
        window(d); e=emit(d,redirect,ARRAY_SIZE(redirect));
        if (!e) e=s->ops.fill_rect(s->ops.ctx,&small,pattern);
        const struct m64_write destination={M64_DST_OFF_PITCH,((pitch/bytes/8u)<<22)|(base(r[3])>>3)};
        if (!e) e=emit(d,&destination,1);
        if (!e) e=s->ops.copy_rect(s->ops.ctx,&small,0,0);
        for (unsigned y=0;!e && y<8;y++) for (unsigned x=0;!e && x<8;x++) {
            uint32_t a=vram_read(d,base(r[0])+y*pitch+x*bytes,bytes);
            uint32_t b=vram_read(d,base(r[3])+y*pitch+x*bytes,bytes);
            if (a!=pattern || b!=pattern) { d->expected=pattern; d->actual=a!=pattern ? a : b; e=-EIO; }
        }
        if (!e) e=guards(d,r[0],false);
        if (!e) e=guards(d,r[3],false);
        if (!s->quarantined && s->cpu_safe) {
            window(d);
            int restored=restore_state(d);
            if (restored) e=restored;
        } else e=e ? e : -EIO;
    }
    r[0]->resident=r[3]->resident=false;
    d->selfcheck_error=e;
    if (e) quarantine(d);
    else { d->selfchecks++; d->failed_phase=0; }
    leave(d);
    return e;
}
static bool channel_close(uint32_t a,uint32_t b,unsigned tolerance)
{
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned x=a>>shift&255,y=b>>shift&255;
        if ((x>y ? x-y : y-x)>tolerance) return false;
    }
    return true;
}
static int coverage_job(struct mach64_3d_device *d,struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],
                        struct render3d_resource *reference)
{
    /* The production scalar renderer is the coverage oracle: exact
     * coverage, colour within two levels. Unrelated to the encoder. */
    struct render3d_resource *ref[R3D_REFS]={reference,0,0,0,0};
    struct render3d_job job={0};
    uint32_t flags=q->flags;
    q->flags&=~R3D_COPY; /* the reference has no copy target */
    while (!render3d_scalar_slice(q,ref,&job,&d->consumer->host)) {}
    q->flags=flags;
    int e=probe_job(d,q,r);
    if (e) return e;
    const uint32_t *hw=r[0]->shadow,*sw=reference->shadow,*copy=r[3]->shadow;
    for (unsigned i=0;i<64*64;i++) {
        bool a=(hw[i]&0xffffffu)!=0,b=(sw[i]&0xffffffu)!=0;
        if (a!=b || !channel_close(hw[i],sw[i],2) || hw[i]!=copy[i]) {
            d->coverage_mismatches++; d->expected=sw[i]; d->actual=hw[i]; d->guard_offset=i; return -EILSEQ;
        }
    }
    return 0;
}
int mach64_3d_probe(struct mach64_3d_device *d)
{
    if (!d || !d->engine || !d->memory.read || !d->memory.write || !d->memory.enter ||
        !d->memory.leave || !d->generation || !d->heap.bytes || (d->heap.first&4095u) ||
        d->heap.first>UINT32_MAX-d->heap.bytes || d->heap.bytes<MACH64_3D_TEXTURE_MIN+65536u ||
        !d->engine->active || d->engine->quarantined || !d->engine->cpu_safe ||
        d->minimum_qualified || d->qualified || d->lost || d->probing) return -ENODEV;
    if (!d->consumer || !d->consumer->contexts[0].resources) return -ENOMEM;
    struct render3d_resource *res=d->consumer->contexts[0].resources;
    struct render3d_resource *r[R3D_REFS]={&res[0],&res[1],&res[2],&res[3],0};
    struct render3d_resource *reference=&res[4];
    if (!reference->live || reference->hardware || reference->kind!=R3D_COLOR || reference->width!=64) return -ENOMEM;
    struct render3d_request *q=(void *)d->consumer->ring[0];
    memset(q,0,sizeof(*q)); q->flags=R3D_FEATURES; q->count=3; q->clear_depth=65535;
    q->vertices[0]=(struct render3d_vertex){8<<16,8<<16,16384,65536,0xffffffffu,0,0};
    q->vertices[1]=(struct render3d_vertex){56<<16,8<<16,16384,65536,0xffffffffu,65536,0};
    q->vertices[2]=(struct render3d_vertex){8<<16,56<<16,16384,65536,0xffffffffu,0,65536};
    int e=validate(d,q,r); if (e) return e;
    uint64_t before=d->engine->render3d_writes;
    d->qualification_started=now(d);
    d->probing=true; d->engine->render3d_qualified=true;
    d->failed_phase=0;
    e=probe_job(d,q,r);
    /* Q1a: nearest texture quadrants, D16 value, clear outside, copy. */
    if (!e) {
        const uint32_t *c=r[0]->shadow,*copy=r[3]->shadow; const uint16_t *z=r[1]->shadow;
        uint32_t color=c[16*64+16]&0xffffffu;
        d->expected=0x4080c0u; d->actual=color;
        if (color!=0x4080c0u || (copy[16*64+16]&0xffffffu)!=0x4080c0u || (c[12*64+48]&0xffffffu)!=0xc04080u ||
            (c[63*64+63]&0xffffffu) || z[63*64+63]!=65535) e=-EILSEQ;
        if (!e && (z[16*64+16]<16383 || z[16*64+16]>16385)) { d->expected=16384; d->actual=z[16*64+16]; e=-EILSEQ; }
        if (e) d->failed_phase=9;
    }
    /* Q1b: a farther triangle fails LESS and leaves colour and depth. */
    if (!e) {
        q->flags=R3D_TEXTURED|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=49152; q->vertices[i].color=0xffff0000u; }
        e=probe_job(d,q,r);
        if (!e && ((((uint32_t *)r[0]->shadow)[16*64+16]&0xffffffu)!=0x4080c0u ||
            ((uint16_t *)r[1]->shadow)[16*64+16]>16385)) { d->failed_phase=9; e=-EILSEQ; }
    }
    /* Q1c: Gouraud red/green/blue modulated by the texel, +-2 levels. */
    if (!e) {
        q->flags=R3D_FEATURES;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=16384; q->vertices[i].color=0xff000000u|(0xffu<<(16-i*8)); }
        e=probe_job(d,q,r);
        if (!e && !channel_close(((uint32_t *)r[0]->shadow)[16*64+16]&0xffffffu,0x00291722u,2)) {
            d->failed_phase=9; d->expected=0x00291722u; d->actual=((uint32_t *)r[0]->shadow)[16*64+16]; e=-EILSEQ;
        }
    }
    /* Q1d: coverage ties. Edges through sample centres in every
     * orientation; the shared diagonal must be owned exactly once. */
    if (!e) {
        static const int32_t tri[2][3][2]={{{34,34},{162,34},{34,162}},{{162,162},{162,34},{34,162}}};
        for (unsigned t=0;!e && t<2;t++) {
            memset(q,0,sizeof(*q)); q->flags=(t ? 0 : R3D_CLEAR_COLOR)|R3D_COPY; q->count=3;
            for (unsigned i=0;i<3;i++) q->vertices[i]=(struct render3d_vertex){tri[t][i][0]<<14,tri[t][i][1]<<14,0,65536,
                t ? 0xff40c080u : 0xffffffffu,0,0};
            struct render3d_resource *rr[R3D_REFS]={r[0],0,0,r[3],0};
            e=coverage_job(d,q,rr,reference);
            if (e==-EILSEQ) d->failed_phase=10;
        }
    }
    if (!e) for (unsigned i=0;i<64*64;i++)
        if (((uint32_t *)r[0]->shadow)[i]!=((uint32_t *)r[3]->shadow)[i]) { d->failed_phase=9; e=-EILSEQ; break; }
    /* Numeric/coverage mismatch after a verified restore is a refusal of a
     * healthy engine; guard, engine or restore faults are quarantined. */
    if (!d->lost && d->engine->render3d_writes!=before) {
        uint32_t phase=d->failed_phase;
        int checked=check_2d(d,r);
        if (checked) e=checked;
        else if (e) d->failed_phase=phase;
    }
    d->probing=false;
    d->minimum_qualified=!e;
    if (e) d->engine->render3d_qualified=false;
    return e;
}

/* ---- Q2 mode-size scenes and Q3 precision envelope. Boot-only scenes
 * share input with the scalar reference, never rasterization. */
static void fullsize_scene(struct render3d_request *q,unsigned w,unsigned h,unsigned phase)
{
    memset(q,0,sizeof(*q));
    q->flags=R3D_TEXTURED|R3D_TEXTURE1|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
    if (!phase) q->flags|=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH;
    q->clear_color=0xff102030u; q->clear_depth=65535; q->count=6;
    q->state.sampler[0]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,0,0};
    q->state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,0,0};
    if (phase==2) {
        q->state.blend=1; q->state.source=R3D_ZERO; q->state.destination=R3D_ONE;
        q->state.sampler[0].repeat_u=q->state.sampler[0].repeat_v=1;
        q->state.sampler[1].repeat_u=q->state.sampler[1].repeat_v=1;
    }
    static const unsigned corners[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
    for (unsigned i=0;i<6;i++) {
        unsigned x=corners[i][0],y=corners[i][1];
        q->vertices[i]=(struct render3d_vertex){(int32_t)(x*w<<16),(int32_t)(y*h<<16),
            phase==1 ? 49152u : phase==2 ? 8192u : 32768u,
            65536,phase==1 ? 0xffff0000u : phase==2 ? 0x8090b0d0u : 0xffe0c0a0u,
            (int32_t)(x*(phase==2 ? 16 : 1)*65536),(int32_t)(y*(phase==2 ? 16 : 1)*65536)};
        q->uv1[i][0]=q->vertices[i].v; q->uv1[i][1]=q->vertices[i].u;
    }
    if (phase==3) {
        q->count=R3D_VERTICES;
        for (unsigned triangle=0;triangle<256;triangle++) for (unsigned v=0;v<3;v++) {
            unsigned i=triangle*3+v;
            unsigned x=(triangle%16)*(w-16)/15+(v==1 ? 16 : 0);
            unsigned y=(triangle/16)*(h-16)/15+(v==2 ? 16 : 0);
            /* A depth plane inside the provable +-1 span (depth_admission). */
            q->vertices[i]=(struct render3d_vertex){(int32_t)(x<<16),(int32_t)(y<<16),4096u+v*8192u+triangle*16u,65536,0xffc0e0a0u,
                v==1 ? 49152 : 16384,v==2 ? 49152 : 16384};
            q->uv1[i][0]=q->vertices[i].v; q->uv1[i][1]=q->vertices[i].u;
        }
    }
}
static int fullsize_compare(struct mach64_3d_device *d,struct render3d_resource *const hw[R3D_REFS],
                            struct render3d_resource *const ref[R3D_REFS])
{
    const uint32_t *a=hw[0]->shadow,*b=ref[0]->shadow,*copy=hw[3]->shadow;
    const uint16_t *az=hw[1]->shadow,*bz=ref[1]->shadow;
    uint32_t ah=2166136261u,bh=2166136261u;
    for (unsigned i=0;i<hw[0]->width*hw[0]->height;i++) {
        unsigned row=i/hw[0]->width;
        if (row%16 && row!=hw[0]->height-1) continue;
        if (a[i]!=copy[i] || (a[i]==0xff102030u)!=(b[i]==0xff102030u)) { d->expected=b[i]; d->actual=a[i]; return -EILSEQ; }
        for (unsigned shift=0;shift<32;shift+=8) {
            unsigned ac=a[i]>>shift&255,bc=b[i]>>shift&255,delta=ac>bc ? ac-bc : bc-ac;
            if (delta>d->max_channel_error) d->max_channel_error=delta;
            if (delta>2) { d->expected=b[i]; d->actual=a[i]; return -EILSEQ; }
        }
        unsigned delta=az[i]>bz[i] ? az[i]-bz[i] : bz[i]-az[i];
        if (delta>d->max_depth_error) d->max_depth_error=delta;
        if (delta>1) { d->expected=bz[i]; d->actual=az[i]; return -EILSEQ; }
        ah=(ah^a[i])*16777619u; ah=(ah^az[i])*16777619u;
        bh=(bh^b[i])*16777619u; bh=(bh^bz[i])*16777619u;
        d->sample_count++;
    }
    d->hardware_checksum=ah; d->reference_checksum=bh; return 0;
}
#include "mach64_3d_precision.h"
/* Q3 phases: the f7-11 envelope scenes the engine can express. 0-3
 * perspective ramps/checkers, 4 filter-fraction sweep, 5-6 large REPEAT
 * coordinates in both unit orders, 7-8 CLAMP extremes (sampling group);
 * 9 sampled alpha test on one unit (alpha group); 10-11 the two
 * expressible blend pairs (blend group). No mip phases: mip filters are
 * refused before emission. */
static const uint8_t envelope_phase[MACH64_3D_PHASES]={0,1,2,3,6,7,8,21,22,9,11,12};
static void precision_scene(struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],unsigned phase)
{
    unsigned p=envelope_phase[phase];
    fullsize_scene(q,r[0]->width,r[0]->height,0);
    for (unsigned stage=0;stage<2;stage++) {
        struct render3d_resource *t=r[stage ? 4 : 2];
        unsigned width=t->width,height=t->height;
        uint32_t *pixels=t->shadow;
        for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
            unsigned ramp=p&1 ? ((x+y)&1)*255 : width>1 ? x*255/(width-1) : 0;
            unsigned cross=p&1 ? ((x/2+y/3)&1)*255 : height>1 ? y*255/(height-1) : 0;
            unsigned alpha=p>=9 && p<21 ? ((x*17+y*29)&255) : 255;
            pixels[y*width+x]=(alpha<<24)|(stage ? 0xff0000u|(ramp<<8)|cross : 0x00ff00u|(ramp<<16)|cross);
            if (p==6) pixels[y*width+x]=stage ? 0xffff00ffu|(((x&1)*255)<<8) : 0xff00ffffu|(((x&1)*255)<<16);
            /* The alpha phase runs unit 0 alone; a white unit 1 makes the
             * two-unit envelope oracle an exact single-unit oracle. */
            if (phase==9 && stage) pixels[y*width+x]=0xffffffffu;
        }
        t->resident=false;
        q->state.sampler[stage]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,p<21,p<21};
    }
    for (unsigned i=0;i<6;i++) {
        struct render3d_vertex *v=&q->vertices[i];
        unsigned x=v->x!=0,y=v->y!=0;
        v->rhw=p<2 ? (x ? 131072 : 32768) : (y ? 32768 : 131072);
        v->color=0xffffffffu;
        v->u=(int32_t)(x*4*65536)-2*65536+1385;
        v->v=(int32_t)(y*2*65536)-65536+1190;
        q->uv1[i][0]=(int32_t)(y*3*65536)-65536+3522;
        q->uv1[i][1]=(int32_t)(x*2*65536)-65536+3570;
        if (p==6) {
            v->rhw=65536;
            v->u=(32768+(int32_t)x*65536)/(int32_t)r[2]->width; v->v=32768/(int32_t)r[2]->height;
            q->uv1[i][0]=(32768+(1-(int32_t)x)*65536)/(int32_t)r[4]->width; q->uv1[i][1]=32768/(int32_t)r[4]->height;
        }
        if (p>=7) {
            v->rhw=(x ? 256u : 64u)*65536;
            v->u=(int32_t)(x*30*65536)-15*65536+1385;
            v->v=(int32_t)(y*28*65536)-14*65536+1190;
            q->uv1[i][0]=(int32_t)(y*28*65536)-14*65536+3522;
            q->uv1[i][1]=(int32_t)(x*30*65536)-15*65536+3570;
        }
        if (p>=21) {
            v->u=(p==21 ? -16 : 16)*65536; v->v=-v->u;
            q->uv1[i][0]=-v->u; q->uv1[i][1]=v->u;
        }
    }
    if (phase==9) { q->state.alpha_test=1; q->state.alpha_func=R3D_GEQUAL; q->state.alpha_ref=64; }
    if (phase>=10) { q->state.blend=1; q->state.source=R3D_ZERO; q->state.destination=phase==10 ? R3D_ONE : R3D_ZERO; }
}
static int precision_phase(struct mach64_3d_device *d,struct render3d_request *q,
    struct render3d_resource *const hardware[R3D_REFS],struct render3d_resource *const reference[R3D_REFS],unsigned phase)
{
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
    for (unsigned i=0;i<R3D_REFS;i++) { hw[i]=hardware[i]; ref[i]=reference[i]; }
    if (envelope_phase[phase]==8) {
        hw[2]=hardware[4]; hw[4]=hardware[2]; ref[2]=reference[4]; ref[4]=reference[2];
    }
    uint32_t levels[2][8]={{0}};
    d->precision_phase=phase+1;
    precision_scene(q,hw,phase);
    for (unsigned t=2;t<=4;t+=2) memcpy(ref[t]->shadow,hw[t]->shadow,hw[t]->bytes);
    struct render3d_job job={0};
    while (!render3d_scalar_slice(q,ref,&job,&d->consumer->host)) {}
    /* The alpha phase executes one unit; the oracle keeps the white unit. */
    struct render3d_request *run=q;
    struct render3d_resource *exec[R3D_REFS]={hw[0],hw[1],hw[2],hw[3],hw[4]};
    if (phase==9) { q->flags&=~R3D_TEXTURE1; exec[4]=0; }
    int e=validate(d,run,exec); if (e) return e;
    e=enter(d); if (e) return e;
    uint64_t start=now(d);
    e=execute(d,run,exec);
    d->fullsize_us+=now(d)-start;
    if (e) probe_failure(d,e);
    leave(d);
    if (phase==9) q->flags|=R3D_TEXTURE1;
    if (e) return e;
    const uint32_t *a=hw[0]->shadow,*copy=hw[3]->shadow;
    const uint16_t *az=hw[1]->shadow;
    for (unsigned y=0;y<hw[0]->height;y++) {
        if (y%16 && y!=hw[0]->height-1) continue;
        for (unsigned x=0;x<hw[0]->width;x++) {
            unsigned at=y*hw[0]->width+x;
            if (a[at]!=copy[at] ||
                !precision_pixel(q,hw,x,y,a[at],az[at],d->sample_delta_q16,d->filter_fraction_bits)) {
                d->expected=((uint32_t *)ref[0]->shadow)[at]; d->actual=a[at]; return -EILSEQ;
            }
            d->precision_samples++;
            if (envelope_phase[phase]==6) {
                unsigned rr=a[at]>>16&255,g=a[at]>>8&255;
                levels[0][rr/32]|=1u<<(rr%32); levels[1][g/32]|=1u<<(g%32);
            }
        }
    }
    if (envelope_phase[phase]==6) for (unsigned stage=0;stage<2;stage++) {
        unsigned count=0;
        for (unsigned i=0;i<8;i++) for (unsigned bit=0;bit<32;bit++) count+=(levels[stage][i]>>bit)&1;
        d->measured_filter_levels[stage]=count;
        /* Seven filter-fraction bits give at most 129 colours on this edge. */
        if (count<192) return -EILSEQ;
    }
    return 0;
}
static bool healthy(const struct mach64_3d_device *d,int e)
{ return !d->lost && (e==-EILSEQ || e==-ETIMEDOUT || e==-EOPNOTSUPP); }
static int precision_probe(struct mach64_3d_device *d,struct render3d_request *q,
    struct render3d_resource *const hardware[R3D_REFS],struct render3d_resource *const reference[R3D_REFS])
{
    d->sample_delta_q16=CIUKI_R3D_SAMPLE_DELTA_Q16;
    d->filter_fraction_bits=CIUKI_R3D_FILTER_FRACTION_BITS;
    int e=0;
    unsigned failed_phase=0;
    for (unsigned phase=0;phase<9 && !e;phase++) {
        if (now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US) e=-ETIMEDOUT;
        else e=precision_phase(d,q,hardware,reference,phase);
    }
    d->precision_error=e;
    if (e) failed_phase=d->precision_phase;
    d->perspective_qualified=!e;
    if (e && !healthy(d,e)) return e;
    /* Alpha and blend scenes are perspective scenes: they depend on the
     * sampling group, then pass or fail independently of each other. */
    d->alpha_error=d->blend_error=e ? -ECANCELED : 0;
    if (!e) {
        d->fullsize_testing=true;
        d->alpha_error=now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US ? -ETIMEDOUT : precision_phase(d,q,hardware,reference,9);
        if (d->alpha_error && !failed_phase) failed_phase=d->precision_phase;
        if (d->alpha_error && !healthy(d,d->alpha_error)) return d->alpha_error;
        for (unsigned phase=10;phase<MACH64_3D_PHASES && !d->blend_error;phase++)
            d->blend_error=now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US ? -ETIMEDOUT : precision_phase(d,q,hardware,reference,phase);
        if (d->blend_error && !healthy(d,d->blend_error)) return d->blend_error;
        if (d->blend_error && !failed_phase) failed_phase=d->precision_phase;
        d->alpha_qualified=!d->alpha_error; d->blend_qualified=!d->blend_error;
    }
    d->precision_phase=failed_phase;
    return 0;
}
int mach64_3d_fullsize_probe(struct mach64_3d_device *d,struct render3d_service *reference)
{
    if (!d || !d->engine || !d->minimum_qualified || d->qualified || d->lost ||
        d->fullsize_qualified || !d->consumer || !reference ||
        d->mode_width<64 || d->mode_height<64 || d->mode_width>1024 || d->mode_height>1024 ||
        !d->consumer->contexts[0].resources || !reference->contexts[0].resources) return -ENODEV;
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
    for (unsigned i=0;i<R3D_REFS;i++) {
        hw[i]=&d->consumer->contexts[0].resources[i]; ref[i]=&reference->contexts[0].resources[i];
        if (!hw[i]->shadow || !ref[i]->shadow || hw[i]->bytes!=ref[i]->bytes) return -EINVAL;
    }
    if (hw[0]->width!=d->mode_width || hw[0]->height!=d->mode_height) return -EINVAL;
    uint64_t before=d->engine->render3d_writes;
    d->fullsize_testing=true; d->probing=true; d->engine->render3d_qualified=true;
    struct render3d_request *q=(void *)d->consumer->ring[0];
    int e=0;
    for (unsigned phase=0;phase<4 && !e;phase++) {
        if (now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US) { e=-ETIMEDOUT; break; }
        fullsize_scene(q,d->mode_width,d->mode_height,phase);
        e=validate(d,q,hw); if (e) break;
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,ref,&job,&reference->host)) {}
        if (now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US) { e=-ETIMEDOUT; break; }
        e=enter(d); if (e) break;
        uint64_t start=now(d);
        e=execute(d,q,hw);
        d->fullsize_us+=now(d)-start;
        if (e) probe_failure(d,e);
        leave(d);
        if (!e) { d->failed_phase=9; e=fullsize_compare(d,hw,ref); }
        if (!e && now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US) e=-ETIMEDOUT;
    }
    if (!e) {
        d->fullsize_qualified=true; d->failed_phase=0;
        e=precision_probe(d,q,hw,ref);
        if (d->lost && !e) e=-EIO;
    }
    if (!d->lost && d->engine->render3d_writes!=before) {
        uint32_t phase=d->failed_phase;
        int checked=check_2d(d,hw);
        if (checked) e=checked;
        else if (e) d->failed_phase=phase;
    }
    if (!e && now(d)-d->qualification_started>=MACH64_3D_QUALIFY_US) e=-ETIMEDOUT;
    d->fullsize_testing=false; d->probing=false;
    if (e) {
        d->fullsize_qualified=d->perspective_qualified=d->alpha_qualified=d->blend_qualified=false;
        d->engine->render3d_qualified=false;
        return e;
    }
    /* Q4: admission for this boot, device generation and mode only. */
    d->qualified=true; d->failed_phase=0;
    return 0;
}
