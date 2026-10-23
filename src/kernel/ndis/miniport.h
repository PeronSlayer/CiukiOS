/* NT5 i386 upper edge. All addresses below are process VAs, never C pointers.
 * Sources: docs/design/ndis-host-netif.md. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_MINIPORT_H
#define CIUKI_NDIS_MINIPORT_H
#include "nt_runtime.h"
#include "channel.h"
#define NDIS_SUCCESS 0u
#define NDIS_PENDING 0x103u
#define NDIS_FAILURE 0xc0000001u
#define NDIS_RESOURCES 0xc000009au
#define NDIS_NOT_SUPPORTED 0xc00000bbu
#define NDIS_INVALID_LENGTH 0xc0010014u
#define NDIS_BUS_MASTER 8u
#define NDIS_DESERIALIZE 0x20u
#define NDIS_PACKET_BYTES 192u
#define NDIS_CONTROL_BYTES 1024u
struct ndis_chars32 {
    uint8_t major,minor;
    uint16_t filler;
    uint32_t reserved;
    uint32_t check_hang,disable_irq,enable_irq,halt,handle_irq,initialize,isr;
    uint32_t query,reconfigure,reset,send,set,transfer;
    uint32_t return_packet,send_packets,allocate_complete;
    uint32_t co[6];
    uint32_t cancel_send,pnp,shutdown,reserved51[4];
};
struct ndis_packet32 {
    uint32_t physical_count,total,head,tail,pool,count,flags;
    uint8_t valid,packet_flags;
    uint16_t oob;
    uint32_t miniport_reserved[2],wrapper_reserved[2],reserved[2],protocol_reserved[4];
    uint32_t time_to_send[2],time_received[2],header_size,media_size,media,status;
    uint32_t extension[12];
};
/* Public DDK-visible prefix through SetCompleteHandler. Byte arrays preserve
 * NT5 KTIMER/KDPC layout without importing host compiler pointer alignment. */
struct ndis_block32 {
    uint32_t signature,next,driver,adapter_context;
    struct ndis_nt_string name;
    uint32_t bind,open,reference[2],device;
    uint8_t padding,lock_acquired,pmode,processor;
    uint32_t lock,media_request,interrupt,flags,pnp_flags,list[2];
    uint32_t pending_packet,return_queue,request_buffer,mcast_buffer,primary,wrapper,bus_context,pnp_caps,resources;
    uint8_t timer[72];
    struct ndis_nt_string base_name,symbolic_name;
    uint32_t hang_seconds;
    uint16_t hang_ticks,hang_current;
    uint32_t reset_status,reset_open,filter[4];
    uint32_t indicate_packet,send_complete,send_resources,reset_complete,medium,bus_number,bus_type,adapter_type;
    uint32_t device_object,physical_device,next_device,map_registers,af_list,thread,set_buffer;
    uint16_t set_length,max_send;
    uint32_t fake_status,lock_handler,instance_name,timer_queue,mac_options,pending_request;
    uint32_t max_long,max_short,lookahead,max_lookahead,handle_irq,disable_irq,enable_irq,send_packets,deferred_send;
    uint32_t eth_receive,tr_receive,fddi_receive,eth_complete,tr_complete,fddi_complete;
    uint32_t indicate_status,status_complete,transfer_complete,query_complete,set_complete;
};
enum ndis_mp_state { NDIS_MP_NEW,NDIS_MP_ENTRY,NDIS_MP_REGISTERED,NDIS_MP_INITIALIZING,
                     NDIS_MP_CONFIGURING,NDIS_MP_RUNNING,NDIS_MP_RESETTING,
                     NDIS_MP_HALTING,NDIS_MP_SHUTTING,NDIS_MP_STOPPED,NDIS_MP_FAILED };
struct ndis_mp_ops {
    void *context;
    int (*enqueue)(void *,enum ndis_host_kind,uint32_t,unsigned,const uint32_t *,uint32_t *);
    int (*result)(void *,uint32_t,uint32_t *);
    int (*interrupt)(void *,bool install,uint32_t isr,uint32_t dpc);
    void (*link)(void *,bool carrier);
    void (*record)(void *,const char *);
};
struct ndis_mp_tx {
    uint32_t packet,buffer,mdl,sg,token,callback;
    uint64_t deadline;
    unsigned slot;
    bool live,submitted,completed,resources;
};
struct ndis_miniport {
    struct ndis_nt_runtime *nt;
    struct ndis_channel *channel;
    struct ndis_mp_ops ops;
    struct ndis_chars32 chars;
    enum ndis_mp_state state;
    uint32_t generation,wrapper,adapter,config,context,scratch,control_buffer;
    uint32_t lifecycle,hang_token,hang_seconds,attributes,interrupt_object;
    uint32_t shutdown,shutdown_context,unload;
    unsigned raw_irq;
    uint64_t hang_due,request_due;
    uint32_t tx_consumed,rx_published,rx_credit[NDIS_RING_SLOTS];
    struct ndis_mp_tx tx[NDIS_RING_SLOTS];
    struct { uint32_t packet,token;bool live; } returns[NDIS_RING_SLOTS];
    struct {
        uint32_t oid,bytes,status,written,needed,callback;
        bool live,set,pending,completed;
    } control;
    unsigned startup_oid;
    uint32_t supported[NDIS_CONTROL_BYTES/4];unsigned supported_count;
    uint32_t mtu,total,lookahead,filter,multicast_limit,link_speed,mac_options,max_sends;
    uint8_t address[6],permanent[6];
    bool attributes_set,serialized,tx_blocked,carrier,status_pending,status_carrier;
    bool reset_pending,reset_completed,addressing_reset,sg;
    uint32_t reset_result;
    uint64_t counter_due,counters[5];
    uint8_t counter_width[5];
    unsigned counter_next;
    bool counter_request;
    struct { struct trap_frame frame;uint32_t token;uint8_t irql;bool active,parked,sync; } call[NDIS_HOST_DEPTH];
    struct { uint32_t packet,buffer,mdl,callback,context,bytes;uint8_t header[14];bool live,pending,completed;uint32_t status,transferred; } transfer;
};
int ndis_mp_init(struct ndis_miniport *,struct ndis_nt_runtime *,struct ndis_channel *,const struct ndis_mp_ops *);
int ndis_mp_start(struct ndis_miniport *,uint32_t driver_object,uint32_t registry_path);
uint32_t ndis_mp_register(struct ndis_miniport *,uint32_t address,uint32_t bytes);
int ndis_mp_attributes(struct ndis_miniport *,uint32_t context,uint32_t hang,uint32_t flags,uint32_t bus);
int ndis_mp_irq(struct ndis_miniport *,uint32_t object,uint32_t vector,uint32_t level,bool isr,bool shared,uint32_t mode);
void ndis_mp_poll(struct ndis_miniport *);
int ndis_mp_oid(struct ndis_miniport *,bool set,uint32_t oid,const void *,uint32_t bytes);
int ndis_mp_oid_complete(struct ndis_miniport *,bool set,uint32_t status);
int ndis_mp_send_complete(struct ndis_miniport *,uint32_t packet,uint32_t status);
int ndis_mp_send_resources(struct ndis_miniport *);
int ndis_mp_receive(struct ndis_miniport *,const uint32_t *,unsigned count);
int ndis_mp_lookahead(struct ndis_miniport *,uint32_t context,uint32_t header,unsigned header_bytes,uint32_t lookahead,unsigned lookahead_bytes,unsigned packet_bytes);
int ndis_mp_transfer_complete(struct ndis_miniport *,uint32_t packet,uint32_t status,unsigned transferred);
int ndis_mp_reset(struct ndis_miniport *);
int ndis_mp_reset_complete(struct ndis_miniport *,uint32_t status,bool addressing_reset);
int ndis_mp_halt(struct ndis_miniport *);
int ndis_mp_shutdown(struct ndis_miniport *);
void ndis_mp_fault(struct ndis_miniport *,const char *);
void ndis_mp_status(struct ndis_miniport *,uint32_t status,bool complete);
int ndis_mp_inline(struct ndis_miniport *,uint32_t function,unsigned,const uint32_t *,bool synchronize);
int ndis_mp_park(struct ndis_miniport *,struct trap_frame *);
int ndis_mp_resume(struct ndis_miniport *,struct trap_frame *);
uint32_t ndis_mp_packet_alloc(struct ndis_miniport *,uint32_t pool,unsigned protocol_bytes);
int ndis_mp_packet_free(struct ndis_miniport *,uint32_t packet);
#endif
