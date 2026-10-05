/* CiukWeb's isolated, cooperative image decoder CAPP. */
#include "webimg.h"
#include "../../third_party/tjpgd/tjpgd.h"
#pragma disable_message(201)
#include "../../third_party/tjpgd/tjpgd.c"
#pragma enable_message(201)

#ifndef WEBIMG_LOCAL_OFFSET
#define WEBIMG_LOCAL_OFFSET(p) ((u16)(p))
#endif

#define FILE_BUFFER 512
#define IMAGE_MAX_DIM 2048
#define IMAGE_MAX_RGB (12UL * 1024UL * 1024UL)
#define PNG_WINDOW 32768
#define PNG_LINE_MAX 8192
#define PNG_HEAP_BYTES (PNG_WINDOW + PNG_LINE_MAX * 2)

static struct webimg_request req;
static u16 req_seg, req_off;
static int file_handle = -1;
static u8 input_buffer[FILE_BUFFER];
static u16 input_pos, input_count;
static u16 png_heap_seg;
static u16 png_heap_paras;
static u16 png_palette_count, png_alpha_count;
static u8 png_color_type, png_depth, png_channels, png_filter_bpp;
static u16 png_row_bytes, png_row;
static u32 png_crc;
static u32 png_chunk_left;
static u8 png_idat_end, png_final_block;
static u32 png_bits;
static u8 png_nbits, png_deflate_mode;
static u16 png_stored_left, png_match_left, png_match_dist, png_win_pos;
static u32 png_adler_a, png_adler_b;
static u16 png_chunk_type[2];
static u8 png_seen_ihdr, png_have_plte;
struct png_huff { u16 count[16]; u16 symbol[288]; };
static u16 png_line_offset, png_prev_offset;
static u8 png_block_active, png_line_filter, png_heap_ready, png_stream_done;
static u32 png_uncompressed;

static u16 gif_screen_w, gif_screen_h, gif_left, gif_top, gif_w, gif_h;
static u16 gif_row, gif_palette_count, gif_clear, gif_end, gif_next;
static u8 gif_transparent, gif_trans_index, gif_interlace;
static u8 gif_code_size, gif_min_code, gif_old_valid, gif_first;
static u32 gif_bits;
static u8 gif_nbits, gif_sub_left, gif_sub_end;
static u16 gif_old_code, gif_stack_len;


/* Only one format is active per CAPP. Share its peak codec buffers so the
 * loader's conventional-memory image stays within the 64 KiB module budget. */
union codec_storage {
    struct {
        u16 prefix[4096];
        u8 suffix[4096], stack[4096], palette[768], row[6144];
    } gif;
    struct {
        u8 palette[768], alpha[256], header[13], lengths[320];
        struct png_huff lit, dist, cl;
        u8 row[6144];
    } png;
    struct { JDEC dec; u8 pool[4096]; } jpeg;
};
static union codec_storage codec;
#define gif_prefix codec.gif.prefix
#define gif_suffix codec.gif.suffix
#define gif_stack  codec.gif.stack
#define gif_palette codec.gif.palette
#define gif_rowbuf codec.gif.row
#define png_palette codec.png.palette
#define png_alpha codec.png.alpha
#define png_header codec.png.header
#define png_tree_lens codec.png.lengths
#define png_lit_tree codec.png.lit
#define png_dist_tree codec.png.dist
#define png_cl_tree codec.png.cl
#define png_rgbrow codec.png.row
#define jpg codec.jpeg.dec
#define jpg_pool codec.jpeg.pool

static void request_write(void)
{
    far_copy(req_seg, req_off, app_seg(), WEBIMG_LOCAL_OFFSET(&req), sizeof req);
}

static void set_error(u16 code)
{
    req.status = WEBIMG_STATUS_ERROR;
    req.error = code;
    req.bytes_written = 0;
    request_write();
}

static int file_byte(void)
{
    int n;
    if (input_pos >= input_count) {
        n = dos_read(file_handle, input_buffer, FILE_BUFFER);
        if (n <= 0) return -1;
        input_pos = 0;
        input_count = (u16)n;
    }
    return input_buffer[input_pos++];
}

static int file_exact(u8 *dst, u16 count)
{
    int c;
    while (count--) {
        c = file_byte();
        if (c < 0) return 0;
        *dst++ = (u8)c;
    }
    return 1;
}

static int file_skip(u32 count)
{
    while (count--) if (file_byte() < 0) return 0;
    return 1;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u16 be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 be32(const u8 *p)
{ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static int dimensions_ok(u16 w, u16 h)
{
    if (!w || !h || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM) return 0;
    return (u32)w * (u32)h * 3UL <= IMAGE_MAX_RGB;
}

static int rgb_buffer_ok(u32 need)
{
    return req.output_seg && req.output_capacity >= need;
}

static void output_copy(const u8 *src, u16 x, u16 y, u16 w, u16 h)
{
    u32 n = (u32)w * (u32)h * 3UL;
    far_copy(req.output_seg, req.output_off, app_seg(), WEBIMG_LOCAL_OFFSET(src), (u16)n);
    req.x=x; req.y=y; req.w=w; req.h=h;
    req.bytes_written=(u16)n;
    req.status=WEBIMG_STATUS_PIXELS;
    req.error=WEBIMG_ERR_NONE;
    request_write();
}

/* ------------------------------- GIF89a ------------------------------- */
static int gif_subbyte(void)
{
    int n;
    if (gif_sub_end) return -1;
    if (!gif_sub_left) {
        n = file_byte();
        if (n < 0) return -1;
        if (!n) { gif_sub_end=1; return -1; }
        gif_sub_left=(u8)n;
    }
    n=file_byte();
    if(n<0) return -1;
    --gif_sub_left;
    return n;
}

static int gif_code(void)
{
    while (gif_nbits < gif_code_size) {
        int b=gif_subbyte();
        if (b<0) return -1;
        gif_bits |= (u32)(u8)b << gif_nbits;
        gif_nbits += 8;
    }
    {
        int c=(int)(gif_bits & ((1UL << gif_code_size)-1));
        gif_bits >>= gif_code_size;
        gif_nbits -= gif_code_size;
        return c;
    }
}

static int gif_pixel(void)
{
    int code, in_code;
    if (gif_stack_len) return gif_stack[--gif_stack_len];
    for (;;) {
        code=gif_code();
        if(code<0) return -1;
        if(code==gif_clear) {
            gif_code_size=(u8)(gif_min_code+1);
            gif_next=(u16)(gif_clear+2);
            gif_old_valid=0;
            continue;
        }
        if(code==gif_end) return -1;
        if(!gif_old_valid) {
            if(code>=gif_clear) return -1;
            gif_first=(u8)code;
            gif_old_code=(u16)code;
            gif_old_valid=1;
            return code;
        }
        in_code=code;
        if((u16)code>=gif_next) {
            if((u16)code!=gif_next) return -1;
            gif_stack[gif_stack_len++]=gif_first;
            code=gif_old_code;
        }
        while((u16)code>=gif_clear+2) {
            if((u16)code>=gif_next || gif_stack_len>=4095) return -1;
            gif_stack[gif_stack_len++]=gif_suffix[code];
            code=gif_prefix[code];
        }
        if((u16)code>=gif_clear || gif_stack_len>=4095) return -1;
        gif_first=(u8)code;
        gif_stack[gif_stack_len++]=gif_first;
        if(gif_next<4096) {
            gif_prefix[gif_next]=gif_old_code;
            gif_suffix[gif_next]=gif_first;
            ++gif_next;
            if(gif_next==(1U<<gif_code_size) && gif_code_size<12) ++gif_code_size;
        }
        gif_old_code=(u16)in_code;
        return gif_stack[--gif_stack_len];
    }
}

static int gif_palette_read(u16 count)
{
    u16 i;
    if(count>256) return 0;
    for(i=0;i<count*3;i++) {
        int b=file_byte();
        if(b<0) return 0;
        gif_palette[i]=(u8)b;
    }
    gif_palette_count=count;
    return 1;
}

static int gif_skip_blocks(void)
{
    for (;;) {
        int n=file_byte();
        if(n<0) return 0;
        if(!n) return 1;
        if(!file_skip((u32)n)) return 0;
    }
}

static int gif_open(void)
{
    u8 lsd[7], desc[9];
    int c, packed, i;
    if(!file_exact(lsd,7)) return 0;
    gif_screen_w=le16(lsd); gif_screen_h=le16(lsd+2);
    if(!dimensions_ok(gif_screen_w,gif_screen_h)) return 0;
    packed=lsd[4];
    if(packed&0x80) {
        u16 n=(u16)(1U<<((packed&7)+1));
        if(!gif_palette_read(n)) return 0;
    } else gif_palette_count=0;
    gif_transparent=0;
    for(;;) {
        c=file_byte();
        if(c<0) return 0;
        if(c==0x21) {
            int label=file_byte();
            if(label<0) return 0;
            if(label==0xF9) {
                int n=file_byte(), flags, delay0, delay1, ti, end;
                if(n!=4) return 0;
                flags=file_byte(); delay0=file_byte(); delay1=file_byte(); ti=file_byte(); end=file_byte();
                if(flags<0||delay0<0||delay1<0||ti<0||end!=0) return 0;
                gif_transparent=(flags&1)!=0; gif_trans_index=(u8)ti;
            } else if(!gif_skip_blocks()) return 0;
            continue;
        }
        if(c==0x3B) return 0;
        if(c!=0x2C) return 0;
        break;
    }
    if(!file_exact(desc,9)) return 0;
    gif_left=le16(desc); gif_top=le16(desc+2); gif_w=le16(desc+4); gif_h=le16(desc+6);
    if(!gif_w||!gif_h || gif_left+gif_w>gif_screen_w || gif_top+gif_h>gif_screen_h) return 0;
    packed=desc[8]; gif_interlace=(packed&0x40)!=0;
    if(packed&0x80) {
        if(!gif_palette_read((u16)(1U<<((packed&7)+1)))) return 0;
    }
    c=file_byte();
    if(c<2||c>8) return 0;
    gif_min_code=(u8)c; gif_clear=(u16)(1U<<c); gif_end=(u16)(gif_clear+1);
    gif_code_size=(u8)(c+1); gif_next=(u16)(gif_clear+2);
    gif_sub_left=gif_sub_end=gif_nbits=gif_old_valid=0;
    gif_bits=0; gif_stack_len=0; gif_row=0;
    for(i=0;i<gif_clear;i++) { gif_prefix[i]=0; gif_suffix[i]=(u8)i; }
    req.total_width=gif_screen_w; req.total_height=gif_screen_h;
    req.format=WEBIMG_FMT_GIF;
    return rgb_buffer_ok((u32)gif_w*3UL);
}

static u16 gif_y_for_row(u16 row)
{
    static const u8 first[4]={0,4,2,1};
    static const u8 step[4]={8,8,4,2};
    u16 base=0, count;
    u8 p;
    if(!gif_interlace) return row;
    for(p=0;p<4;p++) {
        count=gif_h>first[p]?(u16)((gif_h-first[p]+step[p]-1)/step[p]):0;
        if(row<base+count) return (u16)(first[p]+(row-base)*step[p]);
        base+=count;
    }
    return 0xFFFF;
}

static int gif_poll(void)
{
    u16 x, y;
    if(gif_row>=gif_h) { req.status=WEBIMG_STATUS_DONE; request_write(); return 1; }
    y=gif_y_for_row(gif_row);
    if(y==0xFFFF) return 0;
    for(x=0;x<gif_w;x++) {
        int idx=gif_pixel();
        if(idx<0 || idx>=gif_palette_count) return 0;
        if(gif_transparent && idx==gif_trans_index) {
            gif_rowbuf[x*3]=gif_rowbuf[x*3+1]=gif_rowbuf[x*3+2]=255;
        } else {
            gif_rowbuf[x*3]=gif_palette[idx*3];
            gif_rowbuf[x*3+1]=gif_palette[idx*3+1];
            gif_rowbuf[x*3+2]=gif_palette[idx*3+2];
        }
    }
    ++gif_row;
    output_copy(gif_rowbuf,gif_left,(u16)(gif_top+y),gif_w,1);
    return 1;
}

/* ------------------------------- JPEG --------------------------------- */
static size_t jpg_input(JDEC *jd, uint8_t *dst, size_t n)
{
    int got;
    (void)jd;
    if(!dst) {
        long before, after;
        before=dos_seek(file_handle,0,1);
        if(before<0) return 0;
        after=dos_seek(file_handle,(long)n,1);
        if(after<0 || after-before!=(long)n) return 0;
        return n;
    }
    if(n>32767U) return 0;
    got=dos_read(file_handle,dst,(int)n);
    return got>0?(size_t)got:0;
}

static int jpg_output(JDEC *jd, void *pixels, JRECT *r)
{
    u16 w=(u16)(r->right-r->left+1), h=(u16)(r->bottom-r->top+1);
    u32 n=(u32)w*(u32)h*3UL;
    (void)jd;
    if(n>req.output_capacity || n>65535UL) return 0;
    output_copy((const u8*)pixels,r->left,r->top,w,h);
    return 1;
}

static int jpg_open(void)
{
    JRESULT rc;
    if(dos_seek(file_handle,0,0)<0) return 0;
    input_pos=input_count=0;
    mem_set(jpg_pool,0,sizeof jpg_pool);
    rc=jd_prepare(&jpg,jpg_input,jpg_pool,sizeof jpg_pool,0);
    if(rc!=JDR_OK || !dimensions_ok(jpg.width,jpg.height)) return 0;
    req.total_width=jpg.width; req.total_height=jpg.height;
    req.format=WEBIMG_FMT_JPEG;
    return rgb_buffer_ok(768);
}

static int jpg_poll(void)
{
    JRESULT rc=jd_step(&jpg,jpg_output,0);
    if(rc==JDR_DONE) { req.status=WEBIMG_STATUS_DONE; request_write(); return 1; }
    if(rc!=JDR_OK) return 0;
    return 1;
}

/* -------------------------------- PNG ---------------------------------- */
static u32 png_crc_byte(u32 crc, u8 b)
{
    int i;
    crc^=b;
    for(i=0;i<8;i++) crc=(crc&1)?((crc>>1)^0xEDB88320UL):(crc>>1);
    return crc;
}

static int png_read_chunk_header(void)
{
    u8 h[8];
    if(!file_exact(h,8)) return 0;
    png_chunk_left=be32(h);
    png_chunk_type[0]=be16(h+4); png_chunk_type[1]=be16(h+6);
    png_crc=0xFFFFFFFFUL;
    png_crc=png_crc_byte(png_crc,h[4]); png_crc=png_crc_byte(png_crc,h[5]);
    png_crc=png_crc_byte(png_crc,h[6]); png_crc=png_crc_byte(png_crc,h[7]);
    return 1;
}

static int png_data_byte(u8 *v)
{
    int b;
    if(!png_chunk_left) return 0;
    b=file_byte();
    if(b<0) return 0;
    --png_chunk_left;
    png_crc=png_crc_byte(png_crc,(u8)b);
    *v=(u8)b;
    return 1;
}

static int png_finish_chunk(void)
{
    u8 c[4];
    if(!file_exact(c,4)) return 0;
    return (png_crc^0xFFFFFFFFUL)==be32(c);
}

static int png_skip_chunk_data(void)
{
    u8 b;
    while(png_chunk_left) if(!png_data_byte(&b)) return 0;
    return 1;
}

static int png_idat_byte(void)
{
    for(;;) {
        int b;
        if(png_idat_end) return -1;
        if(png_chunk_left) {
            b=file_byte();
            if(b<0) return -1;
            --png_chunk_left;
            png_crc=png_crc_byte(png_crc,(u8)b);
            return b;
        }
        if(!png_finish_chunk() || !png_read_chunk_header()) return -1;
        if(png_chunk_type[0]!=(u16)(('I'<<8)|'D') || png_chunk_type[1]!=(u16)(('A'<<8)|'T')) {
            png_idat_end=1;
            return -1;
        }
    }
}

static int png_open(void)
{
    static const u8 signature[8]={137,80,78,71,13,10,26,10};
    u8 sig[8];
    u32 len, width, height;
    int i;
    if(!file_exact(sig,8) || mem_cmp(sig,signature,8)) return 0;
    png_seen_ihdr=png_have_plte=0; png_palette_count=png_alpha_count=0;
    png_idat_end=0; png_heap_ready=0;
    for(;;) {
        if(!png_read_chunk_header()) return 0;
        len=png_chunk_left;
        if(png_chunk_type[0]==(u16)(('I'<<8)|'H') && png_chunk_type[1]==(u16)(('D'<<8)|'R')) {
            if(png_seen_ihdr || len!=13 || !file_exact(png_header,13)) return 0;
            png_crc=0xFFFFFFFFUL;
            png_crc=png_crc_byte(png_crc,'I'); png_crc=png_crc_byte(png_crc,'H');
            png_crc=png_crc_byte(png_crc,'D'); png_crc=png_crc_byte(png_crc,'R');
            for(i=0;i<13;i++) png_crc=png_crc_byte(png_crc,png_header[i]);
            png_chunk_left=0;
            if(!png_finish_chunk()) return 0;
            width=be32(png_header); height=be32(png_header+4);
            if(width>IMAGE_MAX_DIM || height>IMAGE_MAX_DIM || !dimensions_ok((u16)width,(u16)height)) return 0;
            req.total_width=(u16)width; req.total_height=(u16)height;
            png_depth=png_header[8]; png_color_type=png_header[9];
            if(png_header[10] || png_header[11] || png_header[12]) return 0;
            if(png_color_type==0) png_channels=1;
            else if(png_color_type==2) png_channels=3;
            else if(png_color_type==3) png_channels=1;
            else if(png_color_type==4) png_channels=2;
            else if(png_color_type==6) png_channels=4;
            else return 0;
            if(png_color_type==3) {
                if(png_depth!=1 && png_depth!=2 && png_depth!=4 && png_depth!=8) return 0;
            } else if(png_color_type==0) {
                if(png_depth!=1 && png_depth!=2 && png_depth!=4 && png_depth!=8 && png_depth!=16) return 0;
            } else if(png_depth!=8 && png_depth!=16) return 0;
            if(png_header[12]!=0) return 0; /* Adam7 rejected explicitly */
            png_seen_ihdr=1;
            continue;
        }
        if(!png_seen_ihdr) return 0;
        if(png_chunk_type[0]==(u16)(('P'<<8)|'L') && png_chunk_type[1]==(u16)(('T'<<8)|'E')) {
            u8 b;
            if(!len || len>768 || len%3) return 0;
            png_palette_count=(u16)(len/3);
            for(i=0;i<(int)len;i++) { if(!png_data_byte(&b)) return 0; png_palette[i]=b; }
            if(!png_finish_chunk()) return 0;
            png_have_plte=1; continue;
        }
        if(png_chunk_type[0]==(u16)(('t'<<8)|'R') && png_chunk_type[1]==(u16)(('N'<<8)|'S')) {
            u8 b;
            if(len>256) return 0;
            png_alpha_count=(u16)len;
            for(i=0;i<(int)len;i++) { if(!png_data_byte(&b)) return 0; png_alpha[i]=b; }
            if(!png_finish_chunk()) return 0;
            continue;
        }
        if(png_chunk_type[0]==(u16)(('I'<<8)|'D') && png_chunk_type[1]==(u16)(('A'<<8)|'T')) {
            if(png_color_type==3 && !png_have_plte) return 0;
            break;
        }
        /* Unknown critical chunks alter image interpretation and are rejected. */
        if((png_chunk_type[0]>>8)>='A' && (png_chunk_type[0]>>8)<='Z') return 0;
        if(!png_skip_chunk_data() || !png_finish_chunk()) return 0;
    }
    if(!rgb_buffer_ok(WEBIMG_RGB_ROW_MAX)) return 0;
    png_row_bytes=(u16)(((u32)req.total_width*png_channels*png_depth+7UL)/8UL);
    if(png_row_bytes>PNG_LINE_MAX) return 0;
    png_filter_bpp=(u8)(((u16)png_channels*png_depth+7)/8);
    if(!png_filter_bpp) png_filter_bpp=1;
    png_heap_paras=(u16)((PNG_HEAP_BYTES+15UL)/16UL);
    png_heap_seg=dos_alloc(png_heap_paras);
    if(!png_heap_seg) return 0;
    png_heap_ready=1;
    png_line_offset=PNG_WINDOW; png_prev_offset=(u16)(PNG_WINDOW+PNG_LINE_MAX);
    for(i=0;i<PNG_LINE_MAX;i++) poke8(png_heap_seg,(u16)(png_prev_offset+i),0);
    png_row=0; png_win_pos=0; png_bits=0; png_nbits=0;
    png_final_block=png_block_active=png_deflate_mode=png_stream_done=0;
    png_match_left=png_match_dist=0; png_adler_a=1; png_adler_b=0; png_uncompressed=0;
    if(png_idat_byte()!=0x78) return 0;
    {
        int cmf=0x78, flg=png_idat_byte();
        if(flg<0 || (((cmf<<8)|flg)%31) || (flg&0x20)) return 0;
    }
    req.format=WEBIMG_FMT_PNG;
    return 1;
}

static int png_get_bits(u8 n, u16 *out)
{
    while(png_nbits<n) {
        int b=png_idat_byte();
        if(b<0) return 0;
        png_bits|=(u32)(u8)b<<png_nbits;
        png_nbits=(u8)(png_nbits+8);
    }
    *out=(u16)(png_bits&((1UL<<n)-1));
    png_bits>>=n; png_nbits=(u8)(png_nbits-n);
    return 1;
}

static int png_tree_build(struct png_huff *t, const u8 *lens, u16 n)
{
    u16 i, code=0, index=0, offs[16];
    int bits;
    mem_set(t,0,sizeof *t);
    for(i=0;i<n;i++) { if(lens[i]>15) return 0; ++t->count[lens[i]]; }
    t->count[0]=0;
    for(bits=1;bits<=15;bits++) {
        code=(u16)((code+t->count[bits-1])<<1);
        if((u32)code+t->count[bits]>(1UL<<bits)) return 0;
        offs[bits]=index; index=(u16)(index+t->count[bits]);
    }
    for(i=0;i<n;i++) if(lens[i]) t->symbol[offs[lens[i]]++]=i;
    return 1;
}

static int png_tree_symbol(struct png_huff *t)
{
    u16 code=0, first=0, index=0, bit;
    int len;
    for(len=1;len<=15;len++) {
        if(!png_get_bits(1,&bit)) return -1;
        code=(u16)((code<<1)|bit);
        if(code>=first && (u16)(code-first)<t->count[len]) return t->symbol[index+(code-first)];
        index=(u16)(index+t->count[len]);
        first=(u16)((first+t->count[len])<<1);
    }
    return -1;
}

static int png_fixed_trees(void)
{
    u16 i;
    for(i=0;i<288;i++) png_tree_lens[i]=(u8)(i<=143?8:i<=255?9:i<=279?7:8);
    if(!png_tree_build(&png_lit_tree,png_tree_lens,288)) return 0;
    for(i=0;i<32;i++) png_tree_lens[i]=5;
    return png_tree_build(&png_dist_tree,png_tree_lens,32);
}

static int png_dynamic_trees(void)
{
    static const u8 order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    u16 hlit, hdist, hclen, v, i, total, sym, rep;
    if(!png_get_bits(5,&v)) return 0; hlit=(u16)(v+257);
    if(!png_get_bits(5,&v)) return 0; hdist=(u16)(v+1);
    if(!png_get_bits(4,&v)) return 0; hclen=(u16)(v+4);
    if(hlit>286 || hdist>32) return 0;
    mem_set(png_tree_lens,0,19);
    for(i=0;i<hclen;i++) { if(!png_get_bits(3,&v)) return 0; png_tree_lens[order[i]]=(u8)v; }
    if(!png_tree_build(&png_cl_tree,png_tree_lens,19)) return 0;
    total=(u16)(hlit+hdist); i=0;
    while(i<total) {
        sym=(u16)png_tree_symbol(&png_cl_tree);
        if(sym<=15) png_tree_lens[i++]=(u8)sym;
        else if(sym==16) {
            if(!i || !png_get_bits(2,&v)) return 0;
            rep=(u16)(v+3); if(i+rep>total) return 0;
            while(rep--) { png_tree_lens[i]=png_tree_lens[i-1]; ++i; }
        } else if(sym==17 || sym==18) {
            if(!png_get_bits(sym==17?3:7,&v)) return 0;
            rep=(u16)(v+(sym==17?3:11)); if(i+rep>total) return 0;
            while(rep--) png_tree_lens[i++]=0;
        } else return 0;
    }
    if(!png_tree_lens[256]) return 0;
    if(!png_tree_build(&png_lit_tree,png_tree_lens,hlit)) return 0;
    if(!png_tree_build(&png_dist_tree,png_tree_lens+hlit,hdist)) return 0;
    return 1;
}

static int png_start_block(void)
{
    u16 bfinal, btype, len, nlen;
    if(!png_get_bits(1,&bfinal)||!png_get_bits(2,&btype)) return 0;
    png_final_block=(u8)bfinal;
    if(btype==0) {
        png_bits=0; png_nbits=0;
        if(!png_get_bits(16,&len)||!png_get_bits(16,&nlen) || (u16)(len^0xFFFF)!=nlen) return 0;
        png_stored_left=len; png_deflate_mode=0; png_block_active=1;
        return 1;
    }
    if(btype==1) { if(!png_fixed_trees()) return 0; }
    else if(btype==2) { if(!png_dynamic_trees()) return 0; }
    else return 0;
    png_deflate_mode=(u8)btype; png_block_active=1;
    return 1;
}

static int png_length_symbol(int sym, u16 *value)
{
    static const u16 base[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const u8 extra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    u16 v;
    if(sym<257 || sym>285) return 0;
    *value=base[sym-257];
    if(extra[sym-257]) { if(!png_get_bits(extra[sym-257],&v)) return 0; *value=(u16)(*value+v); }
    return 1;
}

static int png_distance_symbol(int sym, u16 *value)
{
    static const u16 base[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const u8 extra[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    u16 v;
    if(sym<0 || sym>29) return 0;
    *value=base[sym];
    if(extra[sym]) { if(!png_get_bits(extra[sym],&v)) return 0; *value=(u16)(*value+v); }
    return 1;
}

static int png_raw_output(u8 *out)
{
    int sym, ds;
    u16 v;
    for(;;) {
        if(png_match_left) {
            u16 at=(u16)((png_win_pos-png_match_dist)&32767);
            *out=peek8(png_heap_seg,at); --png_match_left;
            break;
        }
        if(!png_block_active) {
            if(png_stream_done) return -1;
            if(!png_start_block()) return -2;
        }
        if(png_deflate_mode==0) {
            if(!png_stored_left) {
                png_block_active=0;
                if(png_final_block) png_stream_done=1;
                continue;
            }
            if(!png_get_bits(8,&v)) return -2;
            --png_stored_left; *out=(u8)v; break;
        }
        sym=png_tree_symbol(&png_lit_tree);
        if(sym<0) return -2;
        if(sym<256) { *out=(u8)sym; break; }
        if(sym==256) {
            png_block_active=0;
            if(png_final_block) png_stream_done=1;
            continue;
        }
        if(!png_length_symbol(sym,&png_match_left)) return -2;
        ds=png_tree_symbol(&png_dist_tree);
        if(ds<0 || !png_distance_symbol(ds,&png_match_dist) || png_match_dist>32768 || png_match_dist>png_uncompressed) return -2;
    }
    poke8(png_heap_seg,png_win_pos,*out);
    png_win_pos=(u16)((png_win_pos+1)&32767);
    ++png_uncompressed;
    png_adler_a+=*out; if(png_adler_a>=65521UL) png_adler_a-=65521UL;
    png_adler_b+=png_adler_a; png_adler_b%=65521UL;
    return 1;
}

static u8 png_line_byte(u16 off, u16 i) { return peek8(png_heap_seg,(u16)(off+i)); }
static void png_line_set(u16 off, u16 i, u8 v) { poke8(png_heap_seg,(u16)(off+i),v); }

static u8 png_paeth(u8 a, u8 b, u8 c)
{
    int p=(int)a+(int)b-(int)c, pa=p-a, pb=p-b, pc=p-c;
    if(pa<0) pa=-pa; if(pb<0) pb=-pb; if(pc<0) pc=-pc;
    if(pa<=pb && pa<=pc) return a;
    if(pb<=pc) return b;
    return c;
}

static int png_filter_row(void)
{
    u16 i;
    int v;
    v=png_raw_output(&png_line_filter);
    if(v!=1 || png_line_filter>4) return 0;
    for(i=0;i<png_row_bytes;i++) {
        u8 raw, a=0, b=png_line_byte(png_prev_offset,i), c=0;
        v=png_raw_output(&raw);
        if(v!=1) return 0;
        if(i>=png_filter_bpp) a=png_line_byte(png_line_offset,(u16)(i-png_filter_bpp));
        if(i>=png_filter_bpp) c=png_line_byte(png_prev_offset,(u16)(i-png_filter_bpp));
        if(png_line_filter==1) raw=(u8)(raw+a);
        else if(png_line_filter==2) raw=(u8)(raw+b);
        else if(png_line_filter==3) raw=(u8)(raw+(((u16)a+b)>>1));
        else if(png_line_filter==4) raw=(u8)(raw+png_paeth(a,b,c));
        png_line_set(png_line_offset,i,raw);
    }
    return 1;
}

static u16 png_sample(u16 x, u8 channel)
{
    u32 sample;
    if(png_depth<8) {
        u32 bit=(u32)x*png_depth;
        u8 value=png_line_byte(png_line_offset,(u16)(bit>>3));
        u8 shift=(u8)(8-png_depth-(bit&7));
        return (u16)((value>>shift)&((1U<<png_depth)-1));
    }
    sample=((u32)x*png_channels+channel)*(png_depth==16?2UL:1UL);
    if(png_depth==16) return (u16)(((u16)png_line_byte(png_line_offset,(u16)sample)<<8)|png_line_byte(png_line_offset,(u16)(sample+1)));
    return png_line_byte(png_line_offset,(u16)sample);
}

static u8 png_to_byte(u16 v)
{
    if(png_depth==16) return (u8)(v>>8);
    if(png_depth<8) return (u8)((v*255U)/((1U<<png_depth)-1));
    return (u8)v;
}

static u8 png_over_white(u8 c, u8 a)
{ return (u8)(((u16)c*a+(u16)255*(255-a)+127)/255); }

static int png_rgb_pixel(u16 x, u8 *r, u8 *g, u8 *b)
{
    u16 v0,v1,v2,v3,key;
    u8 alpha=255;
    if(png_color_type==3) {
        u16 index=png_sample(x,0);
        if(index>=png_palette_count) return 0;
        *r=png_palette[index*3]; *g=png_palette[index*3+1]; *b=png_palette[index*3+2];
        if(index<png_alpha_count) alpha=png_alpha[index];
    } else if(png_color_type==0) {
        v0=png_sample(x,0);
        *r=*g=*b=png_to_byte(v0);
        if(png_alpha_count==2) {
            key=be16(png_alpha);
            if(v0==key) alpha=0;
        }
    } else if(png_color_type==2) {
        v0=png_sample(x,0); v1=png_sample(x,1); v2=png_sample(x,2);
        *r=png_to_byte(v0); *g=png_to_byte(v1); *b=png_to_byte(v2);
        if(png_alpha_count==6 && v0==be16(png_alpha) && v1==be16(png_alpha+2) && v2==be16(png_alpha+4)) alpha=0;
    } else if(png_color_type==4) {
        v0=png_sample(x,0); v1=png_sample(x,1);
        *r=*g=*b=png_to_byte(v0); alpha=png_to_byte(v1);
    } else {
        v0=png_sample(x,0); v1=png_sample(x,1); v2=png_sample(x,2); v3=png_sample(x,3);
        *r=png_to_byte(v0); *g=png_to_byte(v1); *b=png_to_byte(v2); alpha=png_to_byte(v3);
    }
    if(alpha!=255) { *r=png_over_white(*r,alpha); *g=png_over_white(*g,alpha); *b=png_over_white(*b,alpha); }
    return 1;
}

static int png_finish_stream(void)
{
    u8 a[4];
    u32 expected, actual;
    int b, tail;
    if(!png_stream_done) {
        u8 extra;
        tail=png_raw_output(&extra);
        if(tail!=-1 || !png_stream_done) return 0; /* reject extra decoded pixels */
    }
    /* The zlib trailer starts at the next byte boundary after final DEFLATE. */
    png_bits=0; png_nbits=0;
    for(b=0;b<4;b++) { int c=png_idat_byte(); if(c<0) return 0; a[b]=(u8)c; }
    expected=be32(a); actual=(png_adler_b<<16)|png_adler_a;
    if(expected!=actual) return 0;
    /* Consume all remaining IDAT bytes and validate every IDAT CRC. */
    while(!png_idat_end) {
        b=png_idat_byte();
        if(b<0 && !png_idat_end) return 0;
    }
    return 1;
}

static int png_poll(void)
{
    u16 x;
    u8 r,g,b;
    if(png_row>=req.total_height) {
        if(!png_finish_stream()) return 0;
        req.status=WEBIMG_STATUS_DONE; req.bytes_written=0; request_write();
        return 1;
    }
    if(!png_filter_row()) return 0;
    for(x=0;x<req.total_width;x++) {
        if(!png_rgb_pixel(x,&r,&g,&b)) return 0;
        png_rgbrow[x*3]=r; png_rgbrow[x*3+1]=g; png_rgbrow[x*3+2]=b;
    }
    {
        u16 t=png_prev_offset;
        png_prev_offset=png_line_offset;
        png_line_offset=t;
    }
    output_copy(png_rgbrow,0,png_row,req.total_width,1);
    ++png_row;
    return 1;
}

/* --------------------------- CAPP event ABI --------------------------- */
static void release_image(void)
{
    if(file_handle>=0) { dos_close(file_handle); file_handle=-1; }
    if(png_heap_ready && png_heap_seg) { dos_free(png_heap_seg); png_heap_seg=0; png_heap_ready=0; }
}

int app_event(int ev, int a, int b, int c)
{
    u8 sig[8];
    int ok=0;
    long file_size;
    (void)c;
    if(ev==EV_OPEN) {
        release_image();
        req_seg=(u16)a; req_off=(u16)b;
        far_copy(app_seg(),WEBIMG_LOCAL_OFFSET(&req),req_seg,req_off,sizeof req);
        req.status=WEBIMG_STATUS_IDLE; req.error=WEBIMG_ERR_NONE;
        req.format=WEBIMG_FMT_UNKNOWN; req.total_width=req.total_height=0;
        req.x=req.y=req.w=req.h=req.bytes_written=0;
        if(req.abi_version!=WEBIMG_ABI_VERSION || req.struct_bytes<sizeof req ||
           req.output_capacity<WEBIMG_RGB_ROW_MAX || !req.output_seg || !req.path[0]) {
            set_error(WEBIMG_ERR_OUTPUT); return 1;
        }
        req.path[WEBIMG_PATH_BYTES-1]=0;
        file_handle=dos_open(req.path,0);
        if(file_handle<0) { set_error(WEBIMG_ERR_IO); return 1; }
        file_size=dos_seek(file_handle,0,2);
        if(file_size<8 || (u32)file_size>IMAGE_MAX_RGB) {
            set_error(file_size<0?WEBIMG_ERR_IO:WEBIMG_ERR_LIMIT); release_image(); return 1;
        }
        if(dos_seek(file_handle,0,0)<0) { set_error(WEBIMG_ERR_IO); release_image(); return 1; }
        input_pos=input_count=0;
        if(!file_exact(sig,8) || dos_seek(file_handle,0,0)<0) { set_error(WEBIMG_ERR_IO); release_image(); return 1; }
        input_pos=input_count=0;
        if(!mem_cmp(sig,"GIF87a",6) || !mem_cmp(sig,"GIF89a",6)) {
            req.format=WEBIMG_FMT_GIF;
            /* gif_open starts at the logical screen descriptor, after the
             * six-byte GIF signature already inspected above. */
            ok=file_skip(6) && gif_open();
        }
        else if(!mem_cmp(sig,"\x89PNG\r\n\x1a\n",8)) { req.format=WEBIMG_FMT_PNG; ok=png_open(); }
        else if(sig[0]==0xFF && sig[1]==0xD8) { req.format=WEBIMG_FMT_JPEG; ok=jpg_open(); }
        if(!ok) {
            set_error(req.format==WEBIMG_FMT_UNKNOWN?WEBIMG_ERR_FORMAT:WEBIMG_ERR_UNSUPPORTED);
            release_image(); return 1;
        }
        req.status=WEBIMG_STATUS_READY; req.error=WEBIMG_ERR_NONE; request_write();
        return 1;
    }
    if(ev==EV_POLL) {
        if(req.status==WEBIMG_STATUS_ERROR || req.status==WEBIMG_STATUS_DONE || req.status==WEBIMG_STATUS_IDLE) return 0;
        req.bytes_written=0;
        if(req.format==WEBIMG_FMT_GIF) ok=gif_poll();
        else if(req.format==WEBIMG_FMT_PNG) ok=png_poll();
        else if(req.format==WEBIMG_FMT_JPEG) ok=jpg_poll();
        if(!ok) set_error(WEBIMG_ERR_FORMAT);
        return 1;
    }
    if(ev==EV_CLOSE) { release_image(); req.status=WEBIMG_STATUS_IDLE; return 1; }
    return 0;
}
