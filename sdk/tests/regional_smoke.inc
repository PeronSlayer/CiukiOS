/* G24: real pinned newlib, host syscall filesystem only. Never writes the
 * guest's settings. Included in libc_smoke main by the host builder only.
 * SPDX-License-Identifier: MIT */
{
    CHECK(!strcmp(setlocale(LC_ALL,NULL),"C"));
    CHECK(setlocale(LC_ALL,"it_IT")!=NULL);
    CHECK(!strcmp(localeconv()->decimal_point,","));
    char text[256],*end;
    CHECK(snprintf(text,sizeof(text),"%.2f",1234.5)==7 && !strcmp(text,"1234,50"));
    CHECK(strtod("12,5tail",&end)==12.5 && !strcmp(end,"tail"));
    CHECK(strtod("0x1,8p0",&end)==1.5 && !*end);
    CHECK(strtof("2,5",&end)==2.5f && !*end);
    double value=0; CHECK(sscanf("12,5","%lf",&value)==1 && value==12.5);
    struct tm tm={.tm_year=126,.tm_mon=9,.tm_mday=5,.tm_wday=1,.tm_hour=13,.tm_min=4,.tm_sec=5};
    CHECK(strftime(text,sizeof(text),"%x %X %A %B",&tm)>0 && !strcmp(text,"05/10/2026 13:04:05 lunedì ottobre"));
    CHECK(!strcmp(localeconv()->currency_symbol,"EUR") && localeconv()->p_cs_precedes==0 && localeconv()->n_sign_posn==1);
    CHECK(setlocale(LC_NUMERIC,"C")!=NULL);
    CHECK(!strcmp(localeconv()->decimal_point,".") && !strcmp(localeconv()->currency_symbol,"EUR"));
    char composite[224]; strcpy(composite,setlocale(LC_ALL,NULL));
    CHECK(setlocale(LC_ALL,"en_US")!=NULL && setlocale(LC_ALL,composite)!=NULL);
    CHECK(!strcmp(setlocale(LC_TIME,NULL),"it_IT") && !strcmp(setlocale(LC_NUMERIC,NULL),"C"));
    CHECK(setlocale(LC_ALL,"bad")==NULL && !strcmp(setlocale(LC_ALL,NULL),composite));
    CHECK(setlocale(-1,"C")==NULL && errno==EINVAL);
    CHECK(setlocale(LC_ALL,"en_US")!=NULL);
    CHECK(strftime(text,sizeof(text),"%x %X",&tm)>0 && !strcmp(text,"10/5/26 1:04:05 PM"));
    char tiny[2]; CHECK(strftime(tiny,sizeof(tiny),"%-d",&tm)==1 && !strcmp(tiny,"5"));
    CHECK(strftime(tiny,1,"%-d",&tm)==0);
    extern int ciuki_host_regional_file(const char *);
    CHECK(!ciuki_host_regional_file("regional_locale=it_IT\nregional_date=yyyy-MM-dd\nregional_time=HH.mm.ss\nregional_currency=EURO\nregional_digits=3\nregional_grouping=0\n"));
    CHECK(!unsetenv("LC_ALL") && !unsetenv("LANG") && !setenv("LC_NUMERIC","en_US",1));
    CHECK(setlocale(LC_ALL,"")!=NULL);
    CHECK(!strcmp(setlocale(LC_TIME,NULL),"it_IT") && !strcmp(setlocale(LC_NUMERIC,NULL),"en_US"));
    CHECK(strftime(text,sizeof(text),"%x %X",&tm)>0 && !strcmp(text,"2026-10-05 13.04.05"));
    CHECK(!strcmp(localeconv()->currency_symbol,"EURO"));
    CHECK(!setenv("LC_ALL","it_IT",1) && !setenv("LC_TIME","C",1));
    CHECK(setlocale(LC_ALL,"")!=NULL && !strcmp(setlocale(LC_TIME,NULL),"it_IT"));
    CHECK(!strcmp(localeconv()->decimal_point,",") && !localeconv()->grouping[0]);
    CHECK(!ciuki_host_regional_file("regional_locale=it_IT\nregional_date=d/M/y\nregional_time=h:mm t\nregional_pm=pm\nregional_mon_negative=2\n"));
    CHECK(setlocale(LC_ALL,"")!=NULL);
    CHECK(strftime(text,sizeof(text),"%x %X",&tm)>0 && !strcmp(text,"5/10/26 1:04 p"));
    CHECK(localeconv()->n_cs_precedes==1 && localeconv()->n_sign_posn==4);
    CHECK(!ciuki_host_regional_file("regional_locale=it_IT\nregional_mon_negative=13\n"));
    CHECK(setlocale(LC_MONETARY,"")!=NULL && localeconv()->n_cs_precedes==0 && localeconv()->n_sign_posn==3);
    CHECK(!setenv("LC_ALL","invalid",1));
    CHECK(setlocale(LC_ALL,"")==NULL && !strcmp(setlocale(LC_TIME,NULL),"it_IT"));
    CHECK(!ciuki_host_regional_file(NULL));
    CHECK(!unsetenv("LC_NUMERIC") && !unsetenv("LC_TIME") && !unsetenv("LANG") && !setenv("LC_ALL","C",1));
    CHECK(setlocale(LC_ALL,"")!=NULL && !strcmp(setlocale(LC_ALL,NULL),"C"));
    CHECK(localeconv()->frac_digits==127 && !strcmp(localeconv()->decimal_point,"."));
    puts("PASS G24 native regional defaults overrides LC_* printf scanf strtod strftime C restoration");
}
