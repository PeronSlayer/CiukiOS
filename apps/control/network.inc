/* Network applet: UI thread never waits for DNS, disk or TCP.
 * SPDX-License-Identifier: MIT */
enum { N_LIST=1400, N_TABS, N_AUTO, N_STATIC, N_IP, N_MASK, N_GATEWAY,
       N_DNS1, N_DNS2, N_DNS3, N_HOST, N_DOMAIN, N_ENABLED, N_MEDIA,
       N_APPLY, N_REFRESH, N_RENEW, N_RELEASE, N_TEST_NAME, N_TEST, N_FRAME };
static void network_build(struct cp_app *a);
static void network_ip(char out[16], uint32_t ip)
{ const uint8_t *b=(const void *)&ip; snprintf(out,16,"%u.%u.%u.%u",b[0],b[1],b[2],b[3]); }
static int network_parse(const char *s,uint32_t *out)
{
    uint8_t b[4];
    for (unsigned i=0;i<4;i++) {
        unsigned n=0,v=0; while (*s>='0' && *s<='9') { if (++n>3) return 0; v=v*10+(unsigned)(*s++-'0'); }
        if (!n || v>255 || (i<3 ? *s++!='.' : *s!=0)) return 0; b[i]=(uint8_t)v;
    }
    memcpy(out,b,4); return 1;
}
static void network_note(struct cp_app *a,const char *s)
{ snprintf(a->network_message,sizeof(a->network_message),"%s",s); text(a,C_STATUS,s); }
static int network_read_edits(struct cp_app *a)
{
    if (a->network_tab!=1 || !a->network.count) return 1;
    uint32_t *values[]={&a->network_edit.address,&a->network_edit.mask,&a->network_edit.gateway,
        &a->network_edit.dns[0],&a->network_edit.dns[1],&a->network_edit.dns[2]};
    const int ids[]={N_IP,N_MASK,N_GATEWAY,N_DNS1,N_DNS2,N_DNS3};
    for (unsigned i=0;i<6;i++) {
        struct du_widget *w=du_get(a->ui,ids[i]);
        if (a->network_edit.dhcp && i<3) { *values[i]=0; continue; }
        if (!w || !w->text[0]) { *values[i]=0; continue; }
        if (!network_parse(w->text,values[i])) { network_note(a,"Enter IPv4 addresses as four numbers from 0 to 255."); return 0; }
    }
    struct du_widget *h=du_get(a->ui,N_HOST),*d=du_get(a->ui,N_DOMAIN);
    if (h && strlen(h->text)>=sizeof(a->network_edit.hostname)) { network_note(a,"The host name is too long."); return 0; }
    if (d && strlen(d->text)>=sizeof(a->network_edit.domain)) { network_note(a,"The domain is too long."); return 0; }
    snprintf(a->network_edit.hostname,sizeof(a->network_edit.hostname),"%s",h ? h->text : "");
    snprintf(a->network_edit.domain,sizeof(a->network_edit.domain),"%s",d ? d->text : "");
    return 1;
}
static void network_begin(struct cp_app *a,unsigned op)
{
    if (a->network_busy || !a->host.network_begin) return;
    struct ciuki_net_control record={.version=1,.size=sizeof(record),.index=a->network_selected,.config=a->network_edit};
    if (a->network_selected<a->network.count) record.generation=a->network.adapters[a->network_selected].generation;
    record.ipx_frame_type=a->network_frame_type;
    const char *name=a->network_test_name[0] ? a->network_test_name : "example.com";
    int e=a->host.network_begin(a->host.ctx,op,&record,name);
    if (e) { network_note(a,"The network operation could not start."); return; }
    a->network_busy=1; network_note(a,op==CP_NET_TEST ? "Testing DNS and gateway..." : "Reading or applying network settings...");
}
static void network_poll(struct cp_app *a)
{
    if (a->building || !a->network_busy || !a->host.network_poll) return;
    struct cp_network_result result;
    if (a->host.network_poll(a->host.ctx,&result)<=0) return;
    a->network_busy=0; result.count=result.count>2 ? 0 : result.count;
    a->network=result; a->network_loaded=1;
    if (a->network_selected>=result.count) a->network_selected=0;
    if (result.count) { a->network_edit=result.adapters[a->network_selected].config; a->network_frame_type=result.adapters[a->network_selected].ipx_frame_type; }
    network_note(a,result.message[0] ? result.message : result.error ? "Network settings could not be read or applied." : "Network settings are current.");
    if (a->page==CP_NETWORK) build(a);
}
static void network_field(struct cp_app *a,int id,int x,int y,const char *label,uint32_t value,int disabled)
{
    char s[16]; network_ip(s,value);
    add(a,DU_LABEL,id+100,x,y,244,14,label);
    add(a,DU_EDIT,id,x,y+15,244,22,s)->disabled=(uint8_t)disabled;
}
static void network_build(struct cp_app *a)
{
    if (!a->network_loaded && !a->network_busy) network_begin(a,CIUKI_NETCTL_GET);
    for (unsigned i=0;i<a->network.count;i++) {
        struct ciuki_net_control *n=&a->network.adapters[i];
        snprintf(a->network_names[i],sizeof(a->network_names[i]),"%s - %s%s",n->name,n->driver,n->config.enabled ? "" : " (disabled)");
        a->network_items[i]=(struct du_item){.text={a->network_names[i]}};
    }
    struct du_widget *w=add(a,DU_LISTVIEW,N_LIST,24,44,508,52,"");
    w->items=a->network_items; w->count=a->network.count; w->active=a->network_selected;
    w->view=DU_DETAILS; w->column_count=1; w->columns[0]=(struct du_column){"Network adapters",484,0};
    du_set_text(w,a->network_busy ? "Reading adapters..." : "No network adapter detected."); w->disabled=(uint8_t)a->network_busy;
    static const char *const tabs[]={"Adapter Properties","TCP/IP Properties","Status","IPX/SPX Properties"};
    for (unsigned i=0;i<4;i++) a->network_tabs[i]=(struct du_item){.text={tabs[i]}};
    w=add(a,DU_TABS,N_TABS,24,102,508,28,""); w->items=a->network_tabs; w->count=4; w->active=a->network_tab;
    if (!a->network.count) { add(a,DU_LABEL,1510,24,148,508,18,"No configurable network adapter is available."); }
    else {
        struct ciuki_net_control *n=&a->network.adapters[a->network_selected];
        char line[160],ip[16],mask[16];
        if (a->network_tab==0) {
            snprintf(line,sizeof(line),"Driver: %s   PCI: %04x:%04x",n->driver,n->pci_vendor,n->pci_device);
            add(a,DU_LABEL,1510,24,142,508,18,line);
            snprintf(line,sizeof(line),"IRQ: %u   I/O: 0x%04x   MMIO: 0x%08x",n->irq,n->io_base,n->mmio_base);
            add(a,DU_LABEL,1511,24,168,508,18,line);
            snprintf(line,sizeof(line),"MAC: %02x:%02x:%02x:%02x:%02x:%02x",n->mac[0],n->mac[1],n->mac[2],n->mac[3],n->mac[4],n->mac[5]);
            add(a,DU_LABEL,1512,24,194,508,18,line);
            snprintf(line,sizeof(line),"Link: %s   Speed: %u Mb/s   Duplex: %s",n->link==2 ? "connected" : n->link==1 ? "disconnected" : "unknown",n->speed,n->duplex==2 ? "full" : n->duplex==1 ? "half" : "unknown");
            add(a,DU_LABEL,1513,24,220,508,18,line);
            w=add(a,DU_CHECKBOX,N_ENABLED,24,246,340,22,"Enable this network interface"); w->value=(int)a->network_edit.enabled;
            snprintf(line,sizeof(line),a->network_edit.speed ? "Media: %u Mb/s, %s duplex" : "Media: Auto negotiation",a->network_edit.speed,a->network_edit.duplex==2 ? "full" : "half");
            add(a,DU_BUTTON,N_MEDIA,24,278,300,24,line)->disabled=(uint8_t)(!(n->media_caps & (CIUKI_NET_MEDIA_10|CIUKI_NET_MEDIA_100)) || a->network_busy);
            add(a,DU_LABEL,1514,24,310,508,16,"Resource assignments are managed by the system.");
        } else if (a->network_tab==1) {
            w=add(a,DU_RADIO,N_AUTO,24,136,252,22,"Obtain an IP address automatically"); w->value=(int)a->network_edit.dhcp;
            w=add(a,DU_RADIO,N_STATIC,284,136,248,22,"Specify an IP address"); w->value=!a->network_edit.dhcp;
            network_field(a,N_IP,24,160,"IP address:",a->network_edit.address,(int)a->network_edit.dhcp);
            network_field(a,N_MASK,284,160,"Subnet mask:",a->network_edit.mask,(int)a->network_edit.dhcp);
            network_field(a,N_GATEWAY,24,202,"Default gateway:",a->network_edit.gateway,(int)a->network_edit.dhcp);
            network_field(a,N_DNS1,284,202,"DNS server 1 (0 = automatic for DHCP):",a->network_edit.dns[0],0);
            network_field(a,N_DNS2,24,244,"DNS server 2:",a->network_edit.dns[1],0);
            network_field(a,N_DNS3,284,244,"DNS server 3:",a->network_edit.dns[2],0);
            add(a,DU_LABEL,1515,24,286,244,14,"Host name:"); add(a,DU_EDIT,N_HOST,24,301,244,22,a->network_edit.hostname);
            add(a,DU_LABEL,1516,284,286,244,14,"Domain:"); add(a,DU_EDIT,N_DOMAIN,284,301,244,22,a->network_edit.domain);
        } else if (a->network_tab==3) {
            static const char *const frames[]={"Auto","Ethernet 802.2","Ethernet 802.3","Ethernet II","Ethernet SNAP"};
            static const unsigned selection[]={2,1,3,4,0};
            add(a,DU_LABEL,1510,24,144,508,18,"IPX/SPX-compatible Protocol - Advanced");
            add(a,DU_LABEL,1511,24,182,508,18,"Frame type (machine setting):");
            for(unsigned i=0;i<5;i++)a->network_frames[i]=(struct du_item){.text={frames[i]}};
            w=add(a,DU_COMBO,N_FRAME,24,210,300,26,"");
            du_combo_set(w,a->network_frames,5,selection[a->network_frame_type<=4 ? a->network_frame_type : 4]);
            add(a,DU_LABEL,1512,24,258,508,18,"Auto detects routers; without routers it uses Ethernet 802.2.");
            add(a,DU_LABEL,1513,24,286,508,18,"An administrator can save this setting for all adapters.");
        } else {
            network_ip(ip,n->address); network_ip(mask,n->mask);
            snprintf(line,sizeof(line),"IP: %s   Mask: %s",ip,mask); add(a,DU_LABEL,1510,24,140,508,18,line);
            network_ip(ip,n->gateway); snprintf(line,sizeof(line),"Gateway: %s",ip); add(a,DU_LABEL,1511,24,162,508,18,line);
            for (unsigned i=0;i<3;i++) { network_ip(ip,n->dns[i]); snprintf(line,sizeof(line),"DNS %u: %s",i+1,ip); add(a,DU_LABEL,1512+(int)i,24,184+(int)i*20,508,18,line); }
            snprintf(line,sizeof(line),"Lease: %u s   Remaining: %u s",n->lease_seconds,n->lease_remaining); add(a,DU_LABEL,1515,24,246,508,18,line);
            snprintf(line,sizeof(line),"Packets sent: %llu   Received: %llu",(unsigned long long)n->tx_packets,(unsigned long long)n->rx_packets); add(a,DU_LABEL,1516,24,268,508,18,line);
            snprintf(line,sizeof(line),"Errors: TX %llu   RX %llu",(unsigned long long)n->tx_errors,(unsigned long long)n->rx_errors); add(a,DU_LABEL,1517,24,290,508,18,line);
            add(a,DU_LABEL,1518,24,314,48,18,"Name:"); add(a,DU_EDIT,N_TEST_NAME,76,310,352,22,a->network_test_name[0] ? a->network_test_name : "example.com");
            add(a,DU_BUTTON,N_TEST,440,310,88,24,"Test")->disabled=(uint8_t)a->network_busy;
        }
        add(a,DU_BUTTON,N_APPLY,440,336,88,24,"Apply")->disabled=(uint8_t)(a->network_busy || a->network_tab==2);
        add(a,DU_BUTTON,N_RENEW,124,336,88,24,"Renew")->disabled=(uint8_t)(a->network_busy || !n->config.dhcp || !n->config.enabled);
        add(a,DU_BUTTON,N_RELEASE,224,336,88,24,"Release")->disabled=(uint8_t)(a->network_busy || !n->config.dhcp || !n->config.enabled);
    }
    add(a,DU_BUTTON,N_REFRESH,24,336,88,24,"Refresh")->disabled=(uint8_t)a->network_busy;
    if (a->network_busy) for (unsigned i=0;i<a->ui->count;i++) {
        struct du_widget *busy=&a->ui->widgets[i];
        if (busy->id>=N_LIST && busy->id<=N_FRAME) busy->disabled=1;
    }
    text(a,C_STATUS,a->network_message);
}
static int network_event(struct cp_app *a,const struct du_event *e)
{
    if (a->page!=CP_NETWORK) return 0;
    if (e->id==N_LIST && e->type==DU_EV_SELECT && !a->network_busy && e->index>=0 && (unsigned)e->index<a->network.count) {
        a->network_selected=(unsigned)e->index; a->network_edit=a->network.adapters[a->network_selected].config; a->network_frame_type=a->network.adapters[a->network_selected].ipx_frame_type; build(a); return 1;
    }
    if (e->id==N_TABS && e->type==DU_EV_SELECT && e->index>=0 && e->index<4) {
        if (!network_read_edits(a)) return 1; a->network_tab=(unsigned)e->index; build(a); return 1;
    }
    if (a->network_busy) return 0;
    if (e->id==N_FRAME && e->type==DU_EV_SELECT && e->index>=0 && e->index<5) {
        static const unsigned values[]={4,1,0,2,3};
        a->network_frame_type=values[e->index];return 1;
    }
    if (e->type==DU_EV_CHANGE && e->id==N_ENABLED) { a->network_edit.enabled=(uint32_t)e->index; return 1; }
    if (e->type==DU_EV_CHANGE && (e->id==N_AUTO || e->id==N_STATIC)) {
        if (!network_read_edits(a)) return 1; a->network_edit.dhcp=e->id==N_AUTO; build(a); return 1;
    }
    if (e->type!=DU_EV_ACTIVATE) return 0;
    if (e->id==N_AUTO || e->id==N_STATIC) {
        if (!network_read_edits(a)) return 1; a->network_edit.dhcp=e->id==N_AUTO; build(a); return 1;
    }
    if (e->id==N_ENABLED) { struct du_widget *w=du_get(a->ui,N_ENABLED); a->network_edit.enabled=w ? (uint32_t)w->value : 0; return 1; }
    if (e->id==N_MEDIA) {
        unsigned mode=a->network_edit.speed==10 ? a->network_edit.duplex : a->network_edit.speed==100 ? a->network_edit.duplex+2 : 0;
        mode=(mode+1)%5; a->network_edit.speed=mode ? mode<3 ? 10 : 100 : 0; a->network_edit.duplex=mode ? (mode-1)%2+1 : 0; build(a); return 1;
    }
    unsigned op;
    if (e->id==N_REFRESH) op=CIUKI_NETCTL_GET;
    else if (e->id==N_APPLY) { if (!network_read_edits(a)) return 1; op=a->network_tab==3 ? CP_NET_IPX : CIUKI_NETCTL_SET; }
    else if (e->id==N_RENEW) op=CIUKI_NETCTL_RENEW;
    else if (e->id==N_RELEASE) op=CIUKI_NETCTL_RELEASE;
    else if (e->id==N_TEST) { struct du_widget *w=du_get(a->ui,N_TEST_NAME); snprintf(a->network_test_name,sizeof(a->network_test_name),"%s",w ? w->text : ""); op=CP_NET_TEST; }
    else return 0;
    network_begin(a,op); build(a); return 1;
}
