/* Sierra-era public GDI helpers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/printing.h>
HBITMAP W32API CreateDiscardableBitmap(HDC h,int w,int height) {return CreateCompatibleBitmap(h,w,height);}
BOOL W32API FloodFill(HDC h,int x,int y,COLORREF c) {return ExtFloodFill(h,x,y,c,0);}
BOOL W32API GetAspectRatioFilterEx(HDC h,SIZE *p) {
  GDI_GUARD;if(!gdi_dc(h) || !p) return 0;*p=(SIZE){1,1};return 1;
}
BOOL W32API GetBitmapDimensionEx(HBITMAP h,SIZE *p) {
  GDI_GUARD;struct object *o=gdi_object(h,GDI_BITMAP);
  if(!o || !p) return 0;*p=o->u.bitmap.dimension;return 1;
}
BOOL W32API SetBitmapDimensionEx(HBITMAP h,int x,int y,SIZE *old) {
  GDI_GUARD;struct object *o=gdi_object(h,GDI_BITMAP);if(!o) return 0;
  if(old) *old=o->u.bitmap.dimension;o->u.bitmap.dimension=(SIZE){x,y};return 1;
}
LONG W32API SetBitmapBits(HBITMAP h,DWORD count,const void *bits) {
  GDI_GUARD;struct object *o=gdi_object(h,GDI_BITMAP);
  if(!o || !bits) return 0;
  struct w32_dib *b=&o->u.bitmap.dib;
  DWORD row=((b->width*b->bpp+15)/16)*2;
  if(count>row*b->height) count=row*b->height;
  for(DWORD offset=0;offset<count;) {
    DWORD y=offset/row,bytes=count-offset;
    if(bytes>row) bytes=row;
    DWORD to=b->top_down ? y : b->height-1-y;
    w32_copy(b->bits+to*b->pitch,(const uint8_t *)bits+offset,bytes<b->pitch ? bytes : b->pitch);
    offset+=bytes;
  }
  return (LONG)count;
}
HBRUSH W32API CreateDIBPatternBrushPt(const void *packed,unsigned usage) {
  GDI_GUARD;
  if(!packed || usage>1) return 0;
  const BITMAPINFO *info=packed;struct w32_dib src;
  if(!gdi_info(0,info,usage,(void *)packed,&src)) return 0;
  DWORD offset=info->bmiHeader.biSize;
  if(info->bmiHeader.biCompression==BI_BITFIELDS && offset==40) offset+=12;
  offset+=src.color_count*(usage ? 2 : 4);
  src.bits=(uint8_t *)packed+offset;
  HBRUSH h=CreateSolidBrush(0);struct object *o=gdi_object(h,GDI_BRUSH);if(!o) return 0;
  struct brush *b=&o->u.brush;b->log.lbStyle=BS_PATTERN;
  b->pattern=(struct w32_dib){.bits=b->pixels,.width=8,.height=8,.pitch=32,.size=256,
      .bpp=32,.top_down=1,.masks={0xff0000,0xff00,0xff}};
  for(int y=0;y<8;y++) for(int x=0;x<8;x++)
    w32_dib_put(&b->pattern,x,y,w32_dib_rgb(&src,x%(int)src.width,y%(int)src.height));
  return h;
}
BOOL W32API GetColorAdjustment(HDC h,void *out) {
  GDI_GUARD;struct dc *d=gdi_dc(h);if(!d || !out) return 0;
  w32_copy(out,d->s.color_adjustment,24);return 1;
}
BOOL W32API SetColorAdjustment(HDC h,const void *in) {
  GDI_GUARD;struct dc *d=gdi_dc(h);const uint8_t *p=in;
  if(!d || !p || w32_u16(p)!=24 || w32_u16(p+2)>3 || w32_u16(p+4)>8) return 0;
  for(unsigned i=6;i<=10;i+=2) if(w32_u16(p+i)<2500 || w32_u16(p+i)>65000) return 0;
  if(w32_u16(p+12)>4000 || w32_u16(p+14)<6000 || w32_u16(p+14)>10000) return 0;
  for(unsigned i=16;i<24;i+=2) if((int16_t)w32_u16(p+i)<-100 || (int16_t)w32_u16(p+i)>100) return 0;
  w32_copy(d->s.color_adjustment,p,24);return 1;
}
BOOL W32API SetTextJustification(HDC h,int extra,int breaks) {
  GDI_GUARD;struct dc *d=gdi_dc(h);
  if(!d || breaks<0 || extra < -GDI_LIMIT || extra>GDI_LIMIT) return 0;
  d->s.justify_extra=extra;d->s.justify_breaks=breaks;return 1;
}
BOOL W32API UpdateColors(HDC h) {
  GDI_GUARD;struct dc *d=gdi_dc(h);if(!d) return 0;
  RealizePalette(h);struct w32_dib *s=gdi_target(d);
  if(s) gdi_damage(d,(RECT){0,0,(LONG)s->width,(LONG)s->height});return 1;
}
static uint32_t float_integer(int64_t n) {
  uint32_t sign=n<0 ? 0x80000000u : 0;uint64_t v=n<0 ? (uint64_t)-n : (uint64_t)n;
  if(!v) return 0;
  unsigned k=0;for(uint64_t x=v;x>1;x>>=1) k++;
  uint64_t m=k<=23 ? v<<(23-k) : v>>(k-23);
  return sign|((k+127)<<23)|((uint32_t)m&0x7fffff);
}
BOOL W32API GetCharWidthFloatA(HDC h,unsigned a,unsigned b,float *out) {
  if(!out || b<a || b-a>8191) return 0;
  int *width=gdi_alloc((b-a+1)*sizeof(int));if(!width) return 0;
  BOOL ok=GetCharWidth32A(h,a,b,width);
  if(ok) for(unsigned i=0;i<=b-a;i++) {uint32_t v=float_integer(width[i]);w32_copy(out+i,&v,4);}
  gdi_free(width,(b-a+1)*sizeof(int));return ok;
}
BOOL W32API GetCharABCWidthsFloatA(HDC h,unsigned a,unsigned b,void *out) {
  if(!out || b<a || b-a>8191) return 0;
  ABC *abc=gdi_alloc((b-a+1)*sizeof(ABC));if(!abc) return 0;
  BOOL ok=GetCharABCWidthsA(h,a,b,abc);
  if(ok) for(unsigned i=0;i<=b-a;i++) {
    uint32_t v[3]={float_integer(abc[i].abcA),float_integer(abc[i].abcB),float_integer(abc[i].abcC)};
    w32_copy((uint8_t *)out+i*12,v,12);
  }
  gdi_free(abc,(b-a+1)*sizeof(ABC));return ok;
}
HDC W32API ResetDCA(HDC h,const void *mode) {
  GDI_GUARD;(void)mode;
  struct dc *d=gdi_dc(h);if(!d) return 0;
  /* A memory/display DC has no driver settings to reset. Printer settings
   * need a new printer DC; the current spool implementation cannot reset. */
  if(d->printer) {SetLastError(50);return 0;}
  return h;
}
int W32API DrawEscape(HDC h,int escape,int n,const char *in) {
  return ExtEscape(h,escape,n,in,0,0);
}
int W32API Escape(HDC h,int escape,int n,const char *in,void *out) {
  if(escape==1) return EndPage(h);
  if(escape==2) return AbortDoc(h);
  if(escape==11) return EndDoc(h);
  if(escape==9) return SetAbortProc(h,(int(W32API *)(HDC,int))in);
  if(escape==10) {
    if(n<0 || n>32767) return 0;
    char *name=in ? gdi_alloc((unsigned)n+1) : 0;
    if(in && !name) return 0;
    if(name) {w32_copy(name,in,(unsigned)n);name[n]=0;}
    DOCINFOA doc=out ? *(DOCINFOA *)out : (DOCINFOA){.cbSize=sizeof(doc)};
    doc.lpszDocName=name;
    int result=StartDocA(h,&doc);gdi_free(name,(unsigned)n+1);
    return result>0 ? StartPage(h) : result;
  }
  if(escape>=12 && escape<=14) {
    if(!out || !gdi_dc(h)) return 0;
    POINT *p=out;int cap=escape==12 ? 110 : escape==13 ? 112 : 114;
    p->x=GetDeviceCaps(h,cap);p->y=GetDeviceCaps(h,cap+1);return 1;
  }
  if(escape==8 && in && n>=2) {
    unsigned query=n>=4 ? w32_u32(in) : w32_u16(in);
    if(query==1 || query==2 || (query>=8 && query<=14)) return 1;
  }
  return ExtEscape(h,escape,n,in,0,out);
}
int W32API SetAbortProc(HDC h,int(W32API *proc)(HDC,int)) {
  GDI_GUARD;struct dc *d=gdi_dc(h);if(!d) return 0;
  d->abort_proc=proc;return 1;
}
BOOL W32API PlgBlt(HDC h,const POINT *p,HDC source,int sx,int sy,int w,int height,HBITMAP mask,int mx,int my) {
  GDI_GUARD;struct dc *d=gdi_dc(h),*s=gdi_dc(source);
  struct object *m=mask ? gdi_object(mask,GDI_BITMAP) : 0;
  if(!d || !s || !p || w<=0 || height<=0 || w>8192 || height>8192 ||
      sx< -GDI_LIMIT || sx>GDI_LIMIT || sy< -GDI_LIMIT || sy>GDI_LIMIT ||
      mx< -GDI_LIMIT || mx>GDI_LIMIT || my< -GDI_LIMIT || my>GDI_LIMIT ||
      (mask && (!m || m->u.bitmap.dib.bpp!=1))) return 0;
  POINT q[3];w32_copy(q,p,sizeof(q));if(!gdi_transform(d,q,3,0)) return 0;
  int64_t ax=q[1].x-q[0].x,ay=q[1].y-q[0].y,bx=q[2].x-q[0].x,by=q[2].y-q[0].y;
  int64_t det=ax*by-ay*bx;if(!det) return 0;
  RECT r={q[0].x,q[0].y,q[0].x,q[0].y};
  POINT fourth={q[1].x+q[2].x-q[0].x,q[1].y+q[2].y-q[0].y};
  for(unsigned i=0;i<4;i++) {POINT a=i<3 ? q[i] : fourth;
    if(a.x<r.left) r.left=a.x;if(a.x>r.right) r.right=a.x;
    if(a.y<r.top) r.top=a.y;if(a.y>r.bottom) r.bottom=a.y;
  }
  struct w32_dib *dst=gdi_target(d),*src=gdi_target(s);
  if(!dst || !src) return 0;
  if(r.left<0) r.left=0;if(r.top<0) r.top=0;
  if(r.right>(LONG)dst->width) r.right=(LONG)dst->width;
  if(r.bottom>(LONG)dst->height) r.bottom=(LONG)dst->height;
  /* Snapshot supports overlapping source/destination surfaces. */
  if((uint64_t)w*height>16u*1024u*1024u) return 0;
  DWORD bytes=(DWORD)w*(DWORD)height*4;
  DWORD *copy=gdi_alloc(bytes);if(!copy) return 0;
  for(int y=0;y<height;y++) for(int x=0;x<w;x++) {
    POINT a={sx+x,sy+y};
    copy[y*w+x]=gdi_transform(s,&a,1,0) && a.x>=0 && a.y>=0 && a.x<(LONG)src->width && a.y<(LONG)src->height ? w32_dib_rgb(src,a.x,a.y) : 0;
  }
  for(int y=r.top;y<r.bottom;y++) for(int x=r.left;x<r.right;x++) {
    int64_t rx=2*(x-q[0].x)+1,ry=2*(y-q[0].y)+1;
    int64_t u=rx*by-ry*bx,v=ax*ry-ay*rx,den=2*det;
    if(den<0) {den=-den;u=-u;v=-v;}
    if(u<0 || v<0 || u>=den || v>=den || !gdi_visible(d,x,y)) continue;
    int xx=(int)(u*w/den),yy=(int)(v*height/den);
    if(m) {
      int maskx=mx+xx,masky=my+yy;
      if(maskx<0 || masky<0 || maskx>=(LONG)m->u.bitmap.dib.width || masky>=(LONG)m->u.bitmap.dib.height || !w32_dib_raw(&m->u.bitmap.dib,maskx,masky)) continue;
    }
    w32_dib_put(dst,x,y,w32_dib_encode(dst,copy[yy*w+xx]));
  }
  gdi_free(copy,bytes);gdi_damage(d,r);return 1;
}
int W32API EnumObjects(HDC h,int kind,int(W32API *callback)(const void *,intptr_t),intptr_t user) {
  GDI_GUARD;
  if(!gdi_dc(h) || !callback || (kind!=OBJ_PEN && kind!=OBJ_BRUSH)) return 0;
  int result=1;
  if(kind==OBJ_PEN) {
    for(unsigned color=0;color<2;color++) for(unsigned style=0;style<=5;style++) {
      LOGPEN p={style,{1,0},color ? RGB(255,255,255) : 0};
      result=callback(&p,user);if(!result) return 0;
    }
  } else {
    static const COLORREF colors[]={0,0x808080,0xc0c0c0,0xffffff};
    for(unsigned i=0;i<4;i++) {
      LOGBRUSH b={BS_SOLID,colors[i],0};result=callback(&b,user);if(!result) return 0;
    }
    LOGBRUSH b={BS_NULL,0,0};result=callback(&b,user);
  }
  return result;
}
