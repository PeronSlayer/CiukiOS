/* G24 regional NLS regression, called by the production kernel32 harness.
 * SPDX-License-Identifier: MIT */
#include "../../../sdk/libciuki/regional_model.h"
extern int W32API GetLocaleInfoW(DWORD,DWORD,WCHAR *,int);
static int regional_active;
static struct regional regional_preferences;
void k32_regional_host(struct regional *r)
{ if(regional_active) *r=regional_preferences; }
static void regional_nls_tests(void)
{
  char a[256]; WCHAR w[256]; DWORD n;
  SYSTEMTIME d={.year=2026,.month=10,.day=5,.hour=13,.minute=4,.second=5};
  reg_defaults(&regional_preferences,1); regional_active=1;
  CHECK(!strcmp(reg_date_order("dddd, MMMM dd, yyyy"),"0"));
  CHECK(!strcmp(reg_date_order("dddd d MMMM yyyy"),"1"));
  CHECK(GetUserDefaultLCID()==0x410 && GetSystemDefaultLCID()==0x410 && GetThreadLocale()==0x410);
  CHECK(GetUserDefaultUILanguage()==0x409 && GetSystemDefaultUILanguage()==0x409);
  CHECK(GetUserDefaultLangID()==0x410 && IsValidLocale(0x410,1));
  CHECK(ConvertDefaultLocale(0x400)==0x410 && ConvertDefaultLocale(0x10)==0x410);
  CHECK(GetUserDefaultLocaleName(w,256)==6 && wide_equal(w,"it-IT"));
  CHECK(LocaleNameToLCID(W("it-IT"),0)==0x410 && LocaleNameToLCID(W("it"),0)==0x410);
  CHECK(LocaleNameToLCID(W("it"),0x08000000)==0x10);
  CHECK(LCIDToLocaleName(0x410,w,256,0)==6 && wide_equal(w,"it-IT"));
  CHECK(GetLocaleInfoEx(W("it"),0x5c,w,256)==3 && wide_equal(w,"it"));
  CHECK(GetLocaleInfoA(0x400,0x0e,a,256)==2 && !strcmp(a,","));
  CHECK(GetLocaleInfoA(0x409,0x0e,a,256)==2 && !strcmp(a,"."));
  CHECK(GetLocaleInfoA(0x410,0xa0000027,(char *)&n,4)==4 && n==1);
  CHECK(GetLocaleInfoA(0x410,0x20000001,(char *)&n,4)==4 && n==0x410);
  CHECK(GetLocaleInfoW(0x400,0x2000000b,(WCHAR *)&n,2)==2 && n==850);
  CHECK(GetLocaleInfoA(0x400,0x1f,0,0)==11);
  CHECK(!GetLocaleInfoA(0x400,0x1f,a,10) && last_error==122);
  CHECK(!GetLocaleInfoA(0x400,0x1f,NULL,3) && last_error==87);
  CHECK(GetDateFormatA(0x400,0,&d,0,a,256)==11 && !strcmp(a,"05/10/2026"));
  CHECK(GetDateFormatA(0x400,2,&d,0,a,256) && !strcmp(a,"luned\xec 5 ottobre 2026"));
  CHECK(GetDateFormatW(0x400,2,&d,0,w,256) && w[5]==0xec && w[6]==' ');
  CHECK(GetDateFormatA(0x409,0,&d,0,a,256) && !strcmp(a,"10/5/26"));
  CHECK(GetTimeFormatA(0x400,0,&d,0,a,256) && !strcmp(a,"13:04:05"));
  CHECK(GetNumberFormatA(0x400,0,"1234.5",0,a,256) && !strcmp(a,"1.234,50"));
  CHECK(GetCurrencyFormatA(0x400,0,"-1234.5",0,a,256) && !strcmp(a,"-1.234,50 EUR"));
  CHECK(GetNumberFormatW(0x400,0,W("1234.5"),0,w,256) && wide_equal(w,"1.234,50"));
  CHECK(GetCurrencyFormatW(0x400,0,W("1234.5"),0,w,256) && wide_equal(w,"1.234,50 EUR"));
  reg_copy(regional_preferences.value[REG_DATE],"yyyy-MM-dd");
  reg_copy(regional_preferences.value[REG_TIME],"h.mm tt");
  reg_copy(regional_preferences.value[REG_PM],"pm");
  reg_copy(regional_preferences.value[REG_DIGITS],"3");
  reg_copy(regional_preferences.value[REG_GROUPING],"0");
  reg_copy(regional_preferences.value[REG_SYMBOL],"Euro");
  reg_copy(regional_preferences.value[REG_MON_NEGATIVE],"15");
  CHECK(GetDateFormatA(0x410,0,&d,0,a,256) && !strcmp(a,"2026-10-05"));
  CHECK(GetDateFormatA(0x410,0x80000000,&d,0,a,256) && !strcmp(a,"05/10/2026"));
  CHECK(GetDateFormatW(0x400,0,&d,0,w,256)==11 && wide_equal(w,"2026-10-05"));
  CHECK(GetTimeFormatA(0x400,0,&d,0,a,256) && !strcmp(a,"1.04 pm"));
  CHECK(GetLocaleInfoA(0x400,0x1003,a,256)==8 && !strcmp(a,"h.mm tt"));
  CHECK(GetNumberFormatA(0x400,0,"1234.5",0,a,256) && !strcmp(a,"1234,500"));
  CHECK(GetNumberFormatA(0x400,0x80000000,"1234.5",0,a,256) && !strcmp(a,"1.234,50"));
  CHECK(GetCurrencyFormatA(0x400,0,"-1234.5",0,a,256) && !strcmp(a,"(1234,50 Euro)"));
  CHECK(GetCurrencyFormatA(0x400,0x80000000,"-1234.5",0,a,256) && !strcmp(a,"-1.234,50 EUR"));
  CHECK(GetLocaleInfoA(0x400,0x2000001c,(char *)&n,4)==4 && n==15);
  CHECK(GetLocaleInfoW(0x400,0x2000001c,(WCHAR *)&n,2)==2 && n==15);
  CHECK(GetLocaleInfoA(0x400,0x10,a,256)==2 && !strcmp(a,"0"));
  CHECK(!GetDateFormatA(0x400,3,&d,0,a,256) && last_error==1004);
  d.month=2; d.day=30;
  CHECK(!GetDateFormatA(0x400,0,&d,0,a,256) && last_error==87);
  regional_active=0;
  puts("PASS G24 regional NLS defaults overrides explicit LCID NOUSEROVERRIDE A/W buffers");
}
