/* Device-coordinate path brackets. Microsoft path APIs; Wine 10.0 semantics.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
/* MSVC FLOAT ABI marker; all calculations below remain integer-only. */
#ifndef W32_HOST_MODEL
int _fltused;
#endif
static int invalid(void) { SetLastError(1003); return 0; }
void gdi_path_destroy(struct gdi_path *p) {
  if (!p) return;
  gdi_free(p->points, p->capacity * sizeof(POINT));
  gdi_free(p->types, p->capacity);
  gdi_free(p, sizeof(*p));
}
void gdi_path_free(struct dc *d) {gdi_path_destroy(d->path);d->path=0;}
static int reserve(struct gdi_path *p, unsigned n) {
  if (n > 8192 - p->count) { SetLastError(8); return 0; }
  n += p->count;
  if (n <= p->capacity) return 1;
  unsigned cap = p->capacity ? p->capacity : 32;
  while (cap < n) cap *= 2;
  POINT *q = gdi_alloc(cap * sizeof(POINT));
  uint8_t *t = gdi_alloc(cap);
  if (!q || !t) {
    gdi_free(q, cap * sizeof(POINT)); gdi_free(t, cap);
    SetLastError(8); return 0;
  }
  w32_copy(q, p->points, p->count * sizeof(POINT));
  w32_copy(t, p->types, p->count);
  gdi_free(p->points, p->capacity * sizeof(POINT));
  gdi_free(p->types, p->capacity);
  p->points = q; p->types = t; p->capacity = cap;
  return 1;
}
struct gdi_path *gdi_path_clone(const struct gdi_path *source) {
  if(!source) return 0;
  struct gdi_path *p=gdi_alloc(sizeof(*p));if(!p) return 0;
  if(!reserve(p,source->count)) {gdi_path_destroy(p);return 0;}
  w32_copy(p->points,source->points,source->count*sizeof(POINT));
  w32_copy(p->types,source->types,source->count);
  p->count=source->count;p->open=source->open;p->fresh=source->fresh;return p;
}
BOOL W32API BeginPath(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d) return 0;
  if (!d->path) d->path = gdi_alloc(sizeof(*d->path));
  if (!d->path) { SetLastError(8); return 0; }
  d->path->count = 0; d->path->open = 1; d->path->fresh = 1;
  return 1;
}
BOOL W32API EndPath(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !d->path || !d->path->open) return invalid();
  d->path->open = 0;
  return 1;
}
BOOL W32API AbortPath(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d) return 0;
  gdi_path_free(d); return 1;
}
BOOL W32API CloseFigure(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !d->path || !d->path->open) return invalid();
  if (d->path->count) d->path->types[d->path->count - 1] |= 1;
  d->path->fresh = 1;
  return 1;
}
/* Input logical points; append atomically after validating and mapping them.
 * connect starts at the current position, otherwise starts a separate figure. */
int gdi_path_poly(HDC h, const POINT *points, unsigned n, int connect, int close) {
  struct dc *d = gdi_dc(h);
  struct gdi_path *p = d ? d->path : 0;
  if (!p || !p->open || !points || !n || n > 8191) return 0;
  unsigned start = connect && p->fresh;
  unsigned count = n + start;
  POINT *q = gdi_alloc(count * sizeof(POINT));
  if (!q) return 0;
  if (start) q[0] = d->s.current;
  w32_copy(q + start, points, n * sizeof(POINT));
  int ok = LPtoDP(h, q, (int)count) && reserve(p, count);
  if (ok) {
    for (unsigned i = 0; i < count; i++) {
      p->points[p->count] = q[i];
      p->types[p->count++] = (i == 0 && (!connect || start)) ? 6 : 2;
    }
    if (close) p->types[p->count - 1] |= 1;
    p->fresh = close;
  }
  gdi_free(q, count * sizeof(POINT));
  return ok;
}
int gdi_path_bezier(HDC h, const POINT *points, unsigned n, int connect) {
  struct dc *d = gdi_dc(h);
  unsigned old = d->path->count;
  int start = connect && d->path->fresh;
  if (!gdi_path_poly(h, points, n, connect, 0)) return 0;
  for (unsigned i = old + (!connect || start); i < d->path->count; i++)
    d->path->types[i] = 4;
  return 1;
}
int W32API GetPath(HDC h, POINT *points, uint8_t *types, int count) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !d->path || d->path->open) { invalid(); return -1; }
  struct gdi_path *p = d->path;
  if (!count) return (int)p->count;
  if (count < 0 || (unsigned)count < p->count || !points || !types) {
    SetLastError(87); return -1;
  }
  w32_copy(points, p->points, p->count * sizeof(POINT));
  if (!DPtoLP(h, points, (int)p->count)) return -1;
  w32_copy(types, p->types, p->count);
  return (int)p->count;
}
BOOL W32API FlattenPath(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !d->path || d->path->open) return invalid();
  struct gdi_path *p = d->path;
  struct gdi_path q = {0};
  for (unsigned i = 0; i < p->count; i++) {
    uint8_t type = p->types[i];
    if ((type & ~1u) != 4) {
      if (!reserve(&q, 1)) goto fail;
      q.points[q.count] = p->points[i]; q.types[q.count++] = type;
    } else {
      if (!i || i + 2 >= p->count || !reserve(&q, 32)) goto fail;
      POINT a = p->points[i-1], b = p->points[i], c = p->points[i+1], e = p->points[i+2];
      for (int64_t t = 1; t <= 32; t++) {
        int64_t s = 32-t;
        q.points[q.count] = (POINT){
          (LONG)((s*s*s*a.x+3*s*s*t*b.x+3*s*t*t*c.x+t*t*t*e.x)/32768),
          (LONG)((s*s*s*a.y+3*s*s*t*b.y+3*s*t*t*c.y+t*t*t*e.y)/32768)};
        q.types[q.count++] = 2;
      }
      q.types[q.count-1] |= p->types[i+2] & 1;
      i += 2;
    }
  }
  gdi_free(p->points, p->capacity*sizeof(POINT)); gdi_free(p->types,p->capacity);
  *p = q; return 1;
fail:
  gdi_free(q.points,q.capacity*sizeof(POINT)); gdi_free(q.types,q.capacity);
  return 0;
}
static HRGN region(HDC h) {
  struct dc *d = gdi_dc(h);
  if (!FlattenPath(h)) return 0;
  struct gdi_path *p = d->path;
  if (!p->count) return CreateRectRgn(0,0,0,0);
  int *counts = gdi_alloc(p->count * sizeof(int));
  if (!counts) return 0;
  unsigned n = 0;
  for (unsigned i=0;i<p->count;i++) {
    if (!i || (p->types[i]&~1u)==6) counts[n++] = 0;
    counts[n-1]++;
  }
  HRGN r = CreatePolyPolygonRgn(p->points,counts,(int)n,d->s.fill);
  gdi_free(counts,p->count*sizeof(int)); return r;
}
HRGN W32API PathToRegion(HDC h) {
  GDI_GUARD;
  HRGN r = region(h);
  if (r) gdi_path_free(gdi_dc(h));
  return r;
}
BOOL W32API SelectClipPath(HDC h, int mode) {
  GDI_GUARD;
  if (mode < 1 || mode > 5) { SetLastError(87); return 0; }
  HRGN r = region(h);
  if (!r) return 0;
  BOOL ok = ExtSelectClipRgn(h,r,mode) != 0;
  DeleteObject(r);
  if (ok) gdi_path_free(gdi_dc(h));
  return ok;
}
static BOOL render(HDC h, int fill, int stroke) {
  struct dc *d = gdi_dc(h);
  if (!d || !FlattenPath(h)) return 0;
  HRGN r = fill ? region(h) : 0;
  if (fill && !r) return 0;
  struct gdi_path *p = d->path;
  if(d->record) {
    int saved=SaveDC(h);
    if(!saved) {if(r) DeleteObject(r);return 0;}
    SetMapMode(h,MM_TEXT);SetWindowOrgEx(h,0,0,0);SetViewportOrgEx(h,0,0,0);
    if(!stroke) SelectObject(h,GetStockObject(NULL_PEN));
    if(!fill) SelectObject(h,GetStockObject(NULL_BRUSH));
    BOOL ok=1;
    if(fill) {
      int *counts=gdi_alloc(p->count*sizeof(int));
      if(!counts && p->count) ok=0;
      unsigned n=0;
      for(unsigned i=0;ok && i<p->count;i++) {
        if(!i || (p->types[i]&~1u)==6) counts[n++]=0;
        counts[n-1]++;
      }
      if(ok && n) ok=PolyPolygon(h,p->points,counts,(int)n);
      gdi_free(counts,p->count*sizeof(int));
    } else for(unsigned i=0;ok && i<p->count;) {
      unsigned end=i+1;while(end<p->count && (p->types[end]&~1u)!=6) end++;
      if(end-i>1) ok=Polyline(h,p->points+i,(int)(end-i));
      if(ok && (p->types[end-1]&1)) {
        POINT line[2]={p->points[end-1],p->points[i]};ok=Polyline(h,line,2);
      }
      i=end;
    }
    RestoreDC(h,saved);if(r) DeleteObject(r);if(ok) gdi_path_free(d);return ok;
  }
  int map=d->s.map;
  POINT wo=d->s.window_org, vo=d->s.viewport_org;
  SIZE we=d->s.window_ext, ve=d->s.viewport_ext;
  d->s.map=MM_TEXT; d->s.window_org=d->s.viewport_org=(POINT){0,0};
  d->s.window_ext=d->s.viewport_ext=(SIZE){1,1};
  p->open=0;
  BOOL ok = !fill || FillRgn(h,r,d->s.brush);
  if (stroke) for(unsigned i=0;i<p->count && ok;) {
    unsigned end=i+1;
    while(end<p->count && (p->types[end]&~1u)!=6) end++;
    if(end-i>1) ok=Polyline(h,p->points+i,(int)(end-i));
    if(ok && (p->types[end-1]&1)) {
      POINT line[2]={p->points[end-1],p->points[i]};
      ok=Polyline(h,line,2);
    }
    i=end;
  }
  d->s.map=map;d->s.window_org=wo;d->s.viewport_org=vo;
  d->s.window_ext=we;d->s.viewport_ext=ve;
  if(r) DeleteObject(r);
  if(ok) gdi_path_free(d);
  return ok;
}
BOOL W32API FillPath(HDC h) { GDI_GUARD; return render(h,1,0); }
BOOL W32API StrokePath(HDC h) { GDI_GUARD; return render(h,0,1); }
BOOL W32API StrokeAndFillPath(HDC h) { GDI_GUARD; return render(h,1,1); }
BOOL W32API PolylineTo(HDC h,const POINT *p,DWORD n) {
  GDI_GUARD;
  struct dc *d=gdi_dc(h);
  if(!d || !p || !n || n>8191) return 0;
  if(d->path && d->path->open) {
    if(!gdi_path_poly(h,p,n,1,0)) return 0;
  } else {
    POINT *q=gdi_alloc((n+1)*sizeof(POINT));
    if(!q) return 0;
    q[0]=d->s.current;w32_copy(q+1,p,n*sizeof(POINT));
    BOOL ok=Polyline(h,q,(int)n+1);gdi_free(q,(n+1)*sizeof(POINT));
    if(!ok) return 0;
  }
  d->s.current=p[n-1];return 1;
}
BOOL W32API PolyBezierTo(HDC h,const POINT *p,DWORD n) {
  GDI_GUARD;
  struct dc *d=gdi_dc(h);
  if(!d || !p || !n || n>765 || n%3) return 0;
  if(d->path && d->path->open) {
    if(!gdi_path_bezier(h,p,n,1)) return 0;
  } else {
    POINT *q=gdi_alloc((n+1)*sizeof(POINT));if(!q) return 0;
    q[0]=d->s.current;w32_copy(q+1,p,n*sizeof(POINT));
    BOOL ok=PolyBezier(h,q,n+1);gdi_free(q,(n+1)*sizeof(POINT));if(!ok) return 0;
  }
  d->s.current=p[n-1];return 1;
}
BOOL W32API PolyDraw(HDC h,const POINT *p,const uint8_t *types,int n) {
  GDI_GUARD;
  struct dc *d=gdi_dc(h);
  if(!d || !p || !types || n<0 || n>8192) return 0;
  /* Validate the entire stream before changing the DC. */
  for(int i=0;i<n;i++) {
    unsigned t=types[i]&~1u;
    if(t==6) { if(types[i]&1) return 0; }
    else if(t==4) {
      if(i+2>=n || types[i]!=4 || types[i+1]!=4 || (types[i+2]&~1u)!=4) return 0;
      i+=2;
    } else if(t!=2) return 0;
  }
  POINT first=d->s.current;
  for(int i=0;i<n;i++) {
    unsigned t=types[i]&~1u;
    if(t==6) { first=p[i]; if(!MoveToEx(h,p[i].x,p[i].y,0)) return 0; }
    else if(t==4) { if(!PolyBezierTo(h,p+i,3)) return 0; i+=2; }
    else if(!LineTo(h,p[i].x,p[i].y)) return 0;
    if(types[i]&1) {
      if(d->path && d->path->open) { if(!CloseFigure(h)) return 0; }
      else if(!LineTo(h,first.x,first.y)) return 0;
    }
  }
  return 1;
}
BOOL W32API ArcTo(HDC h,int l,int t,int r,int b,int sx,int sy,int ex,int ey) {
  return gdi_arc(h,l,t,r,b,sx,sy,ex,ey,1);
}
BOOL W32API Chord(HDC h,int l,int t,int r,int b,int sx,int sy,int ex,int ey) {
  return gdi_arc(h,l,t,r,b,sx,sy,ex,ey,2);
}
BOOL W32API Pie(HDC h,int l,int t,int r,int b,int sx,int sy,int ex,int ey) {
  return gdi_arc(h,l,t,r,b,sx,sy,ex,ey,3);
}
int W32API GetArcDirection(HDC h) {
  GDI_GUARD;struct dc *d=gdi_dc(h);return d ? d->s.arc_direction : 0;
}
int W32API SetArcDirection(HDC h,int direction) {
  GDI_GUARD;struct dc *d=gdi_dc(h);
  if(!d || (direction!=1 && direction!=2)) { SetLastError(87);return 0; }
  int old=d->s.arc_direction;d->s.arc_direction=direction;return old;
}
/* IEEE single precision in fixed point, without introducing x87/SSE opcodes. */
static int fixed_float(uint32_t bits,int32_t *out) {
  unsigned e=(bits>>23)&255;
  if(e==255 || e>141) return 0;
  uint64_t m=(bits&0x7fffffu)|(e ? 0x800000u : 0);
  int shift=(int)e-134;
  int64_t v=shift>=0 ? (int64_t)(m<<shift) : -shift>=63 ? 0 : (int64_t)(m>>-shift);
  if(bits>>31) v=-v;
  if(v<INT32_MIN || v>INT32_MAX) return 0;
  *out=(int32_t)v;return 1;
}
BOOL W32API GetMiterLimit(HDC h,float *out) {
  GDI_GUARD;struct dc *d=gdi_dc(h);if(!d || !out) return 0;
  w32_copy(out,&d->s.miter_bits,4);return 1;
}
BOOL W32API SetMiterLimit(HDC h,float value,float *old) {
  GDI_GUARD;struct dc *d=gdi_dc(h);int32_t v;
  uint32_t bits=w32_u32(&value);
  if(!d || !fixed_float(bits,&v) || v<65536) {SetLastError(87);return 0;}
  if(old) w32_copy(old,&d->s.miter_bits,4);
  d->s.miter_bits=bits;return 1;
}
/* Integer CORDIC rotation. Angles are degrees in 16.16. */
static POINT circle_point(int x,int y,unsigned radius,int32_t angle) {
  static const int32_t atan_degrees[16]={2949120,1740967,919879,466945,234379,117304,58666,29335,14668,7334,3667,1833,917,458,229,115};
  int64_t z=angle%(360*65536ll),cx=39797,sy=0;
  if(z>180*65536ll) z-=360*65536ll;
  if(z< -180*65536ll) z+=360*65536ll;
  int sign=1;
  if(z>90*65536ll) {z-=180*65536ll;sign=-1;}
  if(z< -90*65536ll) {z+=180*65536ll;sign=-1;}
  for(unsigned i=0;i<16;i++) {
    int s=z>=0 ? 1 : -1;
    int64_t next=cx-s*(sy>>i);sy+=s*(cx>>i);cx=next;z-=s*atan_degrees[i];
  }
  return (POINT){x+(LONG)(sign*cx*radius/65536),y-(LONG)(sign*sy*radius/65536)};
}
BOOL W32API AngleArc(HDC h,int x,int y,DWORD radius,float start,float sweep) {
  GDI_GUARD;
  int32_t a,b;
  if(!gdi_dc(h) || radius>GDI_LIMIT || !fixed_float(w32_u32(&start),&a) || !fixed_float(w32_u32(&sweep),&b)) return 0;
  unsigned n=(unsigned)((b<0 ? -(int64_t)b : b)/(3*65536))+1;
  if(n>8190) return 0;
  POINT *p=gdi_alloc((n+1)*sizeof(POINT));if(!p) return 0;
  for(unsigned i=0;i<=n;i++) p[i]=circle_point(x,y,radius,(int32_t)((int64_t)a+(int64_t)b*i/n));
  BOOL ok=PolylineTo(h,p,n+1);gdi_free(p,(n+1)*sizeof(POINT));return ok;
}
static unsigned root64(uint64_t n) {
  uint64_t a=0,bit=1ull<<62;
  while(bit>n) bit>>=2;
  while(bit) {if(n>=a+bit) {n-=a+bit;a=(a>>1)+bit;}else a>>=1;bit>>=2;}
  return (unsigned)a;
}
static int outline_poly(struct gdi_path *p,const POINT *v,unsigned n) {
  if(!reserve(p,n)) return 0;
  for(unsigned i=0;i<n;i++) {p->points[p->count]=v[i];p->types[p->count++]=i ? 2 : 6;}
  p->types[p->count-1]|=1;return 1;
}
/* Convert the union of widened pieces to boundary contours. Keeping only
 * external edges makes the result correct under both ALTERNATE and WINDING. */
struct edge_event {int axis,c,t,delta;};
struct path_edge {POINT a,b;int used;};
static int event_less(struct edge_event a,struct edge_event b) {
  return a.axis!=b.axis ? a.axis<b.axis : a.c!=b.c ? a.c<b.c : a.t<b.t;
}
static void event_sift(struct edge_event *v,unsigned n,unsigned at) {
  for(;;) {
    unsigned child=at*2+1;if(child>=n) return;
    if(child+1<n && event_less(v[child],v[child+1])) child++;
    if(!event_less(v[at],v[child])) return;
    struct edge_event swap=v[at];v[at]=v[child];v[child]=swap;at=child;
  }
}
static void sort_events(struct edge_event *v,unsigned n) {
  for(unsigned i=n/2;i;i--) event_sift(v,n,i-1);
  for(unsigned i=n;i>1;i--) {
    struct edge_event swap=v[0];v[0]=v[i-1];v[i-1]=swap;event_sift(v,i-1,0);
  }
}
static int point_equal(POINT a,POINT b) {return a.x==b.x && a.y==b.y;}
static int widened_union(struct gdi_path *path) {
  if(!path->count) return 1;
  int *counts=gdi_alloc(path->count*sizeof(int));if(!counts) return 0;
  unsigned n=0;
  for(unsigned i=0;i<path->count;i++) {
    if(!i || (path->types[i]&~1u)==6) counts[n++]=0;
    counts[n-1]++;
  }
  HRGN h=CreatePolyPolygonRgn(path->points,counts,(int)n,WINDING);
  gdi_free(counts,path->count*sizeof(int));
  struct object *object=gdi_object(h,GDI_REGION);if(!object) return 0;
  unsigned rects=object->u.region.count;
  if(rects>2048) {DeleteObject(h);SetLastError(8);return 0;}
  unsigned cap=rects*8;
  struct edge_event *events=cap ? gdi_alloc(cap*sizeof(*events)) : 0;
  struct path_edge *edges=cap ? gdi_alloc(cap*sizeof(*edges)) : 0;
  struct gdi_path out={0};int good=!cap || (events && edges);
  if(!good) goto done;
  for(unsigned i=0;i<rects;i++) {
    RECT r=object->u.region.rects[i];
    struct edge_event v[8]={{0,r.top,r.left,1},{0,r.top,r.right,-1},
      {0,r.bottom,r.left,-1},{0,r.bottom,r.right,1},
      {1,r.right,r.top,1},{1,r.right,r.bottom,-1},
      {1,r.left,r.top,-1},{1,r.left,r.bottom,1}};
    w32_copy(events+i*8,v,sizeof(v));
  }
  sort_events(events,cap);
  unsigned total=0;
  for(unsigned i=0;i<cap;) {
    int axis=events[i].axis,c=events[i].c,previous=events[i].t,sum=0;
    while(i<cap && events[i].axis==axis && events[i].c==c) {
      int t=events[i].t;
      if(sum && t!=previous) {
        POINT a=axis ? (POINT){c,previous} : (POINT){previous,c};
        POINT b=axis ? (POINT){c,t} : (POINT){t,c};
        edges[total++]=(struct path_edge){sum>0 ? a : b,sum>0 ? b : a,0};
      }
      do {sum+=events[i++].delta;} while(i<cap && events[i].axis==axis && events[i].c==c && events[i].t==t);
      previous=t;
    }
  }
  for(unsigned first=0;first<total;first++) if(!edges[first].used) {
    POINT start=edges[first].a,cursor=start;unsigned at=first,begin=out.count;
    if(!reserve(&out,1)) {good=0;goto done;}
    out.points[out.count]=start;out.types[out.count++]=6;
    for(unsigned steps=0;;steps++) {
      if(steps>=total) {good=0;goto done;}
      edges[at].used=1;cursor=edges[at].b;
      if(point_equal(cursor,start)) break;
      if(out.count>begin+1 &&
          ((out.points[out.count-2].x==out.points[out.count-1].x && out.points[out.count-1].x==cursor.x) ||
           (out.points[out.count-2].y==out.points[out.count-1].y && out.points[out.count-1].y==cursor.y)))
        out.points[out.count-1]=cursor;
      else {
        if(!reserve(&out,1)) {good=0;goto done;}
        out.points[out.count]=cursor;out.types[out.count++]=2;
      }
      for(at=0;at<total;at++) if(!edges[at].used && point_equal(edges[at].a,cursor)) break;
      if(at==total) {good=0;goto done;}
    }
    out.types[out.count-1]|=1;
  }
done:
  gdi_free(events,cap*sizeof(*events));gdi_free(edges,cap*sizeof(*edges));DeleteObject(h);
  if(good) {
    gdi_free(path->points,path->capacity*sizeof(POINT));gdi_free(path->types,path->capacity);*path=out;
  } else {gdi_free(out.points,out.capacity*sizeof(POINT));gdi_free(out.types,out.capacity);}
  return good;
}
BOOL W32API WidenPath(HDC h) {
  GDI_GUARD;
  struct dc *d=gdi_dc(h);
  if(!d || !FlattenPath(h)) return 0;
  struct object *pen=gdi_object(d->s.pen,GDI_PEN);
  if(!pen || pen->u.pen.lopnStyle==PS_NULL || pen->u.pen.lopnWidth.x<=1) return invalid();
  unsigned radius=(unsigned)pen->u.pen.lopnWidth.x/2;
  struct gdi_path q={0},*p=d->path;
  for(unsigned first=0;first<p->count;) {
    unsigned end=first+1;while(end<p->count && (p->types[end]&~1u)!=6) end++;
    for(unsigned i=first;i<end;i++) {
      POINT cap[16];
      for(unsigned j=0;j<16;j++) cap[j]=circle_point(p->points[i].x,p->points[i].y,radius,(int32_t)(-(int64_t)j*360*65536/16));
      if(!outline_poly(&q,cap,16)) goto fail;
      unsigned next=i+1;
      if(next==end) {if(!(p->types[i]&1)) continue;next=first;}
      POINT a=p->points[i],b=p->points[next];
      int64_t dx=b.x-a.x,dy=b.y-a.y;
      unsigned length=root64((uint64_t)(dx*dx+dy*dy));if(!length) continue;
      LONG nx=(LONG)(dy*radius/length),ny=(LONG)(-dx*radius/length);
      POINT v[4]={{a.x+nx,a.y+ny},{b.x+nx,b.y+ny},{b.x-nx,b.y-ny},{a.x-nx,a.y-ny}};
      if(!outline_poly(&q,v,4)) goto fail;
    }
    first=end;
  }
  if(!widened_union(&q)) goto fail;
  gdi_free(p->points,p->capacity*sizeof(POINT));gdi_free(p->types,p->capacity);
  *p=q;return 1;
fail:
  gdi_free(q.points,q.capacity*sizeof(POINT));gdi_free(q.types,q.capacity);return 0;
}
int gdi_path_shape(HDC h,int l,int t,int r,int b,int rw,int rh) {
  struct dc *d=gdi_dc(h);
  if(l< -GDI_LIMIT || r>GDI_LIMIT || t< -GDI_LIMIT || b>GDI_LIMIT) return 0;
  if(l>=r || t>=b) return 1;
  if(rw<=0 || rh<=0) return Rectangle(h,l,t,r,b);
  if(rw>r-l) rw=r-l;if(rh>b-t) rh=b-t;
  int rx=rw/2,ry=rh/2,kx=(int)((int64_t)rx*36195/65536),ky=(int)((int64_t)ry*36195/65536);
  r--;b--;
  POINT v[17]={{l+rx,t},{r-rx,t},
    {r-rx+kx,t},{r,t+ry-ky},{r,t+ry},{r,b-ry},
    {r,b-ry+ky},{r-rx+kx,b},{r-rx,b},{l+rx,b},
    {l+rx-kx,b},{l,b-ry+ky},{l,b-ry},{l,t+ry},
    {l,t+ry-ky},{l+rx-kx,t},{l+rx,t}};
  unsigned old=d->path->count;
  if(!gdi_path_poly(h,v,17,0,1)) return 0;
  for(unsigned corner=0;corner<4;corner++) for(unsigned i=0;i<3;i++)
    d->path->types[old+2+corner*4+i]=4;
  d->path->types[old+16]|=1;return 1;
}
