/* Wallpaper preparation is bounded foreground work. Paint only copies native
 * cached pixels into the owned compositor band; see wallpaper-cache.md. */
#include "app.h"
#include "webstore.h"

#define PHOTO_W 1672
#define PHOTO_H 941
#define PHOTO_STRIDE 5016
#define PHOTO_BYTES 4720056UL
#define MAX_SCREEN_W 2560
#pragma pack(push,1)
struct band {u16 seg,top,bottom,stride,width;u8 bytes,rs,rp,gs,gp,bs,bp;u16 left;};
#pragma pack(pop)
struct geometry {long x,y,w,h;int left,top,width,height;};
static u16 active,staging,cache,prepared;
static int file=-1;
static u32 loaded,total;
static u16 width,height,stride,new_width,new_height,new_stride;
static int indexed,style;
static char filename[13],active_name[13],path[40];
static u8 header[16],palette[768],indices[256];
static struct app_wallpaper_info request;
static struct band format,cache_format;
static struct geometry shape,cache_shape;
static int format_known,format_w,format_h,dirty=1,cache_w,cache_h,prepare_row,prepare_previous=-1;
static u16 native_stride,cache_stride;
static u8 native_row[MAX_SCREEN_W*4+2];

static u16 word_at(int n) {return header[n]|((u16)header[n+1]<<8);}
static u32 long_at(int n) {return (u32)word_at(n)|((u32)word_at(n+2)<<16);}
static int same_format(const struct band *a,const struct band *b)
{
    return !mem_cmp(&a->bytes,&b->bytes,7);
}
static void cancel_prepare(void)
{
    webstore_free(prepared);prepared=0;prepare_row=0;prepare_previous=-1;
}
static void invalidate(void) {cancel_prepare();dirty=1;}
static void cancel(void)
{
    if(file>=0)dos_close(file);
    file=-1;webstore_free(staging);staging=0;loaded=0;
}
static void rejected(void) {cancel();app_log("[WALLP] rejected",filename);}
static int begin(int segment,int offset)
{
    long bytes;
    cancel();
    far_copy(app_seg(),(u16)&request,segment,offset,sizeof request);
    if(request.bytes!=sizeof request||!request.kind) {
        webstore_free(active);active=0;webstore_free(cache);cache=0;
        invalidate();return 1;
    }
    mem_copy(filename,request.filename,sizeof filename);filename[12]=0;
    if(str_len(filename)!=10||mem_cmp(filename,"WALL",4)||
       filename[4]<'0'||filename[4]>'9'||filename[5]<'0'||filename[5]>'9'||
       mem_cmp(filename+6,".CWP",4)||request.style>WP_TILE) {rejected();return 1;}
    if(active&&!str_cmp(filename,active_name)) {
        style=request.style;invalidate();return 0;
    }
    str_copy(path,"\\SYSTEM\\UI\\");str_cat(path,filename);
    file=dos_open(path,0);if(file<0){rejected();return 1;}
    bytes=dos_seek(file,0,2);
    if(bytes<0||dos_seek(file,0,0)<0||dos_read(file,header,16)!=16) {
        rejected();return 1;
    }
    indexed=0;
    if(!mem_cmp(header,"CWP2",4)&&word_at(4)==PHOTO_W&&word_at(6)==PHOTO_H&&
       word_at(8)==PHOTO_STRIDE&&word_at(10)==1&&long_at(12)==PHOTO_BYTES&&
       bytes==(long)(PHOTO_BYTES+16)) {
        new_width=PHOTO_W;new_height=PHOTO_H;new_stride=PHOTO_STRIDE;
    }else if(!mem_cmp(header,"CWP1",4)&&word_at(4)&&word_at(4)<=256&&
       word_at(6)&&word_at(6)<=256&&word_at(8)==256&&!word_at(10)&&
       long_at(12)==(u32)word_at(4)*word_at(6)&&
       bytes==16L+768L+(long)long_at(12)) {
        indexed=1;new_width=word_at(4);new_height=word_at(6);
        new_stride=(new_width*3u+1u)&~1u;
        if(dos_read(file,palette,sizeof palette)!=sizeof palette){rejected();return 1;}
    }else {rejected();return 1;}
    total=(u32)new_stride*new_height;
    staging=webstore_alloc(total);if(!staging){rejected();return 1;}
    return 0;
}
static int load_step(void)
{
    int i,j,n;u16 chunk;
    if(file<0)return 0;
    for(i=0;i<8&&loaded<total;++i) {
        if(indexed) {
            chunk=new_stride;
            if(dos_read(file,indices,new_width)!=new_width){rejected();return 1;}
            for(j=0;j<new_width;++j)
                mem_copy(webstore_pixels+j*3,palette+indices[j]*3u,3);
            if(new_stride>new_width*3u)webstore_pixels[new_stride-1]=0;
        }else {
            chunk=6144;if(total-loaded<chunk)chunk=(u16)(total-loaded);
            n=dos_read(file,webstore_pixels,chunk);
            if(n!=chunk){rejected();return 1;}
        }
        if(!webstore_write(staging,loaded,webstore_pixels,chunk)){rejected();return 1;}
        loaded+=chunk;
    }
    if(loaded==total) {
        dos_close(file);file=-1;webstore_free(active);active=staging;staging=0;
        width=new_width;height=new_height;stride=new_stride;style=request.style;
        str_copy(active_name,filename);invalidate();
        app_log("[WALLP] loaded",filename);
    }
    return 0;
}
static void geometry(void)
{
    int w=HOST.screen_w,h=HOST.screen_h-61;long dw=width,dh=height,visible;
    if(style==WP_FILL||style==WP_FIT) {
        int by_width=(u32)w*height>=(u32)h*width;
        if(style==WP_FIT)by_width=!by_width;
        if(by_width) {
            dw=w;dh=(long)(((u32)height*w+(style==WP_FILL?width-1:0))/width);
        }else {
            dh=h;dw=(long)(((u32)width*h+(style==WP_FILL?height-1:0))/height);
        }
        if(!dw)dw=1;if(!dh)dh=1;
    }else if(style==WP_STRETCH||style==WP_TILE){dw=w;dh=h;}
    shape.x=(w-dw)/2;shape.y=29+(h-dh)/2;shape.w=dw;shape.h=dh;
    shape.left=shape.x>0?(int)shape.x:0;shape.top=shape.y>29?(int)shape.y:29;
    visible=dw+(shape.x<0?shape.x:0);
    shape.width=visible>w-shape.left?w-shape.left:(int)visible;
    visible=dh+(shape.y<29?shape.y-29:0);
    shape.height=visible>29+h-shape.top?29+h-shape.top:(int)visible;
}
static int prepare_begin(void)
{
    if(!active||!format_known||HOST.screen_w<1||HOST.screen_w>MAX_SCREEN_W||HOST.screen_h<62)return 0;
    geometry();native_stride=(shape.width*format.bytes+1u)&~1u;
    prepared=webstore_alloc((u32)native_stride*shape.height);
    if(!prepared){dirty=0;app_log("[WALLP] cache rejected","XMS allocation");return 0;}
    prepare_row=0;prepare_previous=-1;dirty=0;return 1;
}
static int prepare_step(void)
{
    int batch,dx,sy,sx,step,direct;long rx,ry;
    u8 *dest;u32 value,rem,err;u16 pixel;
    if(!prepared)return 0;
    direct=format.rs==8&&format.gs==8&&format.bs==8&&format.rp==16&&format.gp==8&&format.bp==0;
    for(batch=0;batch<8&&prepare_row<shape.height;++batch,++prepare_row) {
        ry=shape.top+prepare_row-shape.y;
        sy=style==WP_TILE?ry%height:(int)((u32)ry*height/shape.h);
        if(sy!=prepare_previous) {
            if(!webstore_read(active,(u32)sy*stride,webstore_pixels,stride)) {
                cancel_prepare();app_log("[WALLP] cache rejected","source read");return 0;
            }
            prepare_previous=sy;
        }
        rx=shape.left-shape.x;
        value=(u32)rx*width;sx=(int)(value/shape.w);err=value%shape.w;
        step=width/shape.w;rem=width%shape.w;
        if(style==WP_TILE)sx=rx%width;
        dest=native_row;
        for(dx=0;dx<shape.width;++dx) {
            pixel=sx*3u;
            if(direct) {
                *dest++=webstore_pixels[pixel+2];*dest++=webstore_pixels[pixel+1];*dest++=webstore_pixels[pixel];
                if(format.bytes==4)*dest++=0;
            }else {
                value=((u32)(webstore_pixels[pixel]>>(8-format.rs))<<format.rp)|
                      ((u32)(webstore_pixels[pixel+1]>>(8-format.gs))<<format.gp)|
                      ((u32)(webstore_pixels[pixel+2]>>(8-format.bs))<<format.bp);
                *dest++=(u8)value;*dest++=(u8)(value>>8);
                if(format.bytes>=3)*dest++=(u8)(value>>16);
                if(format.bytes==4)*dest++=0;
            }
            if(style==WP_TILE){if(++sx==width)sx=0;}
            else {sx+=step;err+=rem;if(err>=shape.w){err-=shape.w;++sx;}}
        }
        if((u16)(dest-native_row)<native_stride)*dest=0;
        if(!webstore_write(prepared,(u32)prepare_row*native_stride,native_row,native_stride)) {
            cancel_prepare();app_log("[WALLP] cache rejected","destination write");return 0;
        }
    }
    if(prepare_row==shape.height) {
        webstore_free(cache);cache=prepared;prepared=0;
        cache_shape=shape;cache_format=format;cache_stride=native_stride;
        cache_w=HOST.screen_w;cache_h=HOST.screen_h;
        app_log("[WALLP] ready",active_name);return 1;
    }
    return 0;
}
static void paint(void)
{
    struct band band;int left,top,right,bottom,y;u16 skip,count,read_count;u32 source,dest;
    if(!app_band_info(&band)) {
        if(!active)return;
        geometry();
        webstore_image(active,width,height,shape.x,shape.y,shape.w,shape.h,0,29,HOST.screen_w,HOST.screen_h-61);
        return;
    }
    if(band.bytes<2||band.bytes>4||band.rs>8||band.gs>8||band.bs>8)return;
    if(!format_known||!same_format(&format,&band)||
       format_w!=HOST.screen_w||format_h!=HOST.screen_h) {
        format=band;format_known=1;format_w=HOST.screen_w;format_h=HOST.screen_h;invalidate();
    }
    if(!active)return;
    ui_rect(0,29,HOST.screen_w,HOST.screen_h-61,C_TEAL);
    if(!cache||!same_format(&cache_format,&band)||cache_w!=HOST.screen_w||cache_h!=HOST.screen_h)return;
    left=cache_shape.left;top=cache_shape.top;
    right=left+cache_shape.width;bottom=top+cache_shape.height;
    if(left<band.left)left=band.left;if(right>band.left+band.width)right=band.left+band.width;
    if(top<band.top)top=band.top;if(bottom>band.bottom)bottom=band.bottom;
    if(left>=right||top>=bottom)return;
    count=(right-left)*band.bytes;
    for(y=top;y<bottom;++y) {
        source=(u32)(y-cache_shape.top)*cache_stride+(left-cache_shape.left)*band.bytes;
        skip=(u16)(source&1);read_count=(count+skip+1u)&~1u;
        dest=(u32)(y-band.top)*band.stride+(left-band.left)*band.bytes;
        if(read_count>sizeof native_row||dest+count>65536UL)return;
        if(!webstore_read(cache,source&~1UL,native_row,read_count))return;
        far_copy(band.seg,(u16)dest,app_seg(),(u16)(native_row+skip),count);
    }
}
int app_event(int ev,int a,int b,int c)
{
    int r;(void)c;
    if(ev==EV_OPEN)return begin(a,b);
    if(ev==EV_POLL) {
        r=load_step();
        if(dirty)prepare_begin();
        return prepare_step()||r;
    }
    if(ev==EV_PAINT){paint();return 0;}
    if(ev==EV_CLOSE||ev==EV_SUSPEND) {
        cancel();cancel_prepare();webstore_free(active);active=0;
        webstore_free(cache);cache=0;format_known=0;dirty=1;
    }
    return 0;
}
