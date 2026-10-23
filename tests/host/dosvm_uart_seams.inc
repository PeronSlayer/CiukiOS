/* Physical serial transport seam; null UARTs execute the production model.
 * COM arbitration itself is covered by com_test.c with real com.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/com.h>
static unsigned host_com_owner[CIUKI_COM_MAX], host_com_gen[CIUKI_COM_MAX], host_com_next;
static struct ciuki_com_line host_com_line[CIUKI_COM_MAX];
static unsigned host_com_tx[CIUKI_COM_MAX];
static bool host_com_busy;
int com_lease_open(unsigned i,unsigned owner,uint32_t id,uint32_t *g,const char **why)
{
    (void)id;
    if (i>=CIUKI_COM_MAX) return -ENODEV;
    if (host_com_owner[i]) { if (why) *why="Physical COM lease conflict."; return -EBUSY; }
    host_com_owner[i]=owner; host_com_gen[i]=*g=++host_com_next;
    host_com_line[i]=(struct ciuki_com_line){1,sizeof(struct ciuki_com_line),38400,8,1,0,0,0,0,0};
    return 0;
}
void com_close(unsigned i,uint32_t g)
{ if(i<CIUKI_COM_MAX && host_com_gen[i]==g) host_com_owner[i]=host_com_gen[i]=0; }
int com_control(unsigned i,uint32_t g,uint32_t op,void *p,unsigned bytes,bool write)
{
    (void)bytes; (void)write;
    if(i>=CIUKI_COM_MAX || !host_com_gen[i] || host_com_gen[i]!=g) return -ENODEV;
    if(op==COM_GET_LINE) *(struct ciuki_com_line *)p=host_com_line[i];
    else if(op==COM_SET_LINE) { if(host_com_busy) return -EBUSY; host_com_line[i]=*(struct ciuki_com_line *)p; }
    else if(op==COM_GET_STATUS) *(struct ciuki_com_status *)p=(struct ciuki_com_status){.version=1,.size=sizeof(struct ciuki_com_status)};
    else if(op!=COM_SET_MODEM) return -ENOTTY;
    return 0;
}
int com_transfer(unsigned i,uint32_t g,void *b,unsigned n,bool write)
{
    (void)b;
    if(i>=CIUKI_COM_MAX || !host_com_gen[i] || host_com_gen[i]!=g) return -ENODEV;
    if(write) { host_com_tx[i]+=n; return (int)n; }
    return -EAGAIN;
}
bool com_transmitter_idle(unsigned i,uint32_t g)
{ return i<CIUKI_COM_MAX && host_com_gen[i]==g && !host_com_busy; }
int com_vm_line_errors(unsigned i,uint32_t g)
{ return i<CIUKI_COM_MAX && host_com_gen[i]==g ? 0 : -ENODEV; }
