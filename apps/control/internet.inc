/* Internet Properties: General, History, Security, Connections and Programs. */
enum { I_HOME=1700,I_PROXY,I_PORT,I_CACHE,I_APPLY,I_DAYS,I_CLEAR,I_TRUSTED,I_RESTRICTED,
       I_SCRIPT0,I_SCRIPT1,I_SCRIPT2,I_SCRIPT3,I_USER,I_PASSWORD,I_BYPASS,I_DEFAULT,I_TAB0=1720 };
static int internet_capture(struct cp_app *a)
{
    struct cp_internet_options *o=&a->internet;char *end;
    if(a->internet_tab==0) {
        if(strlen(du_get(a->ui,I_HOME)->text)>=sizeof(o->home) || strlen(du_get(a->ui,I_PROXY)->text)>=sizeof(o->proxy))return -1;
        strcpy(o->home,du_get(a->ui,I_HOME)->text);strcpy(o->proxy,du_get(a->ui,I_PROXY)->text);
        const char *value=du_get(a->ui,I_PORT)->text;unsigned long n=strtoul(value,&end,10);
        if(!*value || *end || !n || n>65535)return -1;o->port=(unsigned)n;
        value=du_get(a->ui,I_CACHE)->text;n=strtoul(value,&end,10);if(!*value || *end || n>16)return -1;o->cache_mb=(unsigned)n;
    } else if(a->internet_tab==1) {
        const char *value=du_get(a->ui,I_DAYS)->text;unsigned long n=strtoul(value,&end,10);
        if(!*value || *end || n>999)return -1;o->history_days=(unsigned)n;
    } else if(a->internet_tab==2) {
        if(strlen(du_get(a->ui,I_TRUSTED)->text)>=sizeof(o->trusted) || strlen(du_get(a->ui,I_RESTRICTED)->text)>=sizeof(o->restricted))return -1;
        strcpy(o->trusted,du_get(a->ui,I_TRUSTED)->text);strcpy(o->restricted,du_get(a->ui,I_RESTRICTED)->text);
        for(unsigned i=0;i<4;i++)o->scripts[i]=!!du_get(a->ui,I_SCRIPT0+(int)i)->value;
    } else if(a->internet_tab==3) {
        if(strlen(du_get(a->ui,I_USER)->text)>=sizeof(o->proxy_user) || strlen(du_get(a->ui,I_PASSWORD)->text)>=sizeof(o->proxy_password))return -1;
        strcpy(o->proxy_user,du_get(a->ui,I_USER)->text);
        /* An unchanged placeholder keeps the saved password. */
        if(strcmp(du_get(a->ui,I_PASSWORD)->text,"********"))strcpy(o->proxy_password,du_get(a->ui,I_PASSWORD)->text);
        o->bypass_local=!!du_get(a->ui,I_BYPASS)->value;
    }
    return internet_valid(o)?0:-1;
}
static void internet_build(struct cp_app *a)
{
    if(!a->internet_loaded) {
        internet_defaults(&a->internet);if(a->host.internet_load)a->host.internet_load(a->host.ctx,&a->internet);
        a->internet_loaded=1;
    }
    char number[16];
    static const char *const pages[]={"General","History","Security","Connections","Programs"};
    for(unsigned i=0;i<5;i++)add(a,DU_BUTTON,I_TAB0+(int)i,24+(int)i*103,40,100,24,pages[i])->value=(int)i==a->internet_tab;
    switch(a->internet_tab) {
    case 0:
        add(a,DU_LABEL,1740,24,78,508,18,"Home page (http, https, file or about)");
        add(a,DU_EDIT,I_HOME,24,98,508,24,a->internet.home);
        add(a,DU_LABEL,1741,24,136,508,18,"Proxy host (blank connects directly), port");
        add(a,DU_EDIT,I_PROXY,24,158,380,24,a->internet.proxy);
        snprintf(number,sizeof(number),"%u",a->internet.port);add(a,DU_EDIT,I_PORT,416,158,116,24,number);
        add(a,DU_LABEL,1742,24,196,508,18,"Memory cache (0 to 16 MiB)");
        snprintf(number,sizeof(number),"%u",a->internet.cache_mb);add(a,DU_EDIT,I_CACHE,24,218,116,24,number);
        break;
    case 1:
        add(a,DU_LABEL,1740,24,92,508,18,"Days to keep pages in history (0 to 999)");
        snprintf(number,sizeof(number),"%u",a->internet.history_days);add(a,DU_EDIT,I_DAYS,24,118,100,24,number);
        add(a,DU_BUTTON,I_CLEAR,24,170,160,26,"Clear History");
        add(a,DU_LABEL,1741,24,212,508,18,"Clearing applies to every open browser window.");
        break;
    case 2: {
        add(a,DU_LABEL,1740,24,82,508,18,"Trusted sites (exact host names, separated by ;)");
        add(a,DU_EDIT,I_TRUSTED,24,104,508,24,a->internet.trusted);
        add(a,DU_LABEL,1741,24,142,508,18,"Restricted sites (take precedence over trusted sites)");
        add(a,DU_EDIT,I_RESTRICTED,24,164,508,24,a->internet.restricted);
        static const char *const zones[]={"Internet: active scripting","Local intranet: active scripting","Trusted sites: active scripting","Restricted sites: active scripting"};
        for(unsigned i=0;i<4;i++)add(a,DU_CHECKBOX,I_SCRIPT0+(int)i,24,202+(int)i*24,508,22,zones[i])->value=(int)a->internet.scripts[i];
        break;
    }
    case 3:
        add(a,DU_LABEL,1740,24,84,508,18,"LAN connection uses the proxy configured on General.");
        add(a,DU_LABEL,1741,24,116,508,18,"Proxy user name (Basic authentication)");
        add(a,DU_EDIT,I_USER,24,138,508,24,a->internet.proxy_user);
        add(a,DU_LABEL,1742,24,176,508,18,"Proxy password (leave the saved placeholder unchanged)");
        add(a,DU_EDIT,I_PASSWORD,24,198,508,24,a->internet.proxy_password[0]?"********":"");
        add(a,DU_CHECKBOX,I_BYPASS,24,238,508,22,"Bypass proxy for local intranet addresses")->value=(int)a->internet.bypass_local;
        break;
    case 4:
        add(a,DU_LABEL,1740,24,100,508,18,"Web browser: NetSurf");
        add(a,DU_BUTTON,I_DEFAULT,24,138,220,26,"Make NetSurf the default")->disabled=!a->host.internet_default;
        add(a,DU_LABEL,1741,24,186,508,18,a->host.internet_default?"Web and HTML file associations are shared with Files.":"Default program settings are not available.");
        break;
    }
    add(a,DU_LABEL,1749,24,306,508,18,"Saved settings apply to new browser windows.");
    add(a,DU_BUTTON,I_APPLY,24,330,128,26,"Apply");
}
static int internet_event(struct cp_app *a,const struct du_event *e)
{
    if(a->page!=CP_INTERNET || e->type!=DU_EV_ACTIVATE)return 0;
    if(e->id>=I_TAB0 && e->id<I_TAB0+5) {
        if(internet_capture(a))text(a,C_STATUS,"Enter valid Internet settings before changing pages.");
        else { a->internet_tab=e->id-I_TAB0;build(a); }
        return 1;
    }
    if(e->id==I_CLEAR) {
        if(!a->host.internet_clear_history || a->host.internet_clear_history(a->host.ctx))text(a,C_STATUS,"History could not be cleared.");
        else text(a,C_STATUS,"Browsing history cleared in all windows.");return 1;
    }
    if(e->id==I_DEFAULT) {
        text(a,C_STATUS,a->host.internet_default && !a->host.internet_default(a->host.ctx)?"NetSurf is the default browser.":"Default browser could not be changed.");return 1;
    }
    if(e->id!=I_APPLY)return 0;
    if(internet_capture(a))text(a,C_STATUS,"Enter valid home, proxy, history and security settings.");
    else if(!a->host.internet_save || a->host.internet_save(a->host.ctx,&a->internet))text(a,C_STATUS,"Internet settings could not be saved.");
    else text(a,C_STATUS,"Saved. Open a new browser window to apply.");
    return 1;
}
