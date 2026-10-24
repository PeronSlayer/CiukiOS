/* G24 production Control Panel and settings parser. SPDX-License-Identifier: MIT */
static void regional_select(int id,int index)
{ struct du_event e={.type=DU_EV_SELECT,.id=id,.index=index}; u.event(&u,&e,u.user); }
static void regional_page(void)
{
    static struct ciuki_settings settings;
    cfg_defaults(&settings); strcpy(settings.extra,"future_key=kept\n");
    saved_n=cfg_format(&settings,saved,sizeof(saved));
    assert(saved_n && cp_page_index("regional")==CP_REGIONAL);
    attach(CP_REGIONAL);
    assert(!a.regional_edit.italian && du_get(&u,1800)->count==5);
    unsigned before=saves;
    regional_select(1801,1); assert(a.regional_edit.italian && saves==before);
    for(int tab=0;tab<5;tab++) {
        regional_select(1800,tab); du_render(&u);
        for(unsigned i=0;i<REG_FIELDS;i++) {
            struct du_widget *w=du_get(&u,1820+(int)i);
            if(w) assert(w->box.x>=24 && w->box.y>=44 && w->box.x+w->box.w<=CP_WIDTH-24 && w->box.y+w->box.h<CP_HEIGHT-64);
        }
    }
    regional_select(1800,1); du_set_text(du_get(&u,1820+REG_DIGITS),"3");
    du_set_text(du_get(&u,1820+REG_DECIMAL),".");
    at_click_widget(&u,1802); assert(saves==before && strstr(du_get(&u,3)->text,"must differ"));
    du_set_text(du_get(&u,1820+REG_DECIMAL),",");
    du_set_text(du_get(&u,1820+REG_THOUSAND)," ");
    /* An unrelated edit after the panel opened must survive Apply. */
    settings.mouse_speed=9; settings.mouse_swap=1; settings.pointer_scheme=2; settings.repeat_delay=3; saved_n=cfg_format(&settings,saved,sizeof(saved));
    at_click_widget(&u,1802); assert(saves==before+1 && !a.quit);
    cfg_defaults(&settings); assert(!cfg_parse(&settings,saved,saved_n));
    assert(settings.regional.italian && !strcmp(settings.regional.value[REG_DIGITS],"3"));
    assert(settings.mouse_swap==1 && settings.pointer_scheme==2 && settings.repeat_delay==3);
    assert(!strcmp(settings.regional.value[REG_THOUSAND]," ") && settings.mouse_speed==9 && strstr(settings.extra,"future_key=kept"));
    regional_select(1800,4); du_set_text(du_get(&u,1820+REG_DATE),"yyyy-MM-dd");
    regional_select(1800,3); du_set_text(du_get(&u,1820+REG_TIME),"HH.mm.ss");
    at_click_widget(&u,1804); assert(a.quit && saves==before+1); cp_detach(&a);
    attach(CP_REGIONAL); assert(!strcmp(a.regional_edit.value[REG_DATE],"dd/MM/yyyy"));
    regional_select(1800,4); du_set_text(du_get(&u,1820+REG_DATE),"yyyy-%x");
    before=saves; at_click_widget(&u,1803); assert(saves==before && !a.quit);
    du_set_text(du_get(&u,1820+REG_DATE),"yyyy-MM-dd"); save_error=1;
    at_click_widget(&u,1803); assert(!a.quit && !a.regional_saving && saves==before);
    save_error=0; a.host.save_async=1; at_click_widget(&u,1803);
    assert(a.regional_saving && !a.quit && a.regional_accept);
    cp_open(&a,CP_HOME); assert(a.page==CP_REGIONAL); assert(!cp_close(&a));
    cp_settings_saved(&a,EROFS); assert(!a.quit && !a.regional_saving && !a.regional_accept);
    du_dialog_close(&u); at_click_widget(&u,1803);
    cp_settings_saved(&a,0); assert(a.quit); cp_detach(&a);
    attach(CP_REGIONAL); assert(!strcmp(a.regional_edit.value[REG_DATE],"yyyy-MM-dd")); cp_detach(&a);
    /* Parser: bad keys, malformed pictures, control bytes and conflicting
     * symbols retain safe values; unknown keys survive a round trip. */
    cfg_defaults(&settings);
    const char *bad="regional_locale=it_IT\nregional_digits=99\nregional_date=yyyy-%x\nregional_time=HH:mm 'bad\nregional_currency=bad\001\nregional_decimal=.\nregional_thousand=.\n";
    assert(cfg_parse(&settings,bad,strlen(bad))==4);
    assert(reg_consistent(&settings.regional) && !strcmp(settings.regional.value[REG_DIGITS],"2"));
    assert(!settings.regional.value[REG_THOUSAND][0]);
    puts("PASS G24 Regional Settings: defaults, edits, merge, Apply/Cancel/OK, failures, validation, round trip");
}
