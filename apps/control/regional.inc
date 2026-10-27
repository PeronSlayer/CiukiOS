/* G24: private snapshot, merged into the current settings only on Apply/OK.
 * SPDX-License-Identifier: MIT */
enum { C_REG_TABS=1800, C_REG_PRESET, C_REG_APPLY, C_REG_OK, C_REG_CANCEL,
       C_REG_FIELD=1820, C_REG_LABEL=1840, C_REG_NOTE=1860 };
static const char *const regional_labels[REG_FIELDS]={
    "Decimal symbol:","Digit grouping symbol:","Decimal digits (0-9):","Negative order (0-4):",
    "Grouping size (0-9):","Currency symbol:","Decimal symbol:","Digit grouping symbol:",
    "Decimal digits (0-9):","Negative order (0-15):","Positive order (0-3):",
    "Short date:","Long date:","Time format:","AM symbol:","PM symbol:","Measurement (0=metric, 1=US):"
};
static int regional_pull(struct cp_app *a)
{
    struct regional r=a->regional_edit;
    for(unsigned i=0;i<REG_FIELDS;i++) {
        struct du_widget *w=du_get(a->ui,C_REG_FIELD+(int)i);
        if(!w) continue;
        if(!reg_valid(i,w->text)) { text(a,C_STATUS,"Invalid regional format. Check the field and its allowed values."); du_focus(a->ui,w->id); return 0; }
        reg_copy(r.value[i],w->text);
    }
    if(!reg_consistent(&r)) { text(a,C_STATUS,"Decimal and grouping symbols must differ."); return 0; }
    a->regional_edit=r; return 1;
}
static void regional_build(struct cp_app *a)
{
    static const char *const tabs[]={"Region","Number","Currency","Time","Date"};
    for(unsigned i=0;i<5;i++) a->regional_tabs[i].text[0]=tabs[i];
    struct du_widget *w=du_get(a->ui,C_TABS);
    w->id=C_REG_TABS; w->items=a->regional_tabs; w->count=5; w->active=a->regional_tab;
    w=du_get(a->ui,C_CLOSE); w->id=C_REG_CANCEL; du_set_text(w,"Cancel");
    add(a,DU_BUTTON,C_REG_OK,CP_WIDTH-272,CP_HEIGHT-34,80,24,"OK")->def=1;
    add(a,DU_BUTTON,C_REG_APPLY,CP_WIDTH-184,CP_HEIGHT-34,88,24,"Apply");
    if(!a->regional_tab) {
        add(a,DU_LABEL,C_REG_NOTE,24,48,500,18,"Select a region to reset all formats to its defaults.");
        a->regional_presets[0].text[0]="English (United States)";
        a->regional_presets[1].text[0]="Italian (Italy)";
        w=add(a,DU_LISTVIEW,C_REG_PRESET,24,80,500,76,"");
        w->items=a->regional_presets; w->count=2; w->active=(unsigned)a->regional_edit.italian;
        w->column_count=1; w->columns[0]=(struct du_column){"Region",470,0}; w->view=DU_DETAILS;
        add(a,DU_LABEL,C_REG_NOTE+1,24,180,500,18,"UI language: English. Keyboard layout is set separately.");
        add(a,DU_LABEL,C_REG_LABEL+REG_MEASURE,24,222,254,18,regional_labels[REG_MEASURE]);
        add(a,DU_EDIT,C_REG_FIELD+REG_MEASURE,292,218,220,22,a->regional_edit.value[REG_MEASURE]);
    } else {
        unsigned first=a->regional_tab==1?REG_DECIMAL:a->regional_tab==2?REG_SYMBOL:a->regional_tab==3?REG_TIME:REG_DATE;
        unsigned end=a->regional_tab==1?REG_SYMBOL:a->regional_tab==2?REG_DATE:a->regional_tab==3?REG_MEASURE:REG_TIME;
        int y=48;
        for(unsigned i=first;i<end;i++,y+=34) {
            add(a,DU_LABEL,C_REG_LABEL+(int)i,24,y+4,250,18,regional_labels[i]);
            add(a,DU_EDIT,C_REG_FIELD+(int)i,280,y,232,22,a->regional_edit.value[i]);
        }
        const char *hint=a->regional_tab==1?"Negative: (1.1), -1.1, - 1.1, 1.1-, 1.1 - (orders 0-4).":
            a->regional_tab==2?"Positive: 0=$1, 1=1$, 2=$ 1, 3=1 $. $ is your symbol.":
            a->regional_tab==3?"H=24-hour, h=12-hour, m=minute, s=second, tt=AM/PM.":
            "d=day, M=month, y=year. Use quotes around literal words.";
        add(a,DU_LABEL,C_REG_NOTE,24,a->regional_tab==2?262:306,510,18,hint);
        if(a->regional_tab==2) {
            add(a,DU_LABEL,C_REG_NOTE+1,24,280,510,18,"Negative: 0=($1), 1=-$1, 2=$-1, 3=$1-, 4=(1$), 5=-1$");
            add(a,DU_LABEL,C_REG_NOTE+2,24,298,510,18,"6=1-$, 7=1$-, 8=-1 $, 9=-$ 1, 10=1 $-");
            add(a,DU_LABEL,C_REG_NOTE+3,24,316,510,18,"11=$ 1-, 12=$ -1, 13=1- $, 14=($ 1), 15=(1 $)");
            add(a,DU_LABEL,C_REG_NOTE+4,24,334,510,18,"Grouping size comes from Number (0 means no grouping).");
        }
    }
}
static void regional_save(struct cp_app *a,int accept)
{
    if(!regional_pull(a)) return;
    static struct ciuki_settings current; static char out[CFG_TEXT_MAX];
    if (load(a,&current)) return;
    current.regional=a->regional_edit;
    size_t n=cfg_format(&current,out,sizeof(out));
    if(!n || !a->host.save || a->host.save(a->host.ctx,out,n)) { text(a,C_STATUS,"Regional settings could not be saved."); return; }
    a->cfg_loaded=1;
    a->regional_accept=accept;
    a->regional_saving=a->host.save_async;
    if(a->host.apply) a->host.apply(a->host.ctx,DESK_SETTINGS_RELOAD,0,0);
    if(!a->regional_saving) { a->cfg.regional=a->regional_edit; a->regional_accept=0; if(accept) a->quit=1; }
    text(a,C_STATUS,a->regional_saving?"Saving regional settings...":"Regional settings saved.");
}
static int regional_event(struct cp_app *a,const struct du_event *e)
{
    if(a->page!=CP_REGIONAL) return 0;
    if(a->regional_saving && e->type!=DU_EV_TICK) return 1;
    if(e->type==DU_EV_KEY && e->key==DU_KEY_ENTER) { regional_save(a,1); return 1; }
    if(e->type==DU_EV_ACTIVATE && e->id==C_REG_CANCEL) { cp_close(a); return 1; }
    if(e->type==DU_EV_ACTIVATE && (e->id==C_REG_APPLY||e->id==C_REG_OK)) { regional_save(a,e->id==C_REG_OK); return 1; }
    if(e->type==DU_EV_SELECT && e->id==C_REG_TABS && e->index>=0 && e->index<5) {
        if(regional_pull(a)) { a->regional_tab=(unsigned)e->index; build(a); } return 1;
    }
    if(e->type==DU_EV_SELECT && e->id==C_REG_PRESET && e->index>=0 && e->index<2) {
        reg_defaults(&a->regional_edit,e->index); build(a); return 1;
    }
    return 0;
}
