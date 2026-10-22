/* Production network applet interaction and raster evidence. SPDX-License-Identifier: MIT */
#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
static struct cp_network_result fixture;
static int pending, hold, calls;
static unsigned operation;
static struct ciuki_net_control submitted;
static int begin(void *ctx,unsigned op,const struct ciuki_net_control *r,const char *name)
{
    (void)ctx;assert(!pending && name && *name);pending=1;calls++;operation=op;submitted=*r;
    if(op==CIUKI_NETCTL_SET)fixture.adapters[r->index].config=r->config;
    return 0;
}
static int poll_result(void *ctx,struct cp_network_result *r)
{
    (void)ctx;if(!pending || hold)return 0;pending=0;*r=fixture;return 1;
}
static void click(int id)
{
    struct du_widget *w=du_get(&ui,id);assert(w && !w->disabled);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=w->box.x+w->box.w/2,.value2=w->box.y+w->box.h/2};du_input(&ui,&e);
    e.type=CIUKI_INPUT_BUTTON;e.code=CIUKI_BUTTON_LEFT;e.value=1;du_input(&ui,&e);e.value=0;du_input(&ui,&e);
}
static void tab(unsigned index)
{ struct du_widget *w=du_get(&ui,1401);assert(w);w->active=index;du_post(&ui,DU_EV_SELECT,1401,(int)index); }
static void frame(const char *dir,const char *name)
{
    du_render(&ui);char path[512];snprintf(path,sizeof(path),"%s/%s.xrgb",dir,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(pixels,4,CP_WIDTH*CP_HEIGHT,f)==CP_WIDTH*CP_HEIGHT);assert(!fclose(f));
    for(unsigned i=0;i<ui.count;i++)for(unsigned j=i+1;j<ui.count;j++)assert(ui.widgets[i].id!=ui.widgets[j].id);
}
int main(int argc,char **argv)
{
    assert(argc==2);du_init(&ui,pixels,CP_WIDTH,CP_HEIGHT);ui.focused_window=1;
    app.host=(struct cp_host){.network_begin=begin,.network_poll=poll_result};
    hold=1;assert(!cp_attach(&app,&ui,CP_NETWORK));assert(pending && app.network_busy);frame(argv[1],"network-busy");
    hold=0;cp_tick(&app);assert(!app.network_busy && !app.network.count);frame(argv[1],"network-absent");
    struct ciuki_net_control *n=&fixture.adapters[0];fixture.count=2;
    *n=(struct ciuki_net_control){.version=1,.size=sizeof(*n),.count=2,.link=2,.state=1,.media_caps=7,
        .pci_vendor=0x8086,.pci_device=0x1229,.irq=11,.io_base=0x1800,.speed=100,.duplex=2,.rx_packets=427,.tx_packets=219,
        .rx_errors=1,.lease_seconds=3600,.lease_remaining=3512};
    strcpy(n->name,"en0");strcpy(n->driver,"e100");memcpy(n->mac,"\x00\xd0\x59\xb6\x43\x8f",6);
    n->config.enabled=n->config.dhcp=1;strcpy(n->config.hostname,"ciukios");strcpy(n->config.domain,"example.test");
    const uint8_t ip[]={192,0,2,100},mask[]={255,255,255,0},gw[]={192,0,2,1};
    memcpy(&n->address,ip,4);memcpy(&n->mask,mask,4);memcpy(&n->gateway,gw,4);n->dns[0]=n->gateway;
    fixture.adapters[1]=*n;strcpy(fixture.adapters[1].name,"en1");strcpy(fixture.adapters[1].driver,"ne2000");fixture.adapters[1].index=1;fixture.adapters[1].media_caps=1;
    click(1415);cp_tick(&app);assert(app.network.count==2);frame(argv[1],"network-adapter");
    click(1412);assert(!app.network_edit.enabled);click(1412);assert(app.network_edit.enabled);
    click(1413);assert(app.network_edit.speed==10 && app.network_edit.duplex==1);
    click(1414);assert(operation==CIUKI_NETCTL_SET && submitted.config.speed==10);cp_tick(&app);
    tab(1);frame(argv[1],"network-dhcp");
    click(1403);assert(!app.network_edit.dhcp && !du_get(&ui,1404)->disabled);
    du_set_text(du_get(&ui,1404),"192.0.2.110");du_set_text(du_get(&ui,1405),"255.255.255.0");
    du_set_text(du_get(&ui,1406),"192.0.2.1");du_set_text(du_get(&ui,1407),"192.0.2.1");
    du_set_text(du_get(&ui,1408),"192.0.2.2");du_set_text(du_get(&ui,1409),"192.0.2.3");
    frame(argv[1],"network-static");int before=calls;
    du_set_text(du_get(&ui,1404),"999.1.2.3");click(1414);assert(calls==before && strstr(app.network_message,"IPv4"));frame(argv[1],"network-invalid");
    du_set_text(du_get(&ui,1404),"192.0.2.110");click(1414);assert(operation==1 && !submitted.config.dhcp && submitted.config.dns[2]);cp_tick(&app);
    tab(2);frame(argv[1],"network-status");click(1419);assert(operation==CP_NET_TEST && app.network_busy);
    strcpy(fixture.message,"DNS 192.0.2.8; gateway TCP/80 refused.");cp_tick(&app);assert(strstr(app.network_message,"refused"));frame(argv[1],"network-test");
    tab(1);click(1402);click(1414);assert(submitted.config.dhcp && !submitted.config.address && !submitted.config.gateway);cp_tick(&app);
    tab(0);du_get(&ui,1400)->active=1;du_post(&ui,DU_EV_SELECT,1400,1);assert(app.network_selected==1 && du_get(&ui,1413)->disabled);
    click(1416);assert(operation==CIUKI_NETCTL_RENEW);cp_tick(&app);click(1417);assert(operation==CIUKI_NETCTL_RELEASE);cp_tick(&app);
    cp_detach(&app);puts("PASS f2-100 network dialog: asynchronous enumeration, adapter selection/options, DHCP/static edits/validation, three DNS, renew/release, status/test and eight frames");return 0;
}
