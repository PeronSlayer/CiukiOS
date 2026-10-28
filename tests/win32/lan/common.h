/* Authored LAN qualification fixtures, no game payload. SPDX-License-Identifier: MIT */
#ifndef CIUKI_LAN_FIXTURE_H
#define CIUKI_LAN_FIXTURE_H
#include "windef.h"
#include "winbase.h"
void *memset(void *out,int value,size_t n) {unsigned char *p=out;while(n--)*p++=(unsigned char)value;return out;}
void *memcpy(void *out,const void *in,size_t n) {unsigned char *p=out;const unsigned char *q=in;while(n--)*p++=*q++;return out;}
int memcmp(const void *a,const void *b,size_t n) {const unsigned char *p=a,*q=b;while(n--){if(*p!=*q)return *p-*q;p++;q++;}return 0;}
static int option(const char *word) {const char *p=GetCommandLineA();for(;*p;p++){unsigned i=0;while(word[i]&&word[i]==p[i])i++;if(!word[i])return 1;}return 0;}
static void log_line(const char *text) {DWORD n=0,written;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&written,NULL);}
static void require(int success,const char *message) {if(!success){log_line(message);ExitProcess(1);}}
#endif
