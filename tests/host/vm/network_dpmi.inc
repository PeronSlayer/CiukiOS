/* Production trap/IVT/DPMI regression; included by lifecycle_test.c.
 * CPU execution of nontrapping MOV/RETF uses the existing DPMI harness.
 * SPDX-License-Identifier: GPL-2.0-only */
static void ipx_simulate(struct dosvm *v, uint16_t data, unsigned vector,
                         unsigned ax, unsigned bx, unsigned dx, unsigned ecb)
{
    rmcs_set(v,ax,bx,0x0202,ecb>>4,0,0,0,0);
    uint8_t s[0x32]; CHECK(!dosvm_read(v,rmcs(),s,sizeof(s)));
    vm_w32(s+0x14,dx); vm_w32(s+4,ecb&15);
    CHECK(!dosvm_write(v,rmcs(),s,sizeof(s)));
    cur.tf.es=data; cur.tf.edi=0x200; cur.tf.ebx=vector; cur.tf.ecx=0;
    int31(v,0x0300);
    CHECK(!in_pm() && cur.tf.cs==0xf000 && cur.tf.eip==vector*16 && v->dpmi.depth==1);
    uint16_t before[3],after[3];
    uint32_t stack=(cur.tf.user_ss<<4)+(uint16_t)cur.tf.user_esp;
    CHECK(!dosvm_read(v,stack,before,sizeof(before)));
    step(v,13,0); /* synthetic INT FE: the IPX service, no second INT frame */
    CHECK(!dosvm_read(v,stack,after,sizeof(after)) && !memcmp(before,after,sizeof(before)));
    CHECK(!in_pm() && cur.tf.eip==vector*16+2);
    step(v,13,0); /* original IRET */
    CHECK(cur.tf.cs==0xf000 && cur.tf.eip==DPMI_RM_RETURN);
    step(v,13,0);
    CHECK(in_pm() && !cf() && !v->dpmi.depth);
}

static void test_network_dpmi(void)
{
    struct dosvm *v=dpmi_vm(true);
    uint8_t id[16]={42}; static struct bridge_pool pool;
    struct netif device={.generation=94,.ready=true,.running=true,.link=NET_LINK_UP};
    struct net_bridge_port port={0};
    CHECK(!net_bridge_port_open(&port,&pool,&device,0,id,0,0,host_network_mode));
    CHECK(!dosvm_network_attach(v,&port));
    v86_start(); enter(v,true); CHECK(in_pm());
    uint16_t data=(uint16_t)cur.tf.ds;
    ipx_simulate(v,data,0x2f,0x7a00,0,0,0);
    CHECK((rmcs32(v,0x1c)&255)==255 && rmcs16(v,0x22)==0xf000 && rmcs16(v,0)==DOSVM_IPX_ENTRY);
    ipx_simulate(v,data,0x7a,0,0,0x9c86,0);
    CHECK(!(rmcs32(v,0x1c)&255) && rmcs32(v,0x14)==0x9c86);
    CHECK(network(v)->ipx.socket[0].owner==PSP_SEG);
    uint8_t e[48]={0}; vm_w16(e+4,0x30); vm_w16(e+6,0x5000);
    vm_w16(e+10,0x9c86); vm_w16(e+34,2); vm_w16(e+38,0x600); vm_w16(e+40,30);
    vm_w16(e+44,0x610); vm_w16(e+46,100);
    CHECK(!dosvm_write(v,0x2000,e,sizeof(e)));
    const uint8_t esr[]={0xbb,4,0,0xcd,0x7a,0xcb}; /* MOV BX,4; INT7A; RETF */
    CHECK(!dosvm_write(v,0x50030,esr,sizeof(esr)));
    ipx_simulate(v,data,0x7a,0,4,0,0x2000);
    CHECK(!dosvm_read(v,0x2000,e,sizeof(e)) && e[8]==0xfe && !(rmcs32(v,0x1c)&255));
    /* Default PM reflection must use the same synthetic IVT service too. */
    cur.tf.eax=0x7a00; pm_int(v,0x2f);
    CHECK(!in_pm() && cur.tf.cs==0xf000 && cur.tf.eip==0x2f0);
    step(v,13,0); CHECK((cur.tf.eax&255)==255);
    step(v,13,0); step(v,13,0); CHECK(in_pm());
    /* STI and one executed instruction leave a real eligible PM boundary. */
    op1(v,0xfb); cur.tf.eip++; step(v,0x20,0);
    CHECK(v->monitor.vif && !v->dpmi.shadow);
    uint8_t frame[64]={0}; memcpy(frame,network(v)->ne.assigned,6); frame[6]=2; frame[7]=99;
    frame[12]=0x81; frame[13]=0x37; frame[14]=frame[15]=255; frame[17]=50;
    memcpy(frame+24,frame,6); memcpy(frame+36,frame+6,6); frame[30]=0x86; frame[31]=0x9c;
    for (unsigned repeat=0; repeat<2; repeat++) {
        if (repeat) {
            /* An actual DPMI callback leaves two existing records while its
             * protected-mode handler runs. The ESR may nest above them. */
            cur.tf.ds=cur.tf.cs; cur.tf.esi=0x900; cur.tf.es=data; cur.tf.edi=0x300;
            int31(v,0x0303); CHECK(!cf()); uint16_t callback=(uint16_t)cur.tf.edx;
            rmcs_set(v,0,0,0x0202,0,0xf000,callback,0,0);
            cur.tf.es=data; cur.tf.edi=0x200; cur.tf.ecx=0; int31(v,0x0301);
            CHECK(!in_pm()); step(v,13,0); CHECK(in_pm() && v->dpmi.depth==2);
            /* Callback eventually RETFs by updating its 0303 register image. */
            uint8_t s[0x32]; uint16_t ret[2];
            CHECK(!dosvm_read(v,(DATA_SEG<<4)+0x300,s,sizeof(s)));
            uint32_t at=((uint32_t)vm_u16(s+0x30)<<4)+vm_u16(s+0x2e);
            CHECK(!dosvm_read(v,at,ret,sizeof(ret)));
            vm_w16(s+0x2a,ret[0]); vm_w16(s+0x2c,ret[1]); vm_w16(s+0x2e,vm_u16(s+0x2e)+4);
            CHECK(!dosvm_write(v,(DATA_SEG<<4)+0x300,s,sizeof(s)));
            op1(v,0xfb); cur.tf.eip++; step(v,0x20,0);
        }
        host_network_tap(&device,frame,sizeof(frame),host_network_cookie);
        dosvm_network_service(v,1000+repeat);
        CHECK(!dosvm_read(v,0x2000,e,sizeof(e)) && !e[8] && !e[9]);
        struct v86_frame continuation=cur;
        unsigned depth=v->dpmi.depth,top=v->dpmi.rstack_top;
        step(v,0x20,0); /* actual return boundary dispatches while PM */
        CHECK(!in_pm() && network(v)->active && v->dpmi.depth==depth+1);
        CHECK(cur.tf.cs==0x5000 && cur.tf.eip==0x30 && cur.es==0x200 && (cur.tf.eax&255)==255);
        cur.tf.ebx=4; cur.tf.eip+=3; /* CPU executes MOV BX,4 */
        step(v,13,0); /* ESR reposts through production INT7A interceptor */
        CHECK(!dosvm_read(v,0x2000,e,sizeof(e)) && e[8]==0xfe);
        cur.tf.eip=rm_pop16(v); cur.tf.cs=rm_pop16(v); /* CPU RETF */
        CHECK(cur.tf.cs==0xf000 && cur.tf.eip==DOSVM_IPX_RETURN);
        step(v,13,0);
        CHECK(in_pm() && !network(v)->active && v->dpmi.depth==depth && v->dpmi.rstack_top==top);
        CHECK(cur.tf.cs==continuation.tf.cs && cur.tf.eip==continuation.tf.eip &&
              cur.tf.user_ss==continuation.tf.user_ss && cur.tf.user_esp==continuation.tf.user_esp &&
              cur.tf.eax==continuation.tf.eax && cur.tf.esi==continuation.tf.esi &&
              cur.tf.ds==continuation.tf.ds && cur.tf.es==continuation.tf.es);
        cur.tf.eax=0xabcdef01; op1(v,0xfa); op1(v,0xfb); /* continuing PM execution */
        cur.tf.eip++; step(v,0x20,0); CHECK(in_pm() && cur.tf.eax==0xabcdef01);
        if (repeat) {
            pm_iret(v); at_stub(v,DPMI_PM_CBRET);
            CHECK(!in_pm() && cur.tf.eip==DPMI_RM_RETURN); step(v,13,0);
            CHECK(in_pm() && !v->dpmi.depth);
        }
    }
    dosvm_network_process_exit(v,PSP_SEG);
    CHECK(!dosvm_read(v,0x2000,e,sizeof(e)) && e[9]==0xfc && !network(v)->ipx.socket[0].open);
    destroy(v,true); empty(); CHECK(!pool.used && !host_network_promiscuous);
    CHECK(!net_bridge_port_close(&port));
    puts("network DPMI: 0300 discovery/open/listen, PM reflection, receive/ESR repost/PM continuation PASS");
}
