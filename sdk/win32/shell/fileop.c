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
int W32API SHFileOperationA(SHELL_FILEOP *a){
  if(!a)return 87;
  a->aborted=0;a->mappings=0;
  if(a->operation<1 || a->operation>4)return 87;
  /* No recycle-bin service exists. Do not silently turn ALLOWUNDO into a
   * permanent delete, or promise name mappings we did not allocate. */
  if(a->flags&(0x40|0x20))return 120;
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
    const char *text=a->operation==DELETE?"Permanently delete the selected files?":"Continue the file operation?";
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
    result=tree(src[i],to,a->operation,a->flags,0,progress);if(result)break;
  }
  shell_progress_end(progress);if(result==0x75)a->aborted=1;return (int)result;
}
