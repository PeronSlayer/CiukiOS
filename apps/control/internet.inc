/* Internet Properties: options take effect in newly opened browser windows. */
enum { I_HOME=1700,I_PROXY,I_PORT,I_CACHE,I_APPLY };
static void internet_build(struct cp_app *a)
{
    internet_defaults(&a->internet);
    if(a->host.internet_load)a->host.internet_load(a->host.ctx,&a->internet);
    char number[16];
    add(a,DU_LABEL,1710,24,48,508,18,"Home page (http, https, file or about)");
    add(a,DU_EDIT,I_HOME,24,70,508,24,a->internet.home);
    add(a,DU_LABEL,1711,24,108,508,18,"HTTP proxy (blank connects directly)");
    add(a,DU_EDIT,I_PROXY,24,130,380,24,a->internet.proxy);
    snprintf(number,sizeof(number),"%u",a->internet.port);add(a,DU_EDIT,I_PORT,416,130,116,24,number);
    add(a,DU_LABEL,1712,24,166,508,18,"Memory cache (0 to 16 MiB)");
    snprintf(number,sizeof(number),"%u",a->internet.cache_mb);add(a,DU_EDIT,I_CACHE,24,188,116,24,number);
    add(a,DU_LABEL,1713,24,228,508,18,"Proxy applies to HTTP. HTTPS connects directly with TLS.");
    add(a,DU_LABEL,1714,24,252,508,18,"Changes apply to newly opened browser windows.");
    add(a,DU_BUTTON,I_APPLY,24,290,128,26,"Apply");
}
static int internet_event(struct cp_app *a,const struct du_event *e)
{
    if(a->page!=CP_INTERNET || e->id!=I_APPLY || e->type!=DU_EV_ACTIVATE)return 0;
    struct cp_internet_options o=a->internet;
    if(strlen(du_get(a->ui,I_HOME)->text)>=sizeof(o.home) || strlen(du_get(a->ui,I_PROXY)->text)>=sizeof(o.proxy)) {
        text(a,C_STATUS,"The home page or proxy host name is too long.");return 1;
    }
    snprintf(o.home,sizeof(o.home),"%s",du_get(a->ui,I_HOME)->text);
    snprintf(o.proxy,sizeof(o.proxy),"%s",du_get(a->ui,I_PROXY)->text);
    char *end;const char *value=du_get(a->ui,I_PORT)->text;unsigned long port=strtoul(value,&end,10);
    if(!*value || *end || port>65535) { text(a,C_STATUS,"Enter a proxy port from 1 to 65535.");return 1; }o.port=(unsigned)port;
    value=du_get(a->ui,I_CACHE)->text;unsigned long cache=strtoul(value,&end,10);
    if(!*value || *end || cache>16) { text(a,C_STATUS,"Enter a cache size from 0 to 16 MiB.");return 1; }o.cache_mb=(unsigned)cache;
    if(!internet_valid(&o))text(a,C_STATUS,"Enter a valid home page and proxy host name.");
    else if(!a->host.internet_save || a->host.internet_save(a->host.ctx,&o))text(a,C_STATUS,"Internet settings could not be saved.");
    else { a->internet=o;text(a,C_STATUS,"Saved. Open a new browser window to apply."); }
    return 1;
}
