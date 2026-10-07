/* CiukiOS Image Viewer. WEBIMG.APP decodes PNG/JPEG/GIF in bounded poll
 * steps; this module reads bounded BMP rows and caches all formats as RGB888. */
#include "app.h"
#include "webimg.h"
#include "webmodule.h"
#include "webstore.h"

#define VIEW_ROW_MAX 2048
#define VIEW_RGB_BYTES (VIEW_ROW_MAX * 3)
#define VIEW_BMP_BYTES (VIEW_ROW_MAX * 4)
#define VIEW_BMP_ROWS_PER_POLL 4
#define VIEW_CACHE_MAX (16UL * 1024UL * 1024UL)

static char path[WEBIMG_PATH_BYTES];
static char status[72];
static struct webmodule decoder;
static struct webimg_request request;
static u8 rgb[VIEW_RGB_BYTES];
static u8 row[VIEW_RGB_BYTES+2];
static u8 bmp_row[VIEW_BMP_BYTES];
static u8 bmp_palette[256][3];
static u8 cache_row_seen[256];
static u16 image_w, image_h, bmp_bpp, bmp_colors, image_handle;
static u32 bmp_data, bmp_stride, image_stride, cache_bytes, bmp_file_bytes;
static int bmp_file=-1, is_bmp, bmp_top_down, ready, decoded, source_done, decode_failed, fit_mode=1;
static int draw_x,draw_y,draw_w,draw_h,zoom=1,pan_x,pan_y;
static u16 bmp_row_index, cache_finish_y;
static char siblings[32][WEBIMG_PATH_BYTES];
static int sibling_count,sibling_index,pan_drag,last_mx,last_my;
static int viewport_hover;

static u32 le32(const u8 *p)
{ return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }
static u16 le16(const u8 *p)
{ return (u16)(p[0]|((u16)p[1]<<8)); }

static void layout(void)
{
    int avail_w=HOST.w-28,avail_h=HOST.h-TITLE_H-56;
    if(avail_w<1)avail_w=1;if(avail_h<1)avail_h=1;
    if(fit_mode){
        draw_w=image_w;draw_h=image_h;
        if(draw_w>avail_w){draw_h=(int)((long)draw_h*avail_w/draw_w);draw_w=avail_w;}
        if(draw_h>avail_h){draw_w=(int)((long)draw_w*avail_h/draw_h);draw_h=avail_h;}
        if(draw_w<1)draw_w=1;if(draw_h<1)draw_h=1;
    }else{draw_w=image_w*zoom;draw_h=image_h*zoom;}
    if(draw_w<=avail_w)draw_x=HOST.x+(HOST.w-draw_w)/2;
    else{if(pan_x>draw_w-avail_w)pan_x=draw_w-avail_w;if(pan_x<0)pan_x=0;draw_x=HOST.x+14-pan_x;}
    if(draw_h<=avail_h)draw_y=HOST.y+TITLE_H+44+(avail_h-draw_h)/2;
    else{if(pan_y>draw_h-avail_h)pan_y=draw_h-avail_h;if(pan_y<0)pan_y=0;draw_y=HOST.y+TITLE_H+44-pan_y;}
}

static int cache_open(void)
{
    image_stride=((u32)image_w*3UL+1UL)&~1UL;
    cache_bytes=image_stride*image_h;
    if(!image_w||image_w>VIEW_ROW_MAX||!image_h||image_h>VIEW_ROW_MAX||cache_bytes>VIEW_CACHE_MAX)return 0;
    image_handle=webstore_alloc(cache_bytes);
    if(!image_handle)return 0;
    mem_set(row,0,(u16)image_stride);mem_set(cache_row_seen,0,sizeof cache_row_seen);
    cache_finish_y=0;source_done=decoded=0;return 1;
}

static int image_extension(const char *name)
{
    int n=str_len(name);const char *e;int a,b,c,d;
    if(n<4)return 0;
    if(n>=5&&name[n-5]=='.'){
        e=name+n-4;a=e[0];b=e[1];c=e[2];d=e[3];
        if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;
        if(c>='A'&&c<='Z')c+=32;if(d>='A'&&d<='Z')d+=32;
        if(a=='j'&&b=='p'&&c=='e'&&d=='g')return 1;
    }
    if(name[n-4]!='.')return 0;e=name+n-3;
    a=e[0];b=e[1];c=e[2];
    if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(c>='A'&&c<='Z')c+=32;
    return (a=='b'&&b=='m'&&c=='p')||(a=='p'&&b=='n'&&c=='g')||
           (a=='j'&&b=='p'&&c=='g')||(a=='g'&&b=='i'&&c=='f');
}

static void list_siblings(void)
{
    struct dir_ent e;char dir[WEBIMG_PATH_BYTES],pattern[WEBIMG_PATH_BYTES];int i,n;
    sibling_count=sibling_index=0;if(!path[0])return;
    str_ncopy(dir,path,sizeof dir);n=str_len(dir);
    while(n&&dir[n-1]!='\\'&&dir[n-1]!='/')--n;
    if(!n){dir[0]='\\';dir[1]=0;n=1;}else dir[n]=0;
    str_ncopy(pattern,dir,sizeof pattern);str_cat(pattern,"*.*");
    if(dir_first(pattern,0,&e)<0)return;
    do{
        if(!(e.attr&A_DIR)&&image_extension(e.name)&&sibling_count<32){
            str_ncopy(siblings[sibling_count],dir,sizeof siblings[0]);
            str_cat(siblings[sibling_count],e.name);
            if(str_cmp(siblings[sibling_count],path)==0)sibling_index=sibling_count;
            ++sibling_count;
        }
        i=dir_next(&e);
    }while(i==0);
    dir_close(&e);
}

static int cache_flush_row(void)
{ return 1; }
static int cache_rgb(int sy,int sx,int count,const u8 *src)
{
    if(sy<0||sy>=image_h||sx<0||count<0||sx+count>image_w)return 0;
    if(cache_row_seen[(u16)sy>>3]&(1U<<((u16)sy&7))){
        if(!webstore_read(image_handle,(u32)(u16)sy*image_stride,row,(u16)image_stride))return 0;
    }else mem_set(row,0,(u16)image_stride);
    mem_copy(row+sx*3,src,(u16)(count*3));
    if(!webstore_write(image_handle,(u32)(u16)sy*image_stride,row,(u16)image_stride))return 0;
    cache_row_seen[(u16)sy>>3]|=(u8)(1U<<((u16)sy&7));return 1;
}

static void close_image(void)
{
    if(decoder.segment){webmodule_call(&decoder,EV_CLOSE,&request);webmodule_free(&decoder);}
    if(bmp_file>=0){dos_close(bmp_file);bmp_file=-1;}
    if(image_handle){cache_flush_row();webstore_free(image_handle);image_handle=0;}
    ready=decoded=source_done=decode_failed=is_bmp=0;bmp_row_index=0;
}

static int bmp_open(void)
{
    u8 header[54],ent[4];u32 dib,off,colors,raw_w,raw_h,abs_h,palette_at,pixel_end,i;
    long end;int n;
    bmp_file=dos_open(path,0);if(bmp_file<0)return 0;
    n=dos_read(bmp_file,header,sizeof header);
    if(n!=(int)sizeof header||header[0]!='B'||header[1]!='M')goto bad;
    dib=le32(header+14);off=le32(header+10);raw_w=le32(header+18);raw_h=le32(header+22);
    if(dib<40||le16(header+26)!=1||le32(header+30)!=0||!raw_w||(raw_w&0x80000000UL))goto bad;
    if(raw_h==0||raw_h==0x80000000UL)goto bad;
    if(raw_h&0x80000000UL){abs_h=(~raw_h)+1UL;bmp_top_down=1;}else{abs_h=raw_h;bmp_top_down=0;}
    if(raw_w>VIEW_ROW_MAX||abs_h>VIEW_ROW_MAX)goto bad;
    image_w=(u16)raw_w;image_h=(u16)abs_h;bmp_bpp=le16(header+28);
    if(bmp_bpp!=8&&bmp_bpp!=24&&bmp_bpp!=32)goto bad;
    bmp_stride=(((u32)image_w*bmp_bpp+31UL)/32UL)*4UL;
    if(bmp_stride>sizeof bmp_row)goto bad;
    end=dos_seek(bmp_file,0,2);if(end<0)goto bad;bmp_file_bytes=(u32)end;
    if(dib>bmp_file_bytes-14UL)goto bad;
    palette_at=14UL+dib;
    colors=0;
    if(bmp_bpp==8){
        colors=le32(header+46);if(!colors)colors=256;
        if(colors>256||palette_at>0xFFFFFFFFUL-colors*4UL)goto bad;
        if(off<palette_at+colors*4UL)goto bad;
        if(dos_seek(bmp_file,(long)palette_at,0)<0)goto bad;
        for(i=0;i<colors;++i){
            if(dos_read(bmp_file,ent,4)!=4)goto bad;
            bmp_palette[i][0]=ent[2];bmp_palette[i][1]=ent[1];bmp_palette[i][2]=ent[0];
        }
    }else if(off<palette_at)goto bad;
    if(off>bmp_file_bytes||bmp_stride>0xFFFFFFFFUL/image_h)goto bad;
    pixel_end=off+bmp_stride*image_h;
    if(pixel_end<off||pixel_end>bmp_file_bytes)goto bad;
    bmp_data=off;bmp_colors=(u16)colors;
    if(!cache_open())goto bad;
    if(dos_seek(bmp_file,(long)bmp_data,0)<0)goto bad;
    is_bmp=ready=1;return 1;
bad:
    dos_close(bmp_file);bmp_file=-1;
    if(image_handle){webstore_free(image_handle);image_handle=0;}
    return 0;
}

static int bmp_step(void)
{
    int n,x,bpp;u32 q;u16 dest_y;
    if(bmp_row_index>=image_h)return 1;
    n=dos_read(bmp_file,bmp_row,(int)bmp_stride);
    if((u32)n!=bmp_stride)return 0;
    bpp=bmp_bpp/8;
    for(x=0;x<image_w;++x){
        if(bmp_bpp==8){q=bmp_row[x];if(q>=bmp_colors)return 0;
            rgb[x*3]=bmp_palette[q][0];rgb[x*3+1]=bmp_palette[q][1];rgb[x*3+2]=bmp_palette[q][2];
        }else{rgb[x*3]=bmp_row[x*bpp+2];rgb[x*3+1]=bmp_row[x*bpp+1];rgb[x*3+2]=bmp_row[x*bpp];}
    }
    dest_y=bmp_top_down?bmp_row_index:(u16)(image_h-bmp_row_index-1);
    /* cache_rgb uses row as its merge buffer, so keep the decoded source
     * separate from that buffer while copying a complete BMP scanline. */
    if(!cache_rgb(dest_y,0,image_w,rgb)||!cache_flush_row())return 0;
    ++bmp_row_index;return 1;
}

/* Some static GIFs describe only a sub-rectangle of their logical screen.
 * Row assembly starts at black, so explicitly initialize rows the decoder
 * never touched before presenting the completed cache. */
static int cache_finish_step(void)
{
    u16 y;
    if(!cache_flush_row())return 0;
    while(cache_finish_y<image_h){
        y=cache_finish_y++;
        if(!(cache_row_seen[y>>3]&(1U<<(y&7)))){
            mem_set(row,0,(u16)image_stride);
            if(!webstore_write(image_handle,(u32)y*image_stride,row,(u16)image_stride))return 0;
            cache_row_seen[y>>3]|=(u8)(1U<<(y&7));return 1;
        }
    }
    decoded=1;return 1;
}

static int image_open(const char *new_path)
{
    close_image();str_ncopy(path,new_path,sizeof path);
    if(!path[0]){str_copy(status,"Open an image from Files.");return 0;}
    list_siblings();
    {
        int n=str_len(path),bmp=(n>=4&&path[n-4]=='.');
        int a,b,c;if(bmp){a=path[n-3];b=path[n-2];c=path[n-1];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(c>='A'&&c<='Z')c+=32;bmp=a=='b'&&b=='m'&&c=='p';}
        if(bmp){if(bmp_open()){str_copy(status,"BMP image: decoding");return 1;}
            str_copy(status,"Unsupported or damaged BMP.");return 0;}
    }
    if(!webmodule_load(&decoder,"\\SYSTEM\\APPS\\WEBIMG.APP")){
        str_copy(status,"Image decoder is unavailable.");return 0;}
    mem_set(&request,0,sizeof request);request.abi_version=WEBIMG_ABI_VERSION;
    request.struct_bytes=sizeof request;str_ncopy(request.path,path,sizeof request.path);
    request.output_seg=app_seg();request.output_off=(u16)rgb;request.output_capacity=sizeof rgb;
    if(!webmodule_call(&decoder,EV_OPEN,&request)||request.status!=WEBIMG_STATUS_READY){
        close_image();str_copy(status,"Unsupported or damaged image.");return 0;}
    image_w=request.total_width;image_h=request.total_height;
    if(!cache_open()){close_image();str_copy(status,"Image dimensions are not supported.");return 0;}
    ready=1;str_copy(status,"Decoding image...");return 1;
}

static int decoder_step(void)
{
    u32 need;u16 y;
    if(source_done){
        if(!cache_finish_step())goto failed;
        if(decoded){str_copy(status,is_bmp?"BMP image":"Image ready");layout();
            ui_damage(HOST.x+4,HOST.y+TITLE_H+39,HOST.w-8,HOST.h-TITLE_H-75);return 3;}
        return 2;
    }
    if(is_bmp){
        for(y=0;y<VIEW_BMP_ROWS_PER_POLL&&bmp_row_index<image_h;++y)
            if(!bmp_step())goto failed;
    }
    else{
        if(!webmodule_call(&decoder,EV_POLL,&request))goto failed;
        if(request.status==WEBIMG_STATUS_PIXELS){
            need=(u32)request.w*request.h*3UL;
            if(!request.w||!request.h||request.w>VIEW_ROW_MAX||request.h>VIEW_ROW_MAX||
               (u32)request.x+request.w>image_w||(u32)request.y+request.h>image_h||
               need>sizeof rgb||request.bytes_written!=need)goto failed;
            for(y=0;y<request.h;++y){
                if(!cache_rgb(request.y+y,request.x,request.w,rgb+(u32)y*request.w*3UL))goto failed;
                if(request.x==0&&request.w==image_w&&!cache_flush_row())goto failed;
            }
        }else if(request.status==WEBIMG_STATUS_ERROR)goto failed;
        else if(request.status==WEBIMG_STATUS_DONE)source_done=1;
        else if(request.status!=WEBIMG_STATUS_READY)goto failed;
    }
    if(is_bmp&&bmp_row_index>=image_h)source_done=1;
    if(source_done){if(!cache_finish_step())goto failed;
        if(decoded){str_copy(status,is_bmp?"BMP image":"Image ready");layout();
            ui_damage(HOST.x+4,HOST.y+TITLE_H+39,HOST.w-8,HOST.h-TITLE_H-75);return 3;}}
    return 2;
failed:
    decode_failed=1;ready=0;str_copy(status,"Image decoding failed.");
    ui_damage(HOST.x,HOST.y,HOST.w,HOST.h);return 3;
}

static void paint(void)
{
    const char *zoom_label;
    zoom_label=fit_mode?"Fit":(zoom==1?"100%":(zoom==2?"200%":(zoom==3?"300%":"400%")));
    layout();ui_rect(HOST.x+4,HOST.y+TITLE_H,HOST.w-8,HOST.h-TITLE_H-4,C_FACE);
    ui_button(HOST.x+10,HOST.y+TITLE_H+6,54,25,zoom_label,1);
    ui_button(HOST.x+68,HOST.y+TITLE_H+6,44,25,"Prev",3);
    ui_button(HOST.x+116,HOST.y+TITLE_H+6,44,25,"Next",4);
    ui_button(HOST.x+164,HOST.y+TITLE_H+6,46,25,"Files",2);
    ui_button(HOST.x+214,HOST.y+TITLE_H+6,30,25,"-",5);
    ui_button(HOST.x+248,HOST.y+TITLE_H+6,30,25,"+",6);
    ui_text(HOST.x+284,HOST.y+TITLE_H+12,status,C_INK);
    if(!ready||!decoded){
        ui_bevel(HOST.x+16,HOST.y+TITLE_H+42,HOST.w-32,HOST.h-TITLE_H-54,C_PAPER);
        ui_text(HOST.x+30,HOST.y+TITLE_H+58,path[0]?"The selected image is loading or unavailable.":"Choose an image in Files and open it here.",C_INK);
        return;
    }
    ui_rect(HOST.x+4,HOST.y+TITLE_H+39,HOST.w-8,HOST.h-TITLE_H-75,C_SHADOW);
    webstore_image(image_handle,image_w,image_h,draw_x,draw_y,draw_w,draw_h,
                   HOST.x+6,HOST.y+TITLE_H+38,HOST.w-12,HOST.h-TITLE_H-72);
    ui_text(HOST.x+10,HOST.y+HOST.h-25,"Wheel or +/-: zoom   Drag/arrows: pan   F: fit",C_INK);
}

static void repaint_all(void)
{ ui_damage(HOST.x,HOST.y,HOST.w,HOST.h); }

static int zoom_step(int direction)
{
    if(direction>0){
        if(fit_mode){fit_mode=0;zoom=1;}
        else if(zoom<4)++zoom;
        else return 0;
    }else{
        if(fit_mode)return 0;
        if(zoom>1)--zoom;
        else fit_mode=1;
    }
    return 1;
}

int app_event(int ev,int a,int b,int c)
{
    int sx,sy,changed=0;
    if(ev==EV_OPEN){
        close_image();str_copy(app_title,"CiukiOS Image Viewer");path[0]=0;
        fit_mode=1;zoom=1;pan_x=pan_y=0;pan_drag=0;
        viewport_hover=0;
        if(APP_ARG[0])image_open(APP_ARG);else str_copy(status,"Open an image from Files.");
        return 3;
    }
    if(ev==EV_PAINT){paint();return 0;}
    if(ev==EV_POLL){if(ready&&!decoded)return decoder_step();return 0;}
    if(ev==EV_KEY){
        const char *action=0;
        if((a&255)=='f'||(a&255)=='F'){fit_mode=1;pan_x=pan_y=0;changed=1;action="fit";}
        else if((a&255)=='1'){fit_mode=0;zoom=1;pan_x=pan_y=0;changed=1;action="100%";}
        else if((a&255)=='+'||(a&255)=='='){changed=zoom_step(1);action="zoom in";}
        else if((a&255)=='-'){changed=zoom_step(-1);action="zoom out";}
        else if((a>>8)==0x4B){fit_mode=0;pan_x-=32;changed=1;action="pan left";}
        else if((a>>8)==0x4D){fit_mode=0;pan_x+=32;changed=1;action="pan right";}
        else if((a>>8)==0x48){fit_mode=0;pan_y-=32;changed=1;action="pan up";}
        else if((a>>8)==0x50){fit_mode=0;pan_y+=32;changed=1;action="pan down";}
        if(changed){app_log("[VIEWER] key",action);repaint_all();return 3;}return 0;
    }
    if(ev==EV_ACTION&&a==1){fit_mode=!fit_mode;zoom=1;pan_x=pan_y=0;app_log("[VIEWER] zoom",fit_mode?"fit":"100%");repaint_all();return 3;}
    if(ev==EV_ACTION&&a==2){app_open(WIN_FILES,path);return 0;}
    if(ev==EV_ACTION&&(a==5||a==6)){
        changed=zoom_step(a==6?1:-1);
        if(changed){app_log("[VIEWER] zoom",a==6?"in":"out");repaint_all();return 3;}
        return 0;
    }
    if(ev==EV_ACTION&&(a==3||a==4)&&sibling_count>1){
        sibling_index=(sibling_index+(a==3?sibling_count-1:1))%sibling_count;
        fit_mode=1;zoom=1;pan_x=pan_y=0;image_open(siblings[sibling_index]);repaint_all();return 3;
    }
    if(ev==EV_WHEEL){
        app_log("[VIEWER] wheel",a<0?"negative":"positive");
        changed=a<0?zoom_step(1):zoom_step(-1);
        if(changed){app_log("[VIEWER] zoom",fit_mode?"fit":(zoom==1?"100%":(zoom==2?"200%":(zoom==3?"300%":"400%"))));repaint_all();return 3;}
        return 0;
    }
    if(ev==EV_MOUSE){
        sx=HOST.x+b;sy=HOST.y+TITLE_H+c;
        if(a==MOUSE_HOVER){
            int over=sx>=HOST.x+6&&sx<HOST.x+HOST.w-6&&
                     sy>=HOST.y+TITLE_H+38&&sy<HOST.y+HOST.h-34;
            if(over!=viewport_hover){viewport_hover=over;app_log("[VIEWER] hover",over?"viewport":"leave");}
            return 0;
        }
        if(a==MOUSE_DOWN&&ready&&decoded){layout();
            if(!fit_mode&&(draw_w>HOST.w-28||draw_h>HOST.h-TITLE_H-56)&&
               sx>=HOST.x+14&&sx<HOST.x+HOST.w-14&&sy>=HOST.y+TITLE_H+39&&sy<HOST.y+HOST.h-35){
                pan_drag=1;last_mx=sx;last_my=sy;app_log("[VIEWER] pan","begin");return 0;}}
        if(a==MOUSE_MOVE&&pan_drag){pan_x+=last_mx-sx;pan_y+=last_my-sy;last_mx=sx;last_my=sy;fit_mode=0;layout();repaint_all();return 3;}
        if(a==MOUSE_UP&&pan_drag){pan_drag=0;app_log("[VIEWER] pan","end");}
    }
    if(ev==EV_CLOSE){close_image();return 0;}
    return 0;
}
