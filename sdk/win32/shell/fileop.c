/* Bounded shell tree operations through the Win32 VFS APIs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
enum { MOVE=1,COPY=2,DELETE=3,RENAME=4 };
static const char *base(const char *p){const char *b=p;for(;*p;p++)if(*p=='\\' || *p=='/')b=p+1;return b;}
static int list(const char *s,const char **paths,unsigned *count){
  *count=0;if(!s)return 0;
  unsigned at=0;
  while(at<32768 && s[at]){
    if(*count==64)return 0;paths[(*count)++]=s+at;
    unsigned n=0;while(at+n<32768 && s[at+n])n++;
    if(!n || n>=260 || at+n+1>=32768)return 0;at+=n+1;
  }
  return at<32768 && *count;
}
static DWORD relation(const char *a,const char *b){
  char x[260],y[260];
  if(!GetFullPathNameA(a,260,x,0) || !GetFullPathNameA(b,260,y,0))return 0;
  DWORD n=w32_strlen(x),m=w32_strlen(y);
  while(n>3 && (x[n-1]=='\\' || x[n-1]=='/'))x[--n]=0;
  while(m>3 && (y[m-1]=='\\' || y[m-1]=='/'))y[--m]=0;
  if(n==m && !w32_stricmp(x,y))return 0x71;
  if(m>n && (y[n]=='\\' || y[n]=='/')){
    y[n]=0;if(!w32_stricmp(x,y))return 0x76;
  }
  return 0;
}
static DWORD tree(const char *src,const char *dst,unsigned op,unsigned flags,unsigned depth,struct shell_progress *progress){
  if(depth>32)return 0x79;
  if(!shell_progress_tick(progress,src))return 0x75;
  char checked[260];DWORD length=GetFullPathNameA(src,260,checked,0);
  if(!length)return GetLastError();if(length>=260)return 0x79;
  if(length==3 && checked[1]==':' && (checked[2]=='\\' || checked[2]=='/'))return 0x74;
  DWORD attr=GetFileAttributesA(src);if(attr==~0u)return GetLastError();
  char renamed[260];
  if(dst && (flags&8) && GetFileAttributesA(dst)!=~0u){
    char parent[260];if(!shell_copy(parent,260,dst))return 0x79;
    char *leaf=parent;for(char *p=parent;*p;p++)if(*p=='/' || *p=='\\')leaf=p+1;
    if(leaf==parent)return 0x7c;char original[260];if(!shell_copy(original,260,leaf))return 0x79;*leaf=0;
    unsigned attempt;
    for(attempt=1;attempt<=99;attempt++){
      char name[260]="Copy (";unsigned at=6;if(attempt>=10)name[at++]=(char)('0'+attempt/10);
      name[at++]=(char)('0'+attempt%10);w32_copy(name+at,") of ",6);at+=5;
      if(w32_strlen(original)>=sizeof(name)-at)return 0x79;w32_copy(name+at,original,w32_strlen(original)+1);
      if(!shell_join(renamed,parent,name))return 0x79;
      if(GetFileAttributesA(renamed)==~0u)break;
    }
    if(attempt>99)return 80;dst=renamed;
  }
  if(dst){DWORD r=relation(src,dst);if(r)return r;}
  if(op==MOVE && GetFileAttributesA(dst)==~0u){
    if(MoveFileA(src,dst))return 0;
    if(GetLastError()!=17)return GetLastError();
  }
  if(!(attr&16)){
    BOOL ok;
    if(op==DELETE)ok=DeleteFileA(src);
    else if(op==RENAME)ok=MoveFileA(src,dst);
    else {
      ok=CopyFileA(src,dst,0);
      if(ok && op==MOVE)ok=DeleteFileA(src);
    }
    return ok?0:GetLastError();
  }
  if(op==RENAME){
    char a[260],b[260];
    if(!GetFullPathNameA(src,260,a,0) || !GetFullPathNameA(dst,260,b,0))return 0x7c;
    char *pa=a,*pb=b;for(char *p=a;*p;p++)if(*p=='\\' || *p=='/')pa=p;
    for(char *p=b;*p;p++)if(*p=='\\' || *p=='/')pb=p;
    *pa=*pb=0;if(w32_stricmp(a,b))return 0x73;
    return MoveFileA(src,dst)?0:GetLastError();
  }
  if(op!=DELETE && !CreateDirectoryA(dst,0)){
    DWORD e=GetLastError(),a=GetFileAttributesA(dst);
    if((e!=183 && e!=80) || a==~0u || !(a&16))return e;
  }
  char pattern[260];if(!shell_join(pattern,src,"*"))return 0x79;
  WIN32_FIND_DATAA data;HANDLE find=FindFirstFileA(pattern,&data);
  DWORD result=0;
  if(find==K32_INVALID){DWORD e=GetLastError();if(e!=2 && e!=18)return e;}
  else {
    do {
      if(!w32_strcmp(data.name,".") || !w32_strcmp(data.name,".."))continue;
      if((flags&0x1000) && (data.attributes&16))continue;
      char from[260],to[260];
      if(!shell_join(from,src,data.name) || (dst && !shell_join(to,dst,data.name))){result=0x79;break;}
      result=tree(from,dst?to:0,op,flags,depth+1,progress);if(result)break;
    }while(FindNextFileA(find,&data));
    if(!result && GetLastError()!=18)result=GetLastError();
    FindClose(find);
  }
  if(!result && (op==DELETE || op==MOVE) && !RemoveDirectoryA(src))result=GetLastError();
  return result;
}
/* ---- Recycle Bin (G2, docs/design/recycle-bin.md) ----
 * FOF_ALLOWUNDO on FO_DELETE moves an item on a fixed drive into X:\RECYCLED as
 * D<drive><n>[.ext] and appends its Windows 9x INFO2 record (the same layout
 * apps/files/recycle.inc keeps), honouring the shared policy file. Items on other
 * drives, with a relative path, inside a bin, or when "do not move files" is set
 * are deleted at once as before. An item larger than the bin's limit fails with
 * ERROR_DISK_FULL instead of being lost. The limit is not enforced by purging here:
 * the next native delete trims the bin. */
DWORD W32API GetDriveTypeA(const char *);
#define RB_REC 280
#define RB_HDR 20
#define RB_ASSUMED (512u<<20)
struct rb_policy {int independent,nuke,dnuke[26];DWORD percent,dpercent[26];};
static int rb_key(const char *a,const char *end,const char *b){
  while(a<end && *b && *a==*b){a++;b++;}return a==end && !*b;
}
static DWORD rb_number(const char *v){DWORD n=0;while(*v>='0' && *v<='9' && n<1000)n=n*10+(DWORD)(*v++-'0');return n<1?1:n>100?100:n;}
static void rb_policy_read(struct rb_policy *p){
  w32_fill(p,0,sizeof(*p));p->percent=10;for(unsigned i=0;i<26;i++)p->dpercent[i]=10;
  HANDLE h=CreateFileA("C:\\system\\settings\\recycle.cfg",0x80000000u,1,0,3,0,0);
  if(h==K32_INVALID)return;
  char buf[2049];DWORD got=0;if(!ReadFile(h,buf,2048,&got,0))got=0;CloseHandle(h);buf[got]=0;
  for(char *line=buf;*line;){
    char *end=line;while(*end && *end!='\n' && *end!='\r')end++;
    char *eq=line;while(eq<end && *eq!='=')eq++;
    if(eq<end && line[0]!='#'){
      const char *v=eq+1;
      if(rb_key(line,eq,"independent"))p->independent=*v!='0';
      else if(rb_key(line,eq,"nuke"))p->nuke=*v!='0';
      else if(rb_key(line,eq,"percent"))p->percent=rb_number(v);
      else if(eq-line==12 && line[0]=='d' && line[1]=='r' && line[2]=='i' && line[3]=='v' && line[4]=='e' && line[5]=='.' && line[7]=='.'){
        char l=line[6]>='a' && line[6]<='z'?(char)(line[6]-32):line[6];
        if(l>='A' && l<='Z' && rb_key(line+8,eq,"nuke"))p->dnuke[l-'A']=*v!='0';
      }else if(eq-line==15 && line[0]=='d' && line[5]=='.' && line[7]=='.'){
        char l=line[6]>='a' && line[6]<='z'?(char)(line[6]-32):line[6];
        if(l>='A' && l<='Z' && rb_key(line+8,eq,"percent"))p->dpercent[l-'A']=rb_number(v);
      }
    }
    if(!*end)break;line=end+1;
  }
}
static DWORD rb_tree_bytes(const char *dir,unsigned depth,DWORD *total){
  if(depth>32)return 0;
  char pattern[260];if(!shell_join(pattern,dir,"*"))return 0;
  WIN32_FIND_DATAA data;HANDLE find=FindFirstFileA(pattern,&data);if(find==K32_INVALID)return 0;
  do{
    if(!w32_strcmp(data.name,".") || !w32_strcmp(data.name,".."))continue;
    if(data.attributes&16){char child[260];if(shell_join(child,dir,data.name))rb_tree_bytes(child,depth+1,total);}
    else if(*total+data.size_low>*total)*total+=data.size_low;
  }while(FindNextFileA(find,&data));
  FindClose(find);return 0;
}
static int rb_lock(const char *path){
  for(unsigned attempt=0;attempt<60;attempt++){
    HANDLE h=CreateFileA(path,0x40000000u,0,0,1,0,0);
    if(h!=K32_INVALID){CloseHandle(h);return 1;}
    Sleep(25);
  }
  return 0;
}
/* 0: not recycled (the caller deletes it); 1: recycled; else a Win32 error. */
static DWORD rb_recycle(const char *src,int *moved){
  *moved=0;
  char full[260];DWORD n=GetFullPathNameA(src,260,full,0);
  if(!n || n>=260 || full[1]!=':' || full[2]!='\\')return 0;
  char drive=full[0]>='a' && full[0]<='z'?(char)(full[0]-32):full[0];
  if(drive<'C' || drive>'Z')return 0;
  char root[4]={drive,':','\\',0};
  if(GetDriveTypeA(root)!=3)return 0;                        /* DRIVE_FIXED only: removable media delete at once */
  struct rb_policy p;rb_policy_read(&p);
  int di=drive-'A';
  if(p.independent?p.dnuke[di]:p.nuke)return 0;
  DWORD percent=p.independent?p.dpercent[di]:p.percent;
  char dir[16]={drive,':','\\','R','E','C','Y','C','L','E','D',0},info[40],lock[40],stored[48];
  {const char *bin="RECYCLED";unsigned k=0;
   while(k<8){char c=full[3+k];if(c>='a' && c<='z')c=(char)(c-32);if(c!=bin[k])break;k++;}
   if(k==8 && (full[11]==0 || full[11]=='\\'))return 0;}        /* inside a bin: delete for good */
  DWORD attr=GetFileAttributesA(full);if(attr==~0u)return GetLastError();
  DWORD size=0;
  if(attr&16)rb_tree_bytes(full,0,&size);
  else{HANDLE h=CreateFileA(full,0x80000000u,1,0,3,0,0);if(h!=K32_INVALID){size=GetFileSize(h,0);CloseHandle(h);}}
  if(size>(RB_ASSUMED/100)*percent)return 112;               /* too large for the bin: never lose it silently */
  if(!CreateDirectoryA(dir,0)){DWORD e=GetLastError();if(e!=183 && e!=80)return e;}
  if(!shell_join(info,dir,"INFO2") || !shell_join(lock,dir,"INFO2.lck"))return 0x7c;
  if(!rb_lock(lock))return 170;                              /* ERROR_BUSY */
  DWORD result=0;uint8_t *buf=0;DWORD have=0;
  HANDLE h=CreateFileA(info,0x80000000u,1,0,3,0,0);
  if(h!=K32_INVALID){have=GetFileSize(h,0);if(have>RB_HDR+RB_REC*4200u)have=0;}
  buf=shell_alloc(have+RB_HDR+RB_REC);
  if(!buf){if(h!=K32_INVALID)CloseHandle(h);DeleteFileA(lock);return 8;}
  w32_fill(buf,0,have+RB_HDR+RB_REC);
  if(h!=K32_INVALID){DWORD got=0;if(!ReadFile(h,buf,have,&got,0) || got!=have)have=0;CloseHandle(h);}
  if(have<RB_HDR || w32_u32(buf+12)!=RB_REC)have=RB_HDR;      /* absent or damaged: start a fresh list */
  DWORD count=(have-RB_HDR)/RB_REC,next=w32_u32(buf+4);
  for(DWORD i=0;i<count;i++){DWORD index=w32_u32(buf+RB_HDR+i*RB_REC+260);if(index>=next)next=index+1;}
  if(!next)next=1;
  /* D<drive><n>[.ext] */
  {unsigned at=0;stored[at++]='D';stored[at++]=(char)(drive+32);char digits[12];unsigned d=0;DWORD v=next;
   do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)stored[at++]=digits[--d];
   const char *leaf=full,*dot=0;for(const char *q=full;*q;q++){if(*q=='\\')leaf=q+1;}
   for(const char *q=leaf;*q;q++)if(*q=='.')dot=q;
   if(dot && dot!=leaf && dot[1] && w32_strlen(dot)<=17){while(*dot)stored[at++]=*dot++;}
   stored[at]=0;}
  char target[40];
  if(!shell_join(target,dir,stored)){result=0x7c;goto done;}
  if(!MoveFileA(full,target)){result=GetLastError();goto done;}
  {uint8_t *rec=buf+have;SYSTEMTIME st;FILETIME ft;GetSystemTime(&st);SystemTimeToFileTime(&st,&ft);
   w32_copy(rec,full,n);                                     /* at most 259 bytes, the rest stays zero */
   w32_put32(rec+260,next);w32_put32(rec+264,(DWORD)di);w32_put32(rec+268,ft.low);w32_put32(rec+272,ft.high);w32_put32(rec+276,size);
   w32_put32(buf,4);w32_put32(buf+4,next+1);w32_put32(buf+8,count+1);w32_put32(buf+12,RB_REC);}
  {HANDLE out=CreateFileA(info,0x40000000u,0,0,2,0,0);
   DWORD put=0,total=have+RB_REC;
   if(out==K32_INVALID || !WriteFile(out,buf,total,&put,0) || put!=total)result=GetLastError()?GetLastError():29;
   if(out!=K32_INVALID)CloseHandle(out);}
  if(!result)*moved=1;
done:
  shell_free(buf);DeleteFileA(lock);return result;
}
int W32API SHFileOperationA(SHELL_FILEOP *a){
  if(!a)return 87;
  a->aborted=0;a->mappings=0;
  if(a->operation<1 || a->operation>4)return 87;
  /* FOF_ALLOWUNDO: deletes go to the Recycle Bin (rb_recycle); the other
   * operations keep no undo information (G8). Name mappings are not allocated. */
  if(a->flags&0x20)return 120;
  const char *src[64],*dst[64];unsigned ns=0,nd=0;
  if(!list(a->from,src,&ns) || (a->operation!=DELETE && !list(a->to,dst,&nd)))return 0x7c;
  if(a->operation==RENAME && (ns!=1 || nd!=1))return 0x72;
  if(a->operation==RENAME){
    char from[260],to[260];
    DWORD n=GetFullPathNameA(src[0],260,from,0),m=GetFullPathNameA(dst[0],260,to,0);
    if(!n || !m || n>=260 || m>=260)return 0x7c;
    char *x=from,*y=to;for(char *p=from;*p;p++)if(*p=='\\' || *p=='/')x=p;
    for(char *p=to;*p;p++)if(*p=='\\' || *p=='/')y=p;
    *x=*y=0;if(w32_stricmp(from,to))return 0x73;
  }
  if(a->operation!=DELETE && ((a->flags&1)?ns!=nd:nd!=1))return 0x7a;
  if(!(a->flags&0x10)){
    int recycle=0;if(a->operation==DELETE && (a->flags&0x40)){
      char probe[260];DWORD pn=GetFullPathNameA(src[0],260,probe,0);
      if(pn && pn<260 && probe[1]==':'){char r[4]={probe[0],':','\\',0};recycle=GetDriveTypeA(r)==3;}
    }
    const char *text=a->operation==DELETE?(recycle?"Send the selected files to the Recycle Bin?":"Permanently delete the selected files?"):"Continue the file operation?";
    if(MessageBoxA(a->window,text,a->title?a->title:"File operation",0x21)!=1){a->aborted=1;return 0;}
  }
  struct shell_progress *progress=shell_progress_begin(a);DWORD result=0;
  for(unsigned i=0;i<ns;i++){
    char target[260];const char *to=0;
    if(a->operation!=DELETE){
      to=dst[(a->flags&1)?i:0];DWORD attr=GetFileAttributesA(to);
      if(a->operation!=RENAME && attr!=~0u && (attr&16)){
        if(!shell_join(target,to,base(src[i]))){result=0x79;break;}to=target;
      }else if(ns>1 && !(a->flags&1)){result=0x72;break;}
    }
    if(a->operation==DELETE && (a->flags&0x40)){
      int moved=0;result=rb_recycle(src[i],&moved);if(result)break;if(moved)continue;
    }
    result=tree(src[i],to,a->operation,a->flags,0,progress);if(result)break;
  }
  shell_progress_end(progress);if(result==0x75)a->aborted=1;return (int)result;
}
