/* CiukWeb: cooperative native HTML layout and graphical resource loading. */
#include "app.h"
#include "webio.h"
#include "webmodule.h"
#include "webstore.h"
#include "webimg.h"
#include "webstyle.h"

#define PAGE_PATH "C:\\NET\\CIUKWEB.HTM"
#define IMAGE_PATH "C:\\NET\\CWIMAGE.BIN"
#define CSS_PATH "C:\\NET\\CWSTYLE.CSS"
#define RENDER_PATH "C:\\NET\\CWRENDER.HTM"
#define NODE_MAX 6144
#define IMAGE_MAX 12
#define LINK_MAX 48
#define FIELD_MAX 24
#define FORM_MAX 6
#define PAGE_HEIGHT 32000
#define CACHE_MAX 16

struct node {u16 next,x,y,w,style;u8 link,kind,bg;char text[68];};
struct browser_css_frame {char tag[20];short old_left,old_right,bottom_gap;u16 old_bg;int old_style;u8 old_hidden,old_link,block;};
struct picture {char url[128],alt[48];u16 handle,width,height,x,y,w,h;u8 state,link;};
struct web_field {char name[40],value[80];u16 x,y,w;u8 type,form,checked;};
struct form {char action[128];u8 post;};
static struct picture images[IMAGE_MAX];
static struct web_field fields[FIELD_MAX];
static struct form forms[FORM_MAX];
static char links[LINK_MAX][128],history[8][128];
static char cache_urls[CACHE_MAX][128];
static u8 cache_valid[CACHE_MAX];
static int cache_head;
static char current_image_path[20]="C:\\NET\\CIMG00.DAT";
static char url[128],page_url[128],resource_url[128],base_url[128],message[96],title[64];
static char tag[512],word[68],input[512];
#define pixels webstore_pixels
static u16 __far *bucket;
static u16 bucket_seg,nodes,node_handle,codec_seg,codec_entry;
static struct webimg_request image_request;
static struct webmodule style_module;
static struct webstyle_request style_request;
static struct webstyle_computed current_css;
static struct browser_css_frame browser_css[64];
static char css_feed_buffer[512];
static char css_inline_pending[8];
static struct node item;
static int image_count,field_count,form_count,link_count,history_count,history_at;
static int loading,transfer_kind,source=-1,current_image,redirects;
static int input_at,input_len,parse_state,tag_len,quote,word_len,skip,skip_match;
static int css_scan_state,css_scan_quote,css_scan_skip_match,css_scan_comment;
static int css_feed_file=-1,css_link_count,css_hidden,css_depth,css_inline_end,css_inline_match,css_inline_pending_len,css_feed_used;
static int css_limited,css_top_gap,css_close_gap,css_close_block;
static int parse_title,title_len,caret,replace_address,focused=1,field_focus=-1;
static int scroll,doc_height,cached_width,lx,ly,line_height,left_margin,right_margin;
static int style=C_INK,active_link,current_form=-1,preformatted,table,table_x,table_y,table_bottom,table_column,table_width;
static int body_started,relayout,partial,last_space,table_columns,table_scans,current_bg=255,in_center;
static u32 document_bytes,cache_bytes;
static struct {u16 index;char url[128];} scripts[16];
static int js_cache,js_warning,js_job,js_at,js_count;
static int closing,worker_stopping;
static struct cww_header js_reply;
static char script_path[32];
static void js_begin(void);
static void js_next(void);
static const char *document_path(void)
{return page_url[1]==':'?page_url:js_cache?RENDER_PATH:PAGE_PATH;}

static int lower(int ch){return ch>='A'&&ch<='Z'?ch+32:ch;}
static int starts(const char *a,const char *b)
{while(*b)if(lower(*a++)!=lower(*b++))return 0;return 1;}
static int space(int c){return c==' '||c=='\t'||c=='\n'||c=='\r';}
static int minimum(int a,int b){return a<b?a:b;}
static int css_palette(u32 rgb)
{
    static const u32 colors[16]={0x000000UL,0x000080UL,0x008000UL,0x008080UL,0x800000UL,0x800080UL,0x805000UL,0xC0C0C0UL,
        0x808080UL,0x0000FFUL,0x00FFFFUL,0x80FFFFUL,0xFF8080UL,0xD0A0FFUL,0xFFFF00UL,0xFFFFFFUL};
    long best=0x7FFFFFFFL,dr,dg,db,d;int i,answer=C_INK;
    if(rgb==0xFFFFFFFFUL)return 255;
    for(i=0;i<16;++i){dr=(long)((rgb>>16)&255)-(long)((colors[i]>>16)&255);
        dg=(long)((rgb>>8)&255)-(long)((colors[i]>>8)&255);db=(long)(rgb&255)-(long)(colors[i]&255);
        d=dr*dr+dg*dg+db*db;if(d<best){best=d;answer=i;}}
    return answer;
}
static int css_module_open(void)
{
    if(!style_module.segment){
        if(!webmodule_load(&style_module,"\\SYSTEM\\APPS\\WEBSTYLE.APP"))return 0;
        mem_set(&style_request,0,sizeof style_request);style_request.abi_version=WEBSTYLE_ABI_VERSION;
        style_request.struct_bytes=sizeof style_request;webmodule_call(&style_module,EV_OPEN,&style_request);
    }
    return 1;
}
static int css_module_op(u16 op)
{
    if(!css_module_open())return WEBSTYLE_STATUS_ERROR;
    style_request.abi_version=WEBSTYLE_ABI_VERSION;style_request.struct_bytes=sizeof style_request;
    style_request.op=op;webmodule_call(&style_module,EV_POLL,&style_request);return style_request.status;
}
static int css_module_feed(const char *bytes,u16 n)
{
    if(!n)return WEBSTYLE_STATUS_OK;
    if(n>WEBSTYLE_CHUNK_MAX)return WEBSTYLE_STATUS_LIMIT;
    mem_set(&style_request,0,sizeof style_request);style_request.abi_version=WEBSTYLE_ABI_VERSION;
    style_request.struct_bytes=sizeof style_request;style_request.op=WEBSTYLE_OP_FEED;style_request.css_bytes=n;
    mem_move(style_request.css,bytes,n);webmodule_call(&style_module,EV_POLL,&style_request);return style_request.status;
}
static void error(const char *s){str_ncopy(message,s,sizeof message);app_log("[CIUKWEB] error",message);}
static void codec_call(int event)
{
    struct regs r;if(!codec_seg)return;
    mem_set(&r,0,sizeof r);r.ax=event;r.dx=app_seg();r.bx=(u16)&image_request;
    r.ds=r.es=app_seg();far_regs(codec_seg,codec_entry,&r);
}
static int codec_load(void)
{
    u8 header[22];u16 paras;int h,n;u8 __far *p;u32 size,i;long file_size;
    if(codec_seg)return 1;
    h=dos_open("\\SYSTEM\\APPS\\WEBIMG.APP",0);if(h<0)return 0;
    n=dos_read(h,header,sizeof header);
    if(n!=sizeof header||mem_cmp(header,"CAPP",4)){dos_close(h);return 0;}
    paras=header[6]|((u16)header[7]<<8);
    if(paras<32||paras>4096){dos_close(h);return 0;}
    file_size=dos_seek(h,0,2);
    if(file_size<22||file_size>0xFE00L||(u32)file_size>((u32)paras<<4)-0x100UL){dos_close(h);return 0;}
    codec_seg=dos_alloc(paras);if(!codec_seg){dos_close(h);return 0;}
    size=(u32)paras<<4;p=(u8 __far *)((u32)codec_seg<<16);
    for(i=0;i<size;++i)p[(u16)i]=0;
    dos_seek(h,0,0);n=dos_read_far(h,codec_seg,0x100,0x7F00);
    if(n==0x7F00){int more=dos_read_far(h,codec_seg,0x8000,0x7F00);if(more<0)n=-1;}
    dos_close(h);
    if(n<22){dos_free(codec_seg);codec_seg=0;return 0;}
    codec_entry=header[12]|((u16)header[13]<<8);return 1;
}
static void get_cache_path(char *out,int slot)
{
    str_copy(out,"C:\\NET\\CIMG00.DAT");
    out[11]=(char)('0'+slot/10);out[12]=(char)('0'+slot%10);
}
static void image_release(struct picture *p)
{
    if(p->handle){
        int i;u32 bytes=(u32)((p->width*3u+1u)&~1u)*p->height;
        for(i=0;i<image_count;++i)if(&images[i]!=p&&images[i].handle==p->handle)break;
        if(i==image_count){
            webstore_free(p->handle);
            cache_bytes=bytes<=cache_bytes?cache_bytes-bytes:0;
        }
        p->handle=0;
    }
}
static void stop(void)
{
    int i;
    webio_cancel();
    if(js_job||loading==1){worker_stopping=!webio_close();js_job=0;}
    if(source>=0){dos_close(source);source=-1;}
    if(css_feed_file>=0){dos_close(css_feed_file);css_feed_file=-1;}
    if(style_module.segment)css_module_op(WEBSTYLE_OP_RESET);
    if(loading==3)codec_call(EV_CLOSE);
    for(i=0;i<image_count;++i)if(images[i].state==0||images[i].state==3){
        image_release(&images[i]);images[i].state=2;
    }
    loading=0;css_scan_state=css_scan_comment=0;css_inline_match=0;in_center=0;
}
static void release_page(void)
{
    int i,j;stop();
    for(i=0;i<image_count;++i){
        if(images[i].handle){
            for(j=i+1;j<image_count;++j)if(images[j].handle==images[i].handle)images[j].handle=0;
            webstore_free(images[i].handle);images[i].handle=0;
        }
    }
    image_count=field_count=form_count=link_count=nodes=0;cache_bytes=0;
    mem_set(images,0,sizeof images);if(bucket)for(i=0;i<2001;++i)bucket[i]=0;doc_height=scroll=0;
}
static void download_start(int kind,const char *target)
{
    transfer_kind=kind;str_ncopy(resource_url,target,sizeof resource_url);
    if(webio_start_file(resource_url,kind==1?PAGE_PATH:kind==3?CSS_PATH:kind==4?script_path:current_image_path,
                        kind==1?1048576UL:kind==3||kind==4?65536UL:4194304UL)!=WEBNET_PENDING){
        if(kind==3){loading=6;return;}if(kind==4){js_warning=1;++js_at;loading=11;return;}error(webio_error());loading=0;return;
    }
    loading=1;str_copy(message,kind==1?"Loading page...":"Loading page images...");
}
static void navigate(const char *target,int remember)
{
    char next[128];int i;str_ncopy(next,target,sizeof next);
    if(!starts(next,"http://")&&!starts(next,"https://")&&next[1]!=':'){
        if(str_len(next)>119){error("The address is too long.");return;}
        str_copy(next,"http://");str_cat(next,target);
    }
    if(remember||!history_count){
        if(history_count)history_count=history_at+1;
        if(history_count==8){for(i=1;i<8;++i)str_copy(history[i-1],history[i]);--history_count;}
        str_copy(history[history_count++],next);history_at=history_count-1;
    }
    release_page();str_copy(url,next);caret=str_len(url);replace_address=0;redirects=0;title[0]=0;field_focus=-1;
    js_cache=js_warning=0;
    if(worker_stopping){loading=12;str_copy(message,"Finishing the previous request...");return;}
    if(next[1]==':'){str_copy(page_url,next);str_copy(base_url,next);relayout=0;loading=4;return;}
    download_start(1,next);app_log("[CIUKWEB] fetch",next);
}
static int attr(const char *name,char *out,int size)
{
    int i=0,n,start,end,q,len=str_len(name);out[0]=0;
    while(tag[i]&&!space(tag[i]))++i;
    while(tag[i]){
        while(space(tag[i])||tag[i]=='/')++i;
        start=i;while(tag[i]&&!space(tag[i])&&tag[i]!='='&&tag[i]!='/')++i;
        end=i;while(space(tag[i]))++i;q=0;n=0;
        if(tag[i]=='='){
            ++i;while(space(tag[i]))++i;if(tag[i]=='\''||tag[i]=='"')q=tag[i++];
            while(tag[i]&&(q?tag[i]!=q:!space(tag[i]))){if(n<size-1)out[n++]=tag[i];++i;}
            if(q&&tag[i]==q)++i;
        }
        out[n]=0;
        if(end-start==len&&!str_nicmp(tag+start,name,len)){
            int k;for(k=0;out[k];++k)if(starts(out+k,"&amp;"))mem_move(out+k+1,out+k+5,str_len(out+k+5)+1);
            return 1;
        }
        if(end==start&&tag[i])++i;
    }
    out[0]=0;return 0;
}
static void parse_begin(void);
static void download_start(int kind,const char *target);
static void image_begin(const char *path);
static void css_scan_begin(void);
static void css_scan_step(void);
static void css_feed_step(void);
static int css_flush(void)
{
    int rc=WEBSTYLE_STATUS_OK;
    if(css_feed_used){rc=css_module_feed(css_feed_buffer,(u16)css_feed_used);css_feed_used=0;
        if(rc!=WEBSTYLE_STATUS_OK)css_limited=1;}
    return rc;
}
static void css_inline_emit(char c)
{
    css_feed_buffer[css_feed_used++]=c;
    if(css_feed_used==sizeof css_feed_buffer)css_flush();
}
static int css_rel_stylesheet(const char *s)
{
    int i=0;while(s[i]){int start;while(space(s[i]))++i;start=i;while(s[i]&&!space(s[i]))++i;
        if(i-start==10&&starts(s+start,"stylesheet"))return 1;}
    return 0;
}
static int css_start_link(void)
{
    char rel[128],href[128],resolved[128];
    if(css_link_count>=8||!attr("rel",rel,sizeof rel)||!css_rel_stylesheet(rel)||!attr("href",href,sizeof href))return 0;
    ++css_link_count;
    if(!webio_resolve(base_url,href,resolved,sizeof resolved))return 0;
    if(css_flush()!=WEBSTYLE_STATUS_OK)return 0;
    download_start(3,resolved);
    return loading==1;
}
static void css_scan_tag_done(void)
{
    char name[20],value[128];int i=0,j=0,closing=tag[0]=='/';
    css_scan_state=0;
    if(closing)++i;while(tag[i]&&!space(tag[i])&&tag[i]!='/'&&j<19)name[j++]=(char)lower(tag[i++]);name[j]=0;
    if(!str_cmp(name,"style")){css_scan_state=closing?0:3;css_inline_match=css_inline_pending_len=css_inline_end=0;return;}
    if(!str_cmp(name,"script")){css_scan_state=closing?0:4;css_scan_skip_match=0;return;}
    if(closing)return;
    if(!str_cmp(name,"base")&&attr("href",value,sizeof value)){
        char resolved[128];if(webio_resolve(page_url,value,resolved,sizeof resolved))str_copy(base_url,resolved);return;
    }
    if(!str_cmp(name,"link"))css_start_link();
}
static void css_scan_finish(void)
{
    if(css_inline_pending_len){int i;for(i=0;i<css_inline_pending_len;i++)css_inline_emit(css_inline_pending[i]);css_inline_pending_len=0;}
    css_flush();
    if(source>=0){dos_close(source);source=-1;}
    if(css_module_op(WEBSTYLE_OP_SHEET_END)!=WEBSTYLE_STATUS_OK)css_limited=1;
    /* The style prepass is complete. Reopen the cached page for layout. */
    parse_begin();
}
static void css_scan_begin(void)
{
    if(!css_module_open()){partial=1;parse_begin();return;}
    if(css_module_op(WEBSTYLE_OP_RESET)!=WEBSTYLE_STATUS_OK){partial=1;parse_begin();return;}
    if(source>=0){dos_close(source);source=-1;}
    source=dos_open(document_path(),0);
    if(source<0){parse_begin();return;}
    css_feed_used=css_link_count=css_limited=0;css_inline_pending_len=css_inline_match=css_inline_end=0;
    css_scan_state=css_scan_quote=css_scan_skip_match=css_scan_comment=0;tag_len=input_at=input_len=0;
    css_hidden=css_depth=0;current_bg=255;style=C_INK;
    loading=6;str_copy(message,"Reading page stylesheets...");
}
static void css_scan_step(void)
{
    int budget=256,ch;
    while(budget-->0){
        if(input_at>=input_len){input_len=dos_read(source,input,sizeof input);input_at=0;if(input_len<=0){css_scan_finish();return;}}
        ch=(u8)input[input_at++];
        if(css_scan_state==3){
            static const char ending[]="</style";
            if(css_inline_end){
                if(ch=='>' ){css_inline_end=css_inline_match=css_inline_pending_len=0;css_scan_state=0;continue;}
                if(space(ch))continue;
                {int i;for(i=0;i<css_inline_pending_len;i++)css_inline_emit(css_inline_pending[i]);css_inline_pending_len=css_inline_match=css_inline_end=0;}
            }
            if(css_inline_pending_len||ch=='<'){
                if(css_inline_pending_len<sizeof css_inline_pending)css_inline_pending[css_inline_pending_len++]=(char)ch;
                else {css_inline_emit(css_inline_pending[0]);mem_move(css_inline_pending,css_inline_pending+1,--css_inline_pending_len);css_inline_pending[css_inline_pending_len++]=(char)ch;}
                css_inline_match=0;while(css_inline_match<css_inline_pending_len&&css_inline_match<7&&lower(css_inline_pending[css_inline_match])==ending[css_inline_match])++css_inline_match;
                if(css_inline_match==7){css_inline_end=1;continue;}
                if(css_inline_match==css_inline_pending_len)continue;
                {int i;for(i=0;i<css_inline_pending_len;i++)css_inline_emit(css_inline_pending[i]);css_inline_pending_len=css_inline_match=0;}
                continue;
            }
            css_inline_emit((char)ch);continue;
        }
        if(css_scan_state==4){
            static const char end_script[]="</script";
            if(css_scan_skip_match==8){if(ch=='>'){css_scan_state=0;css_scan_skip_match=0;}else if(!space(ch))css_scan_skip_match=ch=='<'?1:0;}
            else if(lower(ch)==end_script[css_scan_skip_match]){if(++css_scan_skip_match==8){} }
            else css_scan_skip_match=ch=='<'?1:0;
            continue;
        }
        if(css_scan_state==2){if(ch=='-'){if(css_scan_comment<2)++css_scan_comment;}else{if(ch=='>'&&css_scan_comment==2)css_scan_state=0;css_scan_comment=0;}continue;}
        if(css_scan_state==1){
            if(ch=='\''||ch=='"'){if(!css_scan_quote)css_scan_quote=ch;else if(css_scan_quote==ch)css_scan_quote=0;}
            if(ch=='>'&&!css_scan_quote){tag[tag_len]=0;css_scan_tag_done();tag_len=0;if(loading==1)return;css_scan_quote=0;}
            else if(tag_len<sizeof tag-1){tag[tag_len++]=(char)ch;if(tag_len==3&&!mem_cmp(tag,"!--",3)){css_scan_state=2;css_scan_comment=0;tag_len=0;}}
            continue;
        }
        if(ch=='<'){css_scan_state=1;tag_len=css_scan_quote=0;}
    }
    css_flush();
}
static void css_feed_step(void)
{
    int n;
    if(css_feed_file<0){loading=6;return;}
    n=dos_read(css_feed_file,css_feed_buffer,sizeof css_feed_buffer);
    if(n>0){if(css_module_feed(css_feed_buffer,(u16)n)!=WEBSTYLE_STATUS_OK)css_limited=1;return;}
    dos_close(css_feed_file);css_feed_file=-1;loading=6;
}
static int dimension(const char *name,int fallback,int maximum)
{
    char value[20];u32 n=0;int i;if(!attr(name,value,sizeof value))return fallback;
    for(i=0;value[i]>='0'&&value[i]<='9';++i){n=n*10+value[i]-'0';if(n>(u32)maximum)return maximum;}
    return n?(int)n:fallback;
}
static void line_break(int gap)
{
    long next=(long)ly+gap;
    if(lx>left_margin||line_height>18)next+=line_height;
    if(next>=PAGE_HEIGHT){next=PAGE_HEIGHT;partial=1;}
    ly=(int)next;lx=left_margin;line_height=18;last_space=1;
    if(ly>doc_height)doc_height=ly;if(ly>table_bottom)table_bottom=ly;
}
static int add_node(int kind,const char *text,int width)
{
    int b;if(nodes>=NODE_MAX||ly>=PAGE_HEIGHT){partial=1;return 0;}
    b=ly>>4;mem_set(&item,0,sizeof item);item.x=lx;item.y=ly;item.w=width;item.style=style;item.link=active_link;item.kind=kind;item.bg=(u8)current_bg;
    if(text)str_ncopy(item.text,text,sizeof item.text);item.next=bucket[b];
    if(!webstore_write(node_handle,(u32)nodes*sizeof item,&item,sizeof item)){partial=1;return 0;}
    bucket[b]=++nodes;return 1;
}
static void flush_word(void)
{
    char piece[68];int width,at=0,n;
    if(!word_len)return;word[word_len]=0;
    while(at<word_len){
        str_copy(piece,word+at);width=ui_measure_style(piece,style);
        if(lx>left_margin&&lx+width>right_margin)line_break(0);
        n=str_len(piece);
        while(n>1&&width>right_margin-left_margin){piece[--n]=0;width=ui_measure_style(piece,style);}
        if(width>right_margin-left_margin){partial=1;break;}
        add_node(0,piece,width);lx+=width;at+=n;
        if(at<word_len)line_break(0);
    }
    word_len=0;
}
static void text_char(int ch)
{
    if(parse_title){if(title_len<63&&ch>=32)title[title_len++]=(char)ch;title[title_len]=0;return;}
    if(!body_started||skip||css_hidden)return;
    if(space(ch)){flush_word();if(preformatted&&ch=='\n')line_break(0);else if(lx>left_margin&&(!last_space||preformatted))lx+=ui_measure_style(" ",style);last_space=1;return;}
    if(ch<32)return;last_space=0;if(word_len==sizeof word-1)flush_word();word[word_len++]=(char)ch;
}
static int css_is_block_tag(const char *name)
{
    return !str_cmp(name,"p")||!str_cmp(name,"div")||!str_cmp(name,"blockquote")||!str_cmp(name,"pre")||
        !str_cmp(name,"ul")||!str_cmp(name,"ol")||!str_cmp(name,"li")||!str_cmp(name,"table")||
        !str_cmp(name,"form")||!str_cmp(name,"tr")||(name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&!name[2]);
}
static int css_is_void_tag(const char *name)
{
    return !str_cmp(name,"area")||!str_cmp(name,"base")||!str_cmp(name,"br")||!str_cmp(name,"col")||
        !str_cmp(name,"embed")||!str_cmp(name,"hr")||!str_cmp(name,"img")||!str_cmp(name,"input")||
        !str_cmp(name,"link")||!str_cmp(name,"meta")||!str_cmp(name,"param")||!str_cmp(name,"source");
}
static int css_old_color(const char *s)
{
    if(starts(s,"red")||starts(s,"#ff0000"))return C_RED;
    if(starts(s,"blue")||starts(s,"#0000ff"))return C_BLUE;
    if(starts(s,"green")||starts(s,"#008000"))return C_GREEN;
    if(starts(s,"gray")||starts(s,"grey")||starts(s,"#808080"))return C_SHADOW;
    if(starts(s,"purple")||starts(s,"#800080"))return C_PURPLE;
    return C_INK;
}
static void css_use_style(const struct webstyle_computed *computed,const char *name,int allow_hint)
{
    char value[96];int fg;
    fg=css_palette(computed->color);
    if(active_link&&!(computed->declared&WEBSTYLE_PROP_COLOR))fg=C_BLUE;
    if(allow_hint&&!(computed->declared&WEBSTYLE_PROP_COLOR)&&attr("color",value,sizeof value))fg=css_old_color(value);
    style=fg;
    if(computed->font_weight==WEBSTYLE_WEIGHT_BOLD)style|=BOLD;
    if(computed->font_style==WEBSTYLE_FONT_ITALIC)style|=ALTFONT;
    else if(allow_hint&&!(computed->declared&WEBSTYLE_PROP_FONT_WEIGHT)&&
            (!str_cmp(name,"b")||!str_cmp(name,"strong")||(name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&!name[2])||!str_cmp(name,"th")))style|=BOLD;
    if((computed->specified&WEBSTYLE_PROP_BACKGROUND_COLOR)&&computed->background_color!=0xFFFFFFFFUL){
        int color=css_palette(computed->background_color);if(color<16)current_bg=color;
    }
}
static int css_has_class(const char *classes,const char *target)
{
    int tlen=str_len(target);const char *p=classes;
    while(*p){
        const char *start;
        while(*p==' ')++p;
        if(!*p)break;
        start=p;
        while(*p&&*p!=' ')++p;
        if(p-start==tlen&&!mem_cmp(start,target,tlen))return 1;
    }
    return 0;
}
static int css_style_enter(const char *name,int is_void)
{
    char id[WEBSTYLE_ID_MAX],classes[WEBSTYLE_CLASSES_MAX],inline_style[WEBSTYLE_INLINE_MAX];
    int old_hidden=css_hidden,block,rc;struct browser_css_frame *frame=0;
    if(!css_module_open())return old_hidden;
    mem_set(&style_request,0,sizeof style_request);style_request.abi_version=WEBSTYLE_ABI_VERSION;
    style_request.struct_bytes=sizeof style_request;style_request.op=WEBSTYLE_OP_ENTER;style_request.flags=is_void?1:0;
    str_ncopy(style_request.tag,name,sizeof style_request.tag);attr("id",id,sizeof id);attr("class",classes,sizeof classes);attr("style",inline_style,sizeof inline_style);
    str_ncopy(style_request.id,id,sizeof style_request.id);str_ncopy(style_request.classes,classes,sizeof style_request.classes);
    str_ncopy(style_request.inline_style,inline_style,sizeof style_request.inline_style);
    webmodule_call(&style_module,EV_POLL,&style_request);rc=style_request.status;
    if(rc!=WEBSTYLE_STATUS_OK){partial=1;return old_hidden;}
    current_css=style_request.style;
    if(css_has_class(classes,"prompt-to-accept")||css_has_class(classes,"iubenda-cs-banner")||!str_cmp(id,"iubenda-cs-banner"))
        current_css.display=WEBSTYLE_DISPLAY_NONE;
    block=(current_css.display==WEBSTYLE_DISPLAY_BLOCK||
          (!(current_css.specified&WEBSTYLE_PROP_DISPLAY)&&css_is_block_tag(name)))&&!is_void;
    if(!is_void&&css_depth<64){
        frame=&browser_css[css_depth++];str_ncopy(frame->tag,name,sizeof frame->tag);
        frame->old_left=(short)left_margin;frame->old_right=(short)right_margin;frame->old_bg=(u16)current_bg;
        frame->old_style=style;frame->old_hidden=(u8)css_hidden;frame->old_link=(u8)active_link;frame->block=(u8)block;
        frame->bottom_gap=(short)(current_css.margin[2]+current_css.padding[2]);
    } else if(!is_void) {partial=1;return old_hidden;}
    if(current_css.display==WEBSTYLE_DISPLAY_NONE)css_hidden=1;
    css_use_style(&current_css,name,1);
    if(block){
        left_margin+=current_css.margin[3]+current_css.padding[3];
        right_margin-=current_css.margin[1]+current_css.padding[1];
        if(current_css.width>=0&&current_css.width<right_margin-left_margin)right_margin=left_margin+current_css.width;
        if(left_margin<0)left_margin=0;if(right_margin>cached_width-50)right_margin=cached_width-50;
        if(right_margin<left_margin+8)right_margin=left_margin+8;
        css_top_gap=current_css.margin[0]+current_css.padding[0];if(css_top_gap<0)css_top_gap=0;
    } else css_top_gap=0;
    if(is_void&&css_hidden!=old_hidden)css_hidden=old_hidden;
    return old_hidden||current_css.display==WEBSTYLE_DISPLAY_NONE;
}
static int css_style_leave(const char *name)
{
    int was_hidden=css_hidden,found=-1,leave_ok=0;u16 i;
    if(!css_depth)return was_hidden;
    for(i=css_depth;i>0;i--)if(!str_icmp(browser_css[i-1].tag,name)){found=(int)i-1;break;}
    if(found<0)return was_hidden;
    if(style_module.segment){
        mem_set(&style_request,0,sizeof style_request);style_request.abi_version=WEBSTYLE_ABI_VERSION;
        style_request.struct_bytes=sizeof style_request;style_request.op=WEBSTYLE_OP_LEAVE;
        str_ncopy(style_request.tag,name,sizeof style_request.tag);webmodule_call(&style_module,EV_POLL,&style_request);
        leave_ok=style_request.status==WEBSTYLE_STATUS_OK;
    }
    css_close_gap=browser_css[found].bottom_gap;if(css_close_gap<0)css_close_gap=0;
    css_close_block=browser_css[found].block;
    while(css_depth>(u16)found){
        struct browser_css_frame *frame=&browser_css[--css_depth];
        left_margin=frame->old_left;right_margin=frame->old_right;current_bg=frame->old_bg;
        style=frame->old_style;active_link=frame->old_link;css_hidden=frame->old_hidden;
    }
    if(style_module.segment&&leave_ok)current_css=style_request.style;
    css_top_gap=0;
    return was_hidden;
}
static void css_restore_void(int old_style,u16 old_bg)
{
    style=old_style;current_bg=old_bg;css_top_gap=0;
}
static void css_unwind_all(void)
{
    while(css_depth){
        struct browser_css_frame *frame=&browser_css[css_depth-1];
        if(style_module.segment){
            mem_set(&style_request,0,sizeof style_request);style_request.abi_version=WEBSTYLE_ABI_VERSION;
            style_request.struct_bytes=sizeof style_request;style_request.op=WEBSTYLE_OP_LEAVE;
            webmodule_call(&style_module,EV_POLL,&style_request);
            if(style_request.status==WEBSTYLE_STATUS_OK)current_css=style_request.style;
        }
        left_margin=frame->old_left;right_margin=frame->old_right;current_bg=frame->old_bg;
        style=frame->old_style;active_link=frame->old_link;css_hidden=frame->old_hidden;--css_depth;
    }
}
static int image_supported_url(const char *url)
{
    int i=0,dot=-1;
    while(url[i]&&url[i]!='?'&&url[i]!='#'){if(url[i]=='.')dot=i;++i;}
    if(dot>=0){
        char ext[8];int len=0;
        for(i=dot+1;url[i]&&url[i]!='?'&&url[i]!='#'&&len<7;++i)ext[len++]=(char)lower(url[i]);
        ext[len]=0;
        if(!str_cmp(ext,"svg")||!str_cmp(ext,"webp")||!str_cmp(ext,"avif")||!str_cmp(ext,"ico"))return 0;
    }
    return 1;
}
static void image_tag(void)
{
    char src[128],resolved[128],alt[48];int i,w,h;
    if(ly>=PAGE_HEIGHT||right_margin<=left_margin){partial=1;return;}
    if(!attr("src",src,sizeof src)||!webio_resolve(base_url,src,resolved,sizeof resolved))return;
    if(!image_supported_url(resolved))return;
    for(i=0;i<image_count;++i)if(!str_cmp(images[i].url,resolved)&&images[i].x==65535u)break;
    if(i==image_count){if(image_count==IMAGE_MAX){partial=1;return;}++image_count;str_copy(images[i].url,resolved);images[i].state=0;}
    attr("alt",alt,sizeof alt);str_copy(images[i].alt,alt);
    w=dimension("width",images[i].width?images[i].width:160,2048);h=dimension("height",images[i].height?images[i].height:100,2048);
    if((current_css.specified&WEBSTYLE_PROP_WIDTH)&&current_css.width>=0)w=minimum(current_css.width,2048);
    if((current_css.specified&WEBSTYLE_PROP_HEIGHT)&&current_css.height>=0)h=minimum(current_css.height,2048);
    if(w>right_margin-left_margin){h=(int)((u32)h*(right_margin-left_margin)/w);w=right_margin-left_margin;}
    if(w<1)w=1;if(h<1)h=1;
    if(lx>left_margin&&lx+w>right_margin)line_break(0);
    if(in_center&&lx==left_margin&&w<right_margin-left_margin)images[i].x=left_margin+(right_margin-left_margin-w)/2;
    else images[i].x=lx;
    images[i].y=ly;images[i].w=w;images[i].h=h;images[i].link=active_link;
    lx+=w+6;if(line_height<h+6)line_height=h+6;
    if(in_center)line_break(0);
}
static void field_tag(const char *name)
{
    char value[128];struct web_field *f;int n;if(field_count==FIELD_MAX||current_form<0)return;
    if(ly>=PAGE_HEIGHT||right_margin<=left_margin){partial=1;return;}
    f=&fields[field_count];mem_set(f,0,sizeof *f);f->form=current_form;
    attr("name",f->name,sizeof f->name);attr("value",f->value,sizeof f->value);attr("type",value,sizeof value);
    f->type=!str_icmp(value,"hidden")?1:!str_icmp(value,"checkbox")?2:!str_icmp(value,"radio")?3:
            !str_icmp(value,"submit")||!str_icmp(name,"button")?4:0;
    if(!str_icmp(value,"password"))f->type=5;f->checked=attr("checked",value,sizeof value);
    n=dimension("size",20,60);
    if(f->type==2||f->type==3)f->w=16;
    else if(f->type==4){int mw=ui_measure_style(f->value,style)+16;f->w=minimum(mw>90?mw:90,right_margin-left_margin);}
    else if((f->type==0||f->type==5)&&(current_css.specified&WEBSTYLE_PROP_WIDTH)&&current_css.width>0)
        f->w=minimum(current_css.width,right_margin-left_margin);
    else f->w=minimum(n*8,right_margin-left_margin);
    if(f->type!=1){if(lx>left_margin&&lx+f->w>right_margin)line_break(4);f->x=lx;f->y=ly;lx+=f->w+6;if(line_height<28)line_height=28;}
    ++field_count;
}

/* Determine first-row column count without retaining the entire HTML table.
 * Restore the underlying file position; the parser's buffered bytes survive. */
static int count_columns(void)
{
    long saved=dos_seek(source,0,1);char bytes[128],name[12];
    int limit=8192,n,i,reading=0,len=0,count=0,done=0;
    if(table_scans++>=16||saved<0||dos_seek(source,(long)document_bytes,0)<0)return 1;
    while(limit>0&&!done&&(n=dos_read(source,bytes,sizeof bytes))>0){
        limit-=n;
        for(i=0;i<n&&!done;++i){
            int ch=(u8)bytes[i];
            if(ch=='<'){reading=1;len=0;}
            else if(reading){
                if(ch=='>'||space(ch)){
                    name[len]=0;reading=0;
                    if(!str_cmp(name,"td")||!str_cmp(name,"th"))++count;
                    else if((!str_cmp(name,"/tr")&&count)||!str_cmp(name,"/table"))done=1;
                }else if(len<11)name[len++]=(char)lower(ch);
            }
        }
    }
    dos_seek(source,saved,0);return count<1?1:count>8?8:count;
}

static void handle_tag(void)
{
    char name[20],value[128];int i=0,j=0,closing=tag[0]=='/',is_void,old_style=style,hidden_before,old_link=active_link;
    u16 old_bg=(u16)current_bg;
    flush_word();if(closing)++i;
    while(tag[i]&&!space(tag[i])&&tag[i]!='/'&&j<19)name[j++]=(char)lower(tag[i++]);name[j]=0;
    is_void=css_is_void_tag(name);
    if(!closing&&body_started&&!str_cmp(name,"a")){
        active_link=0;
        if(link_count<LINK_MAX-1&&attr("href",value,sizeof value)&&
           webio_resolve(base_url,value,links[link_count+1],sizeof links[0]))active_link=++link_count;
    }
    hidden_before=closing?css_style_leave(name):css_style_enter(name,is_void);
    if(!closing&&!str_cmp(name,"a")&&!is_void&&css_depth)browser_css[css_depth-1].old_link=(u8)old_link;
    if(hidden_before){if(is_void)css_restore_void(old_style,old_bg);return;}
    if(!str_cmp(name,"title")){parse_title=!closing;return;}
    if(!str_cmp(name,"head")){body_started=closing;return;}
    if(!str_cmp(name,"body")){body_started=!closing;return;}
    if(!str_cmp(name,"script")||!str_cmp(name,"style")){if(!closing)skip=!str_cmp(name,"script")?1:2;return;}
    if(!str_cmp(name,"base")&&!closing&&attr("href",value,sizeof value)){
        char resolved[128];if(webio_resolve(page_url,value,resolved,sizeof resolved))str_copy(base_url,resolved);css_restore_void(old_style,old_bg);return;
    }
    if(!body_started){if(is_void)css_restore_void(old_style,old_bg);return;}
    if(!str_cmp(name,"a"))return;
    if(!str_cmp(name,"img")&&!closing){image_tag();css_restore_void(old_style,old_bg);return;}
    if(!str_cmp(name,"br")){line_break(lx==left_margin?18:0);css_restore_void(old_style,old_bg);return;}
    if(!str_cmp(name,"hr")){line_break(8);add_node(1,0,right_margin-left_margin);line_break(10);css_restore_void(old_style,old_bg);return;}
    if(!str_cmp(name,"b")||!str_cmp(name,"strong")||!str_cmp(name,"font")||!str_cmp(name,"span")){
        return;
    }
    if(!str_cmp(name,"center")){
        in_center=!closing;
        if(lx>left_margin)line_break(0);
        return;
    }
    if((name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&!name[2])||!str_cmp(name,"p")||
       !str_cmp(name,"blockquote")||!str_cmp(name,"pre")){
        line_break((name[0]=='h'?10:6)+(closing?css_close_gap:css_top_gap));
        if(!str_cmp(name,"pre"))preformatted=!closing;return;
    }
    if(!str_cmp(name,"div")){
        if(current_css.display==WEBSTYLE_DISPLAY_INLINE)return;
        if(closing){if(lx>left_margin||css_close_gap)line_break(css_close_gap);}
        else{if(lx>left_margin||css_top_gap)line_break(css_top_gap);}
        return;
    }
    if(!str_cmp(name,"ul")||!str_cmp(name,"ol")){
        line_break(4+(closing?css_close_gap:css_top_gap));
        if(!closing){left_margin+=16;if(left_margin<0)left_margin=0;if(left_margin>right_margin-32)left_margin=right_margin-32;lx=left_margin;}
        return;
    }
    if(!str_cmp(name,"li")){line_break(2+(closing?css_close_gap:css_top_gap));if(!closing){add_node(0,"*",8);lx+=14;}return;}
    if(!str_cmp(name,"table")){
        line_break(8+(closing?css_close_gap:css_top_gap));
        if(!closing){table=1;table_x=left_margin;table_y=table_bottom=ly;table_width=right_margin-left_margin;table_columns=count_columns();}
        else{table=0;left_margin=table_x;right_margin=table_x+table_width;ly=table_bottom;lx=left_margin;line_break(8+css_close_gap);}return;
    }
    if(table&&!str_cmp(name,"tr")){
        if(closing){line_break(4+css_close_gap);if(ly>table_bottom)table_bottom=ly;}
        else{ly=table_y=table_bottom;table_column=0;}return;
    }
    if(table&&(!str_cmp(name,"td")||!str_cmp(name,"th"))){
        if(closing){line_break(4+css_close_gap);if(ly>table_bottom)table_bottom=ly;}
        else{left_margin=table_x+table_column*(table_width/table_columns);right_margin=minimum(left_margin+table_width/table_columns-8,cached_width-50);
             if(left_margin>=right_margin){left_margin=table_x;ly=table_y=table_bottom;table_column=0;right_margin=left_margin+table_width/table_columns-8;}
             lx=left_margin;ly=table_y;++table_column;}return;
    }
    if(!str_cmp(name,"form")){
        line_break(6+(closing?css_close_gap:css_top_gap));
        if(closing)current_form=-1;
        else if(form_count<FORM_MAX){
            current_form=form_count++;attr("action",value,sizeof value);
            if(!webio_resolve(base_url,value,forms[current_form].action,128))str_copy(forms[current_form].action,page_url);
            attr("method",value,sizeof value);forms[current_form].post=!str_icmp(value,"post");
        }return;
    }
    if((!str_cmp(name,"input")||!str_cmp(name,"button"))&&!closing){field_tag(name);css_restore_void(old_style,old_bg);}
    else if(is_void)css_restore_void(old_style,old_bg);
    else if(closing&&css_close_block)line_break(css_close_gap);
    else if(!closing&&current_css.display==WEBSTYLE_DISPLAY_BLOCK&&!css_is_block_tag(name))line_break(css_top_gap);
}
static void parse_begin(void)
{
    int i;char stats[48];source=dos_open(document_path(),0);
    if(source<0){loading=0;error("The page cache cannot be read.");return;}
    if(!node_handle){
        str_copy(stats,"XMS free KiB=");fmt_u32(stats+str_len(stats),webstore_available());app_log("[CIUKWEB] memory",stats);
        node_handle=webstore_alloc((u32)NODE_MAX*sizeof item);
    }
    if(!node_handle){dos_close(source);source=-1;loading=0;error("Not enough extended memory for this page.");return;}
    if(!bucket_seg){bucket_seg=dos_alloc(251);bucket=(u16 __far *)((u32)bucket_seg<<16);}
    if(!bucket_seg){dos_close(source);source=-1;loading=0;error("Not enough conventional memory for page layout.");return;}
    for(i=0;i<2001;++i)bucket[i]=0;nodes=link_count=field_count=form_count=0;
    for(i=0;i<image_count;++i)images[i].x=65535u;
    cached_width=HOST.w;left_margin=0;right_margin=HOST.w-50;lx=ly=0;line_height=18;doc_height=0;
    style=C_INK;active_link=preformatted=table=partial=table_scans=0;current_bg=255;
    css_depth=css_hidden=0;current_form=-1;
    input_at=input_len=parse_state=tag_len=quote=word_len=skip=skip_match=parse_title=title_len=0;
    title[0]=0;body_started=last_space=1;document_bytes=0;loading=2;
    str_copy(base_url,page_url);str_copy(message,"Laying out page...");
}
static void image_next(void);
static void parse_done(void)
{
    char stats[48];flush_word();css_unwind_all();line_break(4);doc_height=ly;dos_close(source);source=-1;loading=0;
    if(scroll>doc_height)scroll=doc_height;
    str_copy(stats,"nodes=");fmt_u32(stats+str_len(stats),nodes);str_cat(stats," images=");fmt_u32(stats+str_len(stats),image_count);
    app_log("[CIUKWEB] rendered",stats);
    if(partial)str_copy(message,"This long page reached the layout limit; the rendered part is available.");
    else if(js_warning)str_copy(message,"Page loaded; a script failed or exceeded its execution limit.");
    else if(css_limited)str_copy(message,"Page loaded; some stylesheet rules exceeded the style worker limits.");
    else str_ncopy(message,title[0]?title:"Page loaded.",sizeof message);
    if(!relayout){current_image=0;image_next();}relayout=0;
}
static void parse_step(void)
{
    /* Return to input dispatch frequently; a word can involve an XMS move
       and a font measurement, so a network-sized batch is too large here. */
    int budget=256,ch;
    while(budget-->0){
        if(input_at>=input_len){input_len=dos_read(source,input,sizeof input);input_at=0;if(input_len<=0){parse_done();return;}}
        ch=(u8)input[input_at++];++document_bytes;
        if(document_bytes>1048576UL){partial=1;parse_done();return;}
        if(skip){
            const char *end=skip==1?"</script":"</style";
            if(lower(ch)==end[skip_match])++skip_match;
            else if(!end[skip_match]&&ch=='>'){skip=skip_match=0;}
            else skip_match=ch=='<'?1:0;continue;
        }
        if(parse_state==2){
            if(ch=='-'){if(skip_match<2)++skip_match;}
            else{if(ch=='>'&&skip_match==2)parse_state=0;skip_match=0;}continue;
        }
        if(parse_state==3){
            if(ch==';'||tag_len>=12||ch=='<'||space(ch)){
                int value='?';tag[tag_len]=0;
                if(!str_cmp(tag,"amp"))value='&';else if(!str_cmp(tag,"lt"))value='<';
                else if(!str_cmp(tag,"gt"))value='>';else if(!str_cmp(tag,"quot"))value='"';
                else if(!str_cmp(tag,"apos"))value='\'';else if(!str_cmp(tag,"nbsp"))value=' ';
                else if(tag[0]=='#'){
                    int i=1,radix=10;value=0;if(tag[i]=='x'||tag[i]=='X'){++i;radix=16;}
                    for(;tag[i];++i){int n=lower(tag[i]);n=n>='0'&&n<='9'?n-'0':n>='a'&&n<='f'?n-'a'+10:radix;
                        if(n>=radix||value>255){value='?';break;}value=value*radix+n;}
                    if(value>255)value='?';
                }
                text_char(value);parse_state=0;if(ch!=';'){--input_at;--document_bytes;}
            }else tag[tag_len++]=(char)ch;continue;
        }
        if(parse_state==1){
            if(ch=='"'||ch=='\''){if(!quote)quote=ch;else if(quote==ch)quote=0;}
            if(ch=='>'&&!quote){tag[tag_len]=0;handle_tag();parse_state=0;}
            else if(tag_len<sizeof tag-1){tag[tag_len++]=(char)ch;if(tag_len==3&&!mem_cmp(tag,"!--",3)){parse_state=2;skip_match=0;}}
            else partial=1;continue;
        }
        if(ch=='<'){parse_state=1;tag_len=quote=0;}
        else if(ch=='&'){parse_state=3;tag_len=0;}
        else text_char(ch);
    }
}
static void image_begin(const char *path)
{
    struct picture *p=&images[current_image];
    if(!codec_load()){p->state=2;error("The image decoder could not be loaded.");++current_image;image_next();return;}
    mem_set(&image_request,0,sizeof image_request);
    image_request.abi_version=WEBIMG_ABI_VERSION;image_request.struct_bytes=sizeof image_request;
    str_copy(image_request.path,path?path:current_image_path);
    image_request.output_seg=app_seg();image_request.output_off=(u16)pixels;image_request.output_capacity=6144;
    codec_call(EV_OPEN);
    if(image_request.status!=WEBIMG_STATUS_READY){
        p->state=2;codec_call(EV_CLOSE);
        {int k;for(k=0;k<CACHE_MAX;++k)if(cache_valid[k]&&!str_cmp(cache_urls[k],p->url))cache_valid[k]=0;}
        ++current_image;image_next();return;
    }
    p->width=image_request.total_width;p->height=image_request.total_height;
    if(!p->width||p->width>2048||!p->height||p->height>2048){
        p->state=2;codec_call(EV_CLOSE);
        {int k;for(k=0;k<CACHE_MAX;++k)if(cache_valid[k]&&!str_cmp(cache_urls[k],p->url))cache_valid[k]=0;}
        ++current_image;image_next();return;
    }
    {
        u32 bytes=(u32)((p->width*3u+1u)&~1u)*p->height;
        if(bytes>16777216UL-cache_bytes||!(p->handle=webstore_alloc(bytes))){p->state=2;codec_call(EV_CLOSE);++current_image;image_next();return;}
        cache_bytes+=bytes;
    }
    p->state=3;loading=3;
}
static void image_next(void)
{
    int j,k;char cpath[20];
    while(current_image<image_count&&images[current_image].state)++current_image;
    if(current_image==image_count){
        int i,missing=0;for(i=0;i<image_count;++i)if(images[i].state==2)++missing;
        if(image_count){
            char stats[40];str_copy(stats,"loaded=");fmt_u32(stats+str_len(stats),image_count-missing);str_cat(stats," failed=");fmt_u32(stats+str_len(stats),missing);app_log("[CIUKWEB] images",stats);
            if(missing)str_copy(message,"Page loaded. Some images are unavailable or unsupported.");
            else str_ncopy(message,title[0]?title:"Page and images loaded.",sizeof message);
        }
        loading=0;return;
    }
    for(j=0;j<current_image;++j){
        if(images[j].state==1&&images[j].handle&&!str_cmp(images[j].url,images[current_image].url)){
            images[current_image].handle=images[j].handle;
            images[current_image].width=images[j].width;
            images[current_image].height=images[j].height;
            images[current_image].state=1;
            ++current_image;image_next();return;
        }
    }
    for(k=0;k<CACHE_MAX;++k){
        if(cache_valid[k]&&!str_cmp(cache_urls[k],images[current_image].url)){
            get_cache_path(cpath,k);
            image_begin(cpath);return;
        }
    }
    get_cache_path(current_image_path,cache_head);
    redirects=0;download_start(2,images[current_image].url);
}
static void image_step(void)
{
    struct picture *p=&images[current_image];u16 row,count,stride;codec_call(EV_POLL);
    if(image_request.status==WEBIMG_STATUS_PIXELS){
        stride=(p->width*3u+1u)&~1u;count=image_request.w*3u;
        if(!image_request.w||image_request.w>2048||!image_request.h||image_request.h>2048||
           (u32)image_request.x+image_request.w>p->width||(u32)image_request.y+image_request.h>p->height||
           (u32)image_request.w*3UL*image_request.h>6144UL||(image_request.x&1))image_request.status=WEBIMG_STATUS_ERROR;
        else for(row=0;row<image_request.h;++row){
            u8 tail=0;u16 offset=row*count,n=(count+1)&~1;
            if(n!=count){tail=pixels[offset+count];pixels[offset+count]=255;}
            if(!webstore_write(p->handle,(u32)(image_request.y+row)*stride+image_request.x*3u,pixels+offset,n))image_request.status=WEBIMG_STATUS_ERROR;
            if(n!=count)pixels[offset+count]=tail;
        }
    }
    if(image_request.status==WEBIMG_STATUS_DONE||image_request.status==WEBIMG_STATUS_ERROR){
        p->state=image_request.status==WEBIMG_STATUS_DONE?1:2;
        if(p->state==2){
            image_release(p);
            {int k;for(k=0;k<CACHE_MAX;++k)if(cache_valid[k]&&!str_cmp(cache_urls[k],p->url))cache_valid[k]=0;}
        }
        codec_call(EV_CLOSE);++current_image;image_next();
    }
}
static void js_fallback(void)
{
    js_warning=1;js_job=0;app_log("[CIUKWEB] script processing failed",0);css_scan_begin();
}
static void js_begin(void)
{
    js_cache=js_warning=js_at=js_count=0;
    str_copy(base_url,page_url);
    if(!webio_work(CWW_PAGE_SCAN,PAGE_PATH,sizeof PAGE_PATH,0)){js_fallback();return;}
    js_job=1;loading=8;str_copy(message,"Reading page scripts...");
}
static void js_next(void)
{
    char target[128];
    if(js_at<js_count){
        if(!webio_resolve(base_url,scripts[js_at].url,target,sizeof target)){
            js_warning=1;++js_at;loading=11;return;
        }
        str_copy(script_path,"C:\\NET\\CWJS00.JS");
        script_path[11]=(char)('0'+js_at/10);script_path[12]=(char)('0'+js_at%10);
        redirects=0;download_start(4,target);return;
    }
    if(!webio_work(CWW_PAGE_RUN,RENDER_PATH,sizeof RENDER_PATH,0)){js_fallback();return;}
    js_job=1;loading=10;str_copy(message,"Running page scripts...");
}
static int js_step(void)
{
    u8 bytes[4];u16 at,n;int result,i;
    result=webio_work_poll(&js_reply);if(!result)return 0;js_job=0;
    if(result<0||js_reply.error){js_fallback();return 1;}
    if(loading==8){
        if(js_reply.output_bytes<2||!webio_work_read(0,bytes,2)){js_fallback();return 1;}
        js_count=bytes[0]|((u16)bytes[1]<<8);
        if(js_count>16){js_fallback();return 1;}
        at=2;
        for(i=0;i<js_count;++i){
            if(!webio_work_read(at,bytes,4)){js_fallback();return 1;}at+=4;
            scripts[i].index=bytes[0]|((u16)bytes[1]<<8);n=bytes[2]|((u16)bytes[3]<<8);
            if(n>=128||!n||!webio_work_read(at,scripts[i].url,n)){js_fallback();return 1;}
            scripts[i].url[n]=0;at+=n;
        }
        if(at!=js_reply.output_bytes){js_fallback();return 1;}
        loading=11;return 1;
    } else if(loading==9){++js_at;loading=11;return 1;}
    else{
        if(js_reply.flags&CWW_F_MORE){
            char warning[128];u16 length=(u16)js_reply.output_bytes;
            js_warning=1;if(length>=sizeof warning)length=sizeof warning-1;
            if(length&&webio_work_read(0,warning,length)){warning[length]=0;app_log("[CIUKWEB] script warning",warning);}
        }
        js_cache=1;app_log("[CIUKWEB] scripts complete",0);css_scan_begin();return 1;
    }
}
static void downloaded(void)
{
    int status=webio_status();char location[128],target[128],stats[64];
    loading=0;
    if((status==301||status==302||status==303||status==307||status==308)&&webio_redirect(location,sizeof location)){
        if(redirects++>=5||!webio_resolve(resource_url,location,target,sizeof target)){error("The redirect is invalid or repeats too often.");return;}
        if(transfer_kind==1){str_copy(url,target);caret=str_len(url);}download_start(transfer_kind,target);return;
    }
    str_copy(stats,"status=");fmt_u32(stats+str_len(stats),status);str_cat(stats," wire=");fmt_u32(stats+str_len(stats),webio_wire_bytes());app_log("[CIUKWEB] HTTP",stats);
    if(transfer_kind==2){
        if(status>=200&&status<300){
            str_copy(cache_urls[cache_head],images[current_image].url);
            cache_valid[cache_head]=1;
            image_begin(current_image_path);
            cache_head=(cache_head+1)%CACHE_MAX;
        }else{images[current_image++].state=2;image_next();}
    }
    else if(transfer_kind==3){
        if(status>=200&&status<300){css_feed_file=dos_open(CSS_PATH,0);if(css_feed_file>=0)loading=7;else loading=6;}
        else loading=6;
    } else if(transfer_kind==4){
        if(status>=200&&status<300&&webio_work(CWW_PAGE_RESOURCE,script_path,str_len(script_path)+1,scripts[js_at].index)){
            js_job=1;loading=9;
        }else{js_warning=1;++js_at;loading=11;}
    } else{str_copy(page_url,resource_url);str_copy(url,page_url);caret=str_len(url);relayout=0;js_begin();}
}
static void fit(int x,int y,int width,const char *s,int color)
{
    char text[128];int n;str_ncopy(text,s,sizeof text);n=str_len(text);
    while(n&&ui_measure(text)>width)text[--n]=0;ui_text(x,y,text,color);
}
static void paint(void)
{
    int x=HOST.x+4,y=HOST.y+TITLE_H,w=HOST.w-8,h=HOST.h-TITLE_H-4;
    int px=x+10,py=y+40,pw=w-20,ph=h-70,i,b,first,last,paint_top,paint_bottom;
    int ax=x+208,ay=y+6,aw=w-258,ah=26,sby=y+h-24,tx,badge_w;
    u16 at;char address[128];u8 band[19];
    if(cached_width&&cached_width!=HOST.w&&!loading&&page_url[0]){relayout=1;parse_begin();}
    ui_rect(x,y,w,h,C_FACE);
    ui_button(x+6,y+6,24,26,"<",2);ui_button(x+32,y+6,24,26,">",3);
    ui_button(x+58,y+6,54,26,"Reload",4);ui_button(x+114,y+6,42,26,"Stop",5);
    ui_button(x+158,y+6,46,26,"Home",6);ui_button(x+w-46,y+6,40,26,"Go",1);
    ui_rect(ax,ay,aw,ah,C_PAPER);ui_inset(ax,ay,aw,ah);
    tx=ax+6;
    if(starts(url,"https://")){
        badge_w=48;ui_rect(ax+3,ay+3,badge_w,ah-6,C_GREEN);
        ui_text(ax+6,ay+6,"HTTPS",C_PAPER);tx=ax+3+badge_w+5;
    }else if(starts(url,"http://")){
        badge_w=42;ui_rect(ax+3,ay+3,badge_w,ah-6,C_FACE);
        ui_text(ax+6,ay+6,"HTTP",C_SHADOW);tx=ax+3+badge_w+5;
    }else if(url[1]==':'){
        badge_w=42;ui_rect(ax+3,ay+3,badge_w,ah-6,C_FACE);
        ui_text(ax+6,ay+6,"FILE",C_BLUE);tx=ax+3+badge_w+5;
    }
    i=0;str_copy(address,url);
    while(i<caret&&ui_measure(address)>aw-(tx-ax)-10)str_copy(address,url+(++i));
    fit(tx,ay+6,aw-(tx-ax)-10,address,C_INK);
    if(focused&&!((HOST.ticks/9)&1)){
        char prefix[128];int count=caret-i;if(count<0)count=0;
        str_ncopy(prefix,address,minimum(count+1,128));
        ui_text(tx+ui_measure(prefix),ay+6,"|",C_BLUE);
    }
    ui_inset(x+6,y+36,w-12,h-64);ui_rect(x+7,y+37,w-14,h-66,C_PAPER);
    for(i=0;i<image_count;++i){
        struct picture *p=&images[i];int iy=py+p->y-scroll;
        if(p->x==65535u||iy+p->h<=py||iy>=py+ph)continue;
        if(p->state==1)webstore_image(p->handle,p->width,p->height,px+p->x,iy,p->w,p->h,px,py,pw,ph);
        else if(iy>=py&&iy+18<py+ph)fit(px+p->x,iy,minimum(p->w,pw-p->x),p->alt[0]?p->alt:p->state==2?"[image unavailable]":"[loading image]",C_SHADOW);
    }
    paint_top=scroll;paint_bottom=scroll+ph;
    if(app_band_info(band)){
        int top=(int)((u16)band[2]|((u16)band[3]<<8))-py+scroll;
        int bottom=(int)((u16)band[4]|((u16)band[5]<<8))-py+scroll;
        if(paint_top<top)paint_top=top;if(paint_bottom>bottom)paint_bottom=bottom;
    }
    first=paint_top>15?(paint_top-15)>>4:0;last=minimum(paint_bottom>>4,2000);
    for(b=first;bucket&&b<=last;++b)for(at=bucket[b];at;at=item.next){
        if(at>nodes||!webstore_read(node_handle,(u32)(at-1)*sizeof item,&item,sizeof item))break;
        if(item.y<scroll||item.y+16>scroll+ph||item.x>=pw)continue;
        if(item.bg<16&&item.kind!=1)ui_rect(px+item.x,py+item.y-scroll,minimum(item.w,pw-item.x),16,item.bg);
        if(item.kind==1)ui_rect(px+item.x,py+item.y-scroll,minimum(item.w,pw-item.x),1,C_SHADOW);
        else{ui_text(px+item.x,py+item.y-scroll,item.text,item.style);
             if(item.link)ui_rect(px+item.x,py+item.y-scroll+15,item.w,1,C_BLUE);}
    }
    for(i=0;i<field_count;++i){
        struct web_field *f=&fields[i];int fy=py+f->y-scroll;if(f->type==1||fy<py||fy+24>py+ph)continue;
        if(f->type==2)draw_check(px+f->x,fy+3,f->checked);
        else if(f->type==3)draw_radio(px+f->x,fy+3,f->checked);
        else{ui_rect(px+f->x,fy,f->w,24,f->type==4?C_FACE:C_PAPER);ui_inset(px+f->x,fy,f->w,24);
             fit(px+f->x+4,fy+4,f->w-8,f->type==4?(f->value[0]?f->value:"Submit"):f->type==5?"********":f->value,C_INK);
             if(field_focus==i)draw_focus(px+f->x,fy,f->w,24);
             if(field_focus==i&&(f->type==0||f->type==5)&&!((HOST.ticks/9)&1)){
                 int tw=ui_measure(f->type==5?"********":f->value);
                 if(tw<f->w-14)ui_text(px+f->x+4+tw,fy+4,"|",C_BLUE);
             }
        }
    }
    ui_inset(x+6,sby,w-12,20);ui_rect(x+7,sby+1,w-14,18,C_FACE);
    fit(x+12,sby+3,w-160,message,loading?C_BLUE:C_INK);
    fit(x+w-140,sby+3,130,loading?"Transferring...":"Ready",C_SHADOW);
}
static int encode(char *out,int at,const char *value)
{
    static const char hex[]="0123456789ABCDEF";int c;
    while((c=(u8)*value++)!=0){
        if(at>122)return -1;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out[at++]=(char)c;
        else if(c==' ')out[at++]='+';
        else{out[at++]='%';out[at++]=hex[c>>4];out[at++]=hex[c&15];}
    }
    out[at]=0;return at;
}
static void submit(int form,int button)
{
    char target[128];int n,i,have=0;if(form<0||form>=form_count)return;
    if(forms[form].post){error("This form requires POST, which is not supported yet.");return;}
    str_copy(target,forms[form].action);n=str_len(target);
    for(i=0;i<n;++i)if(target[i]=='?'||target[i]=='#'){n=i;break;}
    if(n>120){error("The form address is too long.");return;}target[n++]='?';target[n]=0;
    for(i=0;i<field_count;++i){
        struct web_field *f=&fields[i];
        if(f->form!=form||!f->name[0]||((f->type==2||f->type==3)&&!f->checked)||(f->type==4&&i!=button))continue;
        if(have){if(n>122)goto too_long;target[n++]='&';}
        n=encode(target,n,f->name);if(n<0||n>123)goto too_long;target[n++]='=';
        n=encode(target,n,(f->type==2||f->type==3)&&!f->value[0]?"on":f->value);if(n<0)goto too_long;have=1;
    }
    if(!have)target[n-1]=0;navigate(target,1);return;
too_long:error("The form exceeds the supported address length.");
}
static int key(int keycode)
{
    int ch=KEY_CHAR(keycode),scan=KEY_SCAN(keycode),len=str_len(url),i;
    if(ch==27){if(loading){stop();str_copy(message,"Stopped.");return 1;}return 0;}
    if(ch==12){focused=1;field_focus=-1;caret=str_len(url);replace_address=1;return 1;}
    if(ch==18){navigate(page_url[0]?page_url:url,0);return 1;}
    if(ch==9&&field_count){focused=0;field_focus=(field_focus+1)%field_count;return 1;}
    if(!focused&&field_focus>=0){
        struct web_field *f=&fields[field_focus];i=str_len(f->value);
        if(ch==13){submit(f->form,f->type==4?field_focus:-1);return 1;}
        if(ch==' '&&(f->type==2||f->type==3)){f->checked=!f->checked;return 1;}
        if(f->type==0||f->type==5){if(ch==8&&i)f->value[i-1]=0;else if(ch>=32&&ch<127&&i<79){f->value[i++]=(char)ch;f->value[i]=0;}return 1;}
    }
    if(scan==0x48||scan==0x49){scroll-=scan==0x49?HOST.h-150:24;if(scroll<0)scroll=0;return 1;}
    if(scan==0x50||scan==0x51){scroll+=scan==0x51?HOST.h-150:24;if(scroll>doc_height)scroll=doc_height;return 1;}
    if(!focused)return 0;if(ch==13){navigate(url,1);return 1;}
    if(scan==0x4B&&caret>0){--caret;return 1;}if(scan==0x4D&&caret<len){++caret;return 1;}
    if(scan==0x47){caret=0;return 1;}if(scan==0x4F){caret=len;return 1;}
    if(ch==8&&caret>0){mem_move(url+caret-1,url+caret,len-caret+1);--caret;return 1;}
    if(scan==0x53&&caret<len){mem_move(url+caret,url+caret+1,len-caret);return 1;}
    if(ch>=32&&ch<127&&len<126){if(replace_address){url[0]=0;len=caret=0;replace_address=0;}for(i=len;i>=caret;--i)url[i+1]=url[i];url[caret++]=(char)ch;return 1;}
    return 0;
}
static void browser_teardown(void)
{
    int i;
    release_page();webstore_free(node_handle);node_handle=0;
    if(bucket_seg){dos_free(bucket_seg);bucket_seg=0;bucket=0;}
    if(codec_seg){codec_call(EV_CLOSE);dos_free(codec_seg);codec_seg=0;}
    if(style_module.segment){webmodule_call(&style_module,EV_CLOSE,&style_request);webmodule_free(&style_module);}
    dos_delete(IMAGE_PATH);dos_delete(CSS_PATH);dos_delete(RENDER_PATH);
    for(i=0;i<CACHE_MAX;++i){
        char cp[20];get_cache_path(cp,i);dos_delete(cp);cache_valid[i]=0;
    }
    app_log("[CIUKWEB] closed",0);
}
int app_event(int ev,int a,int b,int c)
{
    if(ev==EV_OPEN){
        if(a==2)return 1;str_copy(app_title,"CiukWeb");HDR_WIDTH=800;HDR_HEIGHT=560;
        str_copy(message,"Enter a web address and select Go.");str_copy(url,"https://example.com/");caret=str_len(url);
        if(APP_ARG[0])navigate(APP_ARG,0);return 1;
    }
    if(ev==EV_PAINT){paint();return 0;}
    if(ev==EV_POLL){
        static u16 last_caret_phase;
        u16 phase=(HOST.ticks/9)&1;
        if(closing){if(webio_close()){closing=0;browser_teardown();app_close();return 0;}return 2;}
        if(worker_stopping){
            if(!webio_close())return 2;worker_stopping=0;
            if(loading==12){
                if(url[1]==':'){str_copy(page_url,url);str_copy(base_url,url);loading=4;}
                else download_start(1,url);
            }
            return 1;
        }
        if(loading==4){css_scan_begin();return 1;}
        if(loading==11){js_next();return 1;}
        if(loading>=8&&loading<=10)return js_step();
        if(loading==6){css_scan_step();return loading==6?2:1;}
        if(loading==7){css_feed_step();return loading==7?2:1;}
        if(loading==2){parse_step();return loading==2?2:1;}
        if(loading==3){int old=current_image;image_step();return current_image!=old?1:2;}
        if(loading==1){int state=webio_poll();if(state==WEBNET_COMPLETE){downloaded();return 1;}
            if(state==WEBNET_FAILED){loading=0;
                if(transfer_kind==2){images[current_image++].state=2;image_next();}
                else if(transfer_kind==3)loading=6;
                else if(transfer_kind==4){js_warning=1;++js_at;loading=11;}
                else error(webio_error());return 1;}}
        if(phase!=last_caret_phase){
            last_caret_phase=phase;
            if(focused){
                int x=HOST.x+4,y=HOST.y+TITLE_H,w=HOST.w-8;
                int ax=x+208,ay=y+6,aw=w-258,ah=26;
                ui_damage(ax+6,ay+2,aw-12,ah-4);
                return 3;
            }
            if(field_focus>=0&&(fields[field_focus].type==0||fields[field_focus].type==5)){
                int x=HOST.x+4,y=HOST.y+TITLE_H;
                int px=x+10,py=y+40;
                int fx=px+fields[field_focus].x,fy=py+fields[field_focus].y-scroll;
                ui_damage(fx,fy,fields[field_focus].w,24);
                return 3;
            }
        }
        return 0;
    }
    if(ev==EV_KEY)return key(a);
    if(ev==EV_ACTION){
        if(a==1)navigate(url,1);
        else if(a==2&&history_at>0){char target[128];str_copy(target,history[--history_at]);navigate(target,0);}
        else if(a==3&&history_at+1<history_count){char target[128];str_copy(target,history[++history_at]);navigate(target,0);}
        else if(a==4)navigate(page_url[0]?page_url:url,0);
        else if(a==5){stop();str_copy(message,"Stopped.");}
        else if(a==6)navigate("https://example.com/",1);
        return 1;
    }
    if(ev==EV_WHEEL){scroll+=a*48;if(scroll<0)scroll=0;if(scroll>doc_height)scroll=doc_height;return 1;}
    if(ev==EV_MOUSE){
        if(a==MOUSE_HOVER||a==MOUSE_MOVE){
            if(c>=6&&c<33&&b>=212&&b<HOST.w-54)ui_cursor(CURSOR_IBEAM);
            else if(c>=38&&c<HOST.h-TITLE_H-26){
                int x=b-14,y=c-40+scroll,cur=CURSOR_ARROW,i;
                for(i=0;i<field_count;++i)if(fields[i].type!=1&&x>=fields[i].x&&x<fields[i].x+fields[i].w&&y>=fields[i].y&&y<fields[i].y+24){
                    if(fields[i].type==0||fields[i].type==5)cur=CURSOR_IBEAM;
                    break;
                }
                ui_cursor(cur);
            }else ui_cursor(CURSOR_ARROW);
            return 0;
        }
        if(a==MOUSE_DOWN){
            int i;u16 at;
        if(c>=6&&c<33&&b>=212&&b<HOST.w-54){focused=1;field_focus=-1;caret=str_len(url);return 1;}
        focused=0;field_focus=-1;
        if(c>=38&&c<HOST.h-TITLE_H-26){
            int x=b-14,y=c-40+scroll;
            for(i=0;i<field_count;++i)if(fields[i].type!=1&&x>=fields[i].x&&x<fields[i].x+fields[i].w&&y>=fields[i].y&&y<fields[i].y+24){
                field_focus=i;if(fields[i].type==2)fields[i].checked=!fields[i].checked;
                if(fields[i].type==3){int j;for(j=0;j<field_count;++j)if(fields[j].type==3&&fields[j].form==fields[i].form&&!str_cmp(fields[j].name,fields[i].name))fields[j].checked=0;fields[i].checked=1;}
                if(fields[i].type==4)submit(fields[i].form,i);return 1;
            }
            for(i=0;i<image_count;++i)if(images[i].link&&x>=images[i].x&&x<images[i].x+images[i].w&&y>=images[i].y&&y<images[i].y+images[i].h){navigate(links[images[i].link],1);return 1;}
            for(i=y>15?(y-15)>>4:0;bucket&&i<2001&&i<=(y>>4);++i)for(at=bucket[i];at;at=item.next){
                if(at>nodes||!webstore_read(node_handle,(u32)(at-1)*sizeof item,&item,sizeof item))break;
                if(item.link&&x>=item.x&&x<item.x+item.w&&y>=item.y&&y<item.y+18){navigate(links[item.link],1);return 1;}
            }
            return 1;
        }
        return 0;
    }
    return 0;
}
    if(ev==EV_SUSPEND||ev==EV_CLOSE){
        /* WEBNET owns worker-backed transfer buffers; keep this app alive if
           its worker cannot stop cleanly. */
        if(ev==EV_SUSPEND)str_ncopy(APP_ARG,url,APP_ARG_BYTES);
        if(!webio_close()){
            if(ev==EV_CLOSE){closing=1;str_copy(message,"Closing web services...");}
            else{stop();worker_stopping=1;} /* Stay resident across suspension. */
            return 1;
        }
        browser_teardown();
        return 0;
    }
    return 0;
}
