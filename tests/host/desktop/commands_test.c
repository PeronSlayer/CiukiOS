/* Production compositor's new wire batches against production preflight and
 * swdib; native colour cursor overlaid by the existing visible-frame oracle. */
#define main desktop_accel_regression_main
#include "accel_test.c"
#undef main
#include <ciuki/display_batch.h>
static unsigned pixel_bpp=32;
static unsigned command_counts[15], released, batches, fenced;
static uint64_t next_fence;
static unsigned hot_x, hot_y, colour_w, colour_h;
static int stretch_enabled;
static uint32_t colour_pixels[64*64];
static int commands_info(int fd, uint32_t generation, struct ciuki_command_info *out)
{
    assert(fd==7 && generation==55);
    *out=(struct ciuki_command_info){.size=sizeof(*out),.version=1,
        .native_commands=(1u<<CIUKI_CMD_WRITE)|(stretch_enabled ? 1u<<CIUKI_CMD_SCALED : 0),
        .capabilities=CIUKI_CAP_BATCH|CIUKI_CAP_MONO|CIUKI_CAP_INVERT|CIUKI_CAP_KEYED|CIUKI_CAP_CURSOR_ARGB|
            (stretch_enabled ? CIUKI_CAP_SCALED|CIUKI_CAP_STRETCH : 0),.cursor_width=64,.cursor_height=64};
    return 0;
}
static int fence_service(int fd, uint64_t id, uint16_t action, uint32_t timeout, struct ciuki_batch_reply *reply)
{
    assert(fd==7 && id && action==CIUKI_FENCE_RELEASE && !timeout);
    *reply=(struct ciuki_batch_reply){.size=64,.version=1,.state=2,.fence=id,.flags=2}; released++; return 0;
}
static int batch_service(int fd, const void *wire, uint32_t bytes, struct ciuki_batch_reply *reply)
{
    assert(fd==7); struct display_batch *b=calloc(1,sizeof(*b)); assert(b);
    b->blob=wire; assert(!display_batch_header(wire,bytes,&b->header));
    b->targets[0]=(struct swdib_target){.identity=scanout,.rights=3,.kind=1,.device={.width=d.width,.height=d.height,
        .pitch=d.width*4,.size=d.width*d.height*4,.mapped=(void *)scanout,.bpp=pixel_bpp,.red_size=pixel_bpp==16 ? 5 : 8,.red_pos=pixel_bpp==16 ? 11 : 16,
        .green_size=pixel_bpp==16 ? 6 : 8,.green_pos=pixel_bpp==16 ? 5 : 8,.blue_size=pixel_bpp==16 ? 5 : 8,.generation=55}};
    if (b->header.target_count==2) {
        b->targets[1]=b->targets[0]; b->targets[1].kind=2; b->targets[1].offset=(uint64_t)d.height*d.width*4;
        b->targets[1].device.height=2; b->targets[1].device.size=d.width*8;
        b->targets[1].device.mapped=(uint8_t *)(scanout+(size_t)d.height*d.width);
    }
    assert(!display_batch_validate(b));
    for (unsigned i=0;i<b->header.record_count;i++) {
        struct swdib_command c; display_batch_command(b,i,&c);
        const struct ciuki_batch_record *r=c.record; command_counts[r->opcode]++;
        if (r->opcode==CIUKI_CMD_CURSOR_ARGB_SET) {
            shown=0; memset(&cursor,0,sizeof(cursor)); cursor.colour=0xffffff; cursor.background=0x101010;
            hot_x=r->origin_x; hot_y=r->origin_y; colour_w=r->dw; colour_h=r->dh;
            for (unsigned y=0;y<r->dh;y++) memcpy(colour_pixels+y*r->dw,c.data+y*r->stride,r->dw*4);
            for (unsigned y=0;y<r->dh;y++) for (unsigned x=0;x<r->dw;x++) {
                const uint8_t *p=c.data+y*r->stride+x*4;
                if (p[3]) { cursor.cursor_mask[y]|=0x80000000u>>x; if (p[0]==255) cursor.cursor_image[y]|=0x80000000u>>x; }
            }
        } else if (r->opcode==CIUKI_CMD_CURSOR_MOVE) { cursor_x=r->dx-(int)hot_x; cursor_y=r->dy-(int)hot_y; }
        else if (r->opcode==CIUKI_CMD_CURSOR_SHOW) shown=!!r->flags;
        else swdib_execute(&c,NULL,NULL);
    }
    *reply=(struct ciuki_batch_reply){.size=64,.version=1,.state=2,.fence=++next_fence,.completed=b->header.record_count,
        .first_uncompleted=UINT32_MAX,.flags=2,.admitted=b->header.record_count,.generation=55};
    batches++; fenced++; free(b); return 0;
}
static void command_redraw(void)
{
    assert(!desk_redraw(&d));
    uint32_t *expected=calloc((size_t)d.width*d.height,4), *visible=malloc((size_t)d.width*d.height*4);
    struct desktop *oracle=malloc(sizeof(d)); assert(expected && visible && oracle);
    *oracle=d; oracle->pixels=expected; oracle->hw_cursor=0;
    desk_composite(oracle,(struct desk_box){0,0,(int)d.width,(int)d.height});
    memcpy(visible,scanout,(size_t)d.width*d.height*4);
    if (pixel_bpp==16) for (unsigned y=0;y<d.height;y++) for (unsigned x=0;x<d.width;x++) {
        uint16_t value; memcpy(&value,(uint8_t *)scanout+(size_t)y*d.width*4+x*2,2);
        visible[(size_t)y*d.width+x]=(((255*((value>>11)&31)+15)/31)<<16)|(((255*((value>>5)&63)+31)/63)<<8)|((255*(value&31)+15)/31);
    }
    if (shown) for (unsigned y=0;y<colour_h;y++) for (unsigned x=0;x<colour_w;x++) {
        int dx=cursor_x+(int)x, dy=cursor_y+(int)y;
        if (dx<0 || dy<0 || dx>=(int)d.width || dy>=(int)d.height) continue;
        uint32_t s=colour_pixels[y*colour_w+x], *under=&visible[(unsigned)dy*d.width+(unsigned)dx];
        unsigned a=s>>24; if (!a) continue;
        unsigned red=((s>>16)&255)+(((((*under)>>16)&255)*(255-a)+127)/255);
        unsigned green=((s>>8)&255)+(((((*under)>>8)&255)*(255-a)+127)/255);
        unsigned blue=(s&255)+(((*under&255)*(255-a)+127)/255);
        *under=red<<16|green<<8|blue;
    }
    if (pixel_bpp==16) for (unsigned i=0;i<d.width*d.height;i++) {
        uint32_t a=expected[i],b=visible[i];
        expected[i]=((a>>8)&0xf800)|((a>>5)&0x07e0)|((a>>3)&31);
        visible[i]=((b>>8)&0xf800)|((b>>5)&0x07e0)|((b>>3)&31);
    }
    assert(!memcmp(expected,visible,(size_t)d.width*d.height*4));
    free(expected); free(visible); free(oracle);
}
static int format_info(int fd,struct ciuki_framebuffer_info *info)
{
    assert(fd==7);
    *info=(struct ciuki_framebuffer_info){.size=sizeof(*info),.width=d.width,.height=d.height,.bpp=pixel_bpp,
        .red_size=pixel_bpp==16 ? 5 : 8,.red_pos=pixel_bpp==16 ? 11 : 16,
        .green_size=pixel_bpp==16 ? 6 : 8,.green_pos=pixel_bpp==16 ? 5 : 8,.blue_size=pixel_bpp==16 ? 5 : 8,.generation=55}; return 0;
}
static int packed_upload(int display,int surface,const struct ciuki_rect *r)
{
    if (pixel_bpp==32) return upload(display,surface,r);
    for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
        uint32_t rgb=d.pixels[(size_t)(r->src_y+y)*d.width+r->src_x+x];
        uint16_t packed=((rgb>>8)&0xf800)|((rgb>>5)&0x7e0)|((rgb>>3)&31);
        memcpy((uint8_t *)scanout+(size_t)(r->dst_y+y)*d.width*4+(r->dst_x+x)*2,&packed,2);
    }
    return 0;
}
#ifndef DESKTOP_COMMANDS_NO_MAIN /* fx-2: occlusion_test.c reuses the harness above */
static void scenario_commands(unsigned bpp)
{
    pixel_bpp=bpp;
    fake_reset(); struct desk_ops ops=fake_ops; ops.present=packed_upload;
    desk_init(&d,&ops,640,480); d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4); scanout=calloc((size_t)d.width*(d.height+2),4); assert(d.pixels && scanout);
    caps=1; vram_first=d.height; vram_end=d.height+2; d.display_info=format_info; d.lfb_bytes_per_pixel=bpp/8; d.accel=service; d.commands=commands_info; d.batch=batch_service; d.fence=fence_service;
    desk_accel_reset(&d,55); assert(d.hw_cursor && d.hw_colour_cursor);
    int w=window_new(10,20,120,80,50,80); command_redraw();
    dirty_client(w); command_redraw();
    /* A two-colour client row must travel through MONO, including odd widths
     * whose uploads require eight-byte payload padding. */
    uint32_t *pixels=(void *)d.clients[w].pixels;
    for (unsigned y=0;y<80;y++) for (unsigned x=0;x<120;x++) pixels[y*120+x]=x&1 ? 0xff0000 : 0x0000ff;
    dirty_client(w); command_redraw();
    for (unsigned y=0;y<80;y++) for (unsigned x=0;x<120;x++) if ((x==0 || x==119 || y==0 || y==79) && !((x+y)&1)) pixels[y*120+x]^=0xffffffu;
    dirty_client(w); command_redraw();
    assert(command_counts[CIUKI_CMD_KEYED] && command_counts[CIUKI_CMD_INVERT]);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=13,.value2=5};
    desk_input(&d,&e); command_redraw();
    assert(command_counts[CIUKI_CMD_MONO] && command_counts[CIUKI_CMD_CURSOR_ARGB_SET] &&
        command_counts[CIUKI_CMD_CURSOR_MOVE] && command_counts[CIUKI_CMD_CURSOR_SHOW]);
    assert(batches && released==fenced && !d.recovering_fence && d.hw_colour_cursor);
    stretch_enabled=1; desk_accel_reset(&d,55);
    for (unsigned y=0;y<80;y++) for (unsigned x=0;x<120;x++) pixels[y*120+x]=(x*317u+(y/4)*4173u)&0xffffff;
    unsigned before_stretch=command_counts[CIUKI_CMD_SCALED];
    dirty_client(w); command_redraw();
    assert(command_counts[CIUKI_CMD_SCALED]>before_stretch);
    stretch_enabled=0;
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d); free(d.pixels); free(scanout); assert(!fake_live());
    printf("desktop command batches %u bpp: %u batches, %u released fences, %u MONO rows, KEYED and INVERT, exact frames and ARGB hotspot/MOVE/SHOW PASS\n",bpp,batches,released,command_counts[5]);
}
int main(void) { scenario_commands(32); scenario_commands(16); return 0; }
#endif
