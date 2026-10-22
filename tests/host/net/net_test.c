/* Register models, strict resource lifetimes and production driver code.
 * No GPL driver imported. SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_CPU_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/net.h>
#include <ciuki/timing.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;
static uint32_t host_flags = 0x200;
static unsigned sleeps, yields, page_count, releases, quarantines, config_writes, irq_adds, irq_removes;
static int claim_fail, allocation_fail, irq_add_fail, release_fail;
static unsigned map_calls, unmap_calls;
static bool map_fail;
static unsigned mmio_token;
static uint64_t cycles;
static void (*sleep_hook)(void);
static bool sleep_hook_active;
static uint8_t memory[128 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t route_bios[0x10000];
static bool allocated[128];
#define HOST_PHYS 0x1000000u
#define V2P(p) ((uint32_t)((uint8_t *)(p) - memory) + HOST_PHYS)
#define P2V(p) ((void *)((p) == 0xf0000 ? route_bios : memory + ((p) - HOST_PHYS)))
static uint32_t irq_save(void) { uint32_t f = host_flags; host_flags &= ~0x200u; return f; }
static void irq_restore(uint32_t f) { host_flags = f; }
static uint32_t route_read(uint16_t p, unsigned width);
static void route_write(uint16_t p, uint32_t value);
static uint8_t inb(uint16_t p) { return route_read(p, 1); }
static uint32_t inl(uint16_t p) { return route_read(p, 4); }
static void outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
static void outl(uint16_t p, uint32_t v) { route_write(p, v); }
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
uint64_t g_tsc_per_ms = 1000;
bool g_cpu_tsc = true;
static unsigned stall_read;
static uint32_t read_eflags(void) { return host_flags; }
static uint64_t read_tsc_serialized(void)
{
    if (stall_read && !--stall_read) cycles += 2100;
    return cycles;
}
uint64_t ktime_cycles(void) { return cycles; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool deadline_passed(uint64_t deadline) { return (int64_t)(g_ticks - deadline) >= 0; }
int udelay(uint32_t us) { cycles += us; return 0; }
gen_t gen_alloc(void) { static gen_t n; return ++n; }
void klog(const char *fmt, ...) { (void)fmt; }
void kmutex_init(struct kmutex *m) { memset(m, 0, sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { CHECK(host_flags & 0x200); CHECK(!m->owner); m->owner = (void *)(uintptr_t)1; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner); m->owner = 0; }
void task_sleep_ms(uint32_t ms)
{
    CHECK(host_flags & 0x200); sleeps++; g_ticks += ms; cycles += ms * 1000;
    if (sleep_hook && !sleep_hook_active) { sleep_hook_active = true; sleep_hook(); sleep_hook_active = false; }
}
static bool stack_service_needs_yield;
void task_yield(void) { CHECK(host_flags & 0x200); yields++;stack_service_needs_yield=false; }
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *arg, enum task_prio p)
{ (void)n; (void)fn; (void)arg; (void)p; return 0; }
void task_start(struct task *t) { (void)t; }
__attribute__((noreturn)) void task_exit(int status) { exit(status); }
uint32_t pmm_alloc_contig(uint32_t pages)
{
    if (allocation_fail) return 0;
    for (unsigned i = 0; i + pages <= 128; i++) {
        bool free = true; for (unsigned j = 0; j < pages; j++) if (allocated[i + j]) free = false;
        if (!free) continue;
        for (unsigned j = 0; j < pages; j++) allocated[i + j] = true;
        page_count += pages; return HOST_PHYS + i * PAGE_SIZE;
    }
    return 0;
}
void pmm_free(uint32_t phys)
{
    unsigned i = (phys - HOST_PHYS) / PAGE_SIZE; CHECK(i < 128 && allocated[i]);
    allocated[i] = false; page_count--; memset(P2V(phys), 0xcc, PAGE_SIZE);
}
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached)
{
    CHECK(phys == 0xf0000000 && bytes == 0x20000 && uncached); map_calls++;
    return map_fail ? NULL : &mmio_token;
}
void vmm_revoke_mmio(void *mapped, uint32_t bytes)
{ CHECK(mapped == &mmio_token && bytes == 0x20000); unmap_calls++; }
static struct resource resources[32];
static unsigned resource_count;
unsigned registry_count(void) { return resource_count; }
const struct resource *registry_get(unsigned h) { return h < resource_count ? &resources[h] : 0; }
bool registry_valid(int h, gen_t gen)
{ return h >= 0 && (unsigned)h < resource_count && resources[h].generation == gen && resources[h].state != RS_QUARANTINED && resources[h].state != RS_RELEASED; }
int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share)
{
    CHECK(start < end && owner);
    if (claim_fail && resource_count + 1 == (unsigned)claim_fail) { claim_fail=0; return -16; }
    for (unsigned i = 0; i < resource_count; i++) {
        struct resource *r = &resources[i];
        if (r->state != RS_RELEASED && r->type == type && start < r->end && end > r->start && !(share && r->shareable)) return -16;
    }
    CHECK(resource_count < 32); unsigned h = resource_count++;
    resources[h] = (struct resource){type,start,end,owner,gen_alloc(),RS_CLAIMED,share}; return h;
}
int registry_activate(int h, gen_t gen)
{ CHECK(registry_valid(h, gen) && resources[h].state == RS_CLAIMED); resources[h].state = RS_ACTIVE; return 0; }
int registry_quiesce(int h, gen_t gen, registry_idle_proof_fn proof)
{ CHECK(registry_valid(h, gen) && resources[h].state == RS_ACTIVE); resources[h].state = RS_QUIESCING; CHECK(proof(h, gen)); return 0; }
int registry_release(int h, gen_t gen)
{
    CHECK(registry_valid(h, gen) && resources[h].state == RS_QUIESCING);
    if (release_fail) return -5;
    resources[h].state = RS_RELEASED; releases++; return 0;
}
int registry_quarantine(int h, gen_t gen)
{ CHECK(h >= 0 && (unsigned)h < resource_count && resources[h].generation == gen); resources[h].state = RS_QUARANTINED; quarantines++; return 0; }
int irq_chain_add(unsigned irq, irq_chain_handler_t handler, const char *owner)
{ CHECK(irq == 11 && handler && owner); irq_adds++; return irq_add_fail ? -16 : 0; }
int irq_chain_remove(unsigned irq, irq_chain_handler_t handler, const char *owner)
{ CHECK(irq == 11 && handler && owner); irq_removes++; return 0; }
static struct pci_func functions[2];
unsigned pci_count(void) { return 2; }
const struct pci_func *pci_get(unsigned n) { return n < 2 ? &functions[n] : 0; }

#include "../../../src/kernel/core/irq_budget.c"
#include "../../../src/kernel/drivers/net.c"
void net_stack_driver_service(void) { stack_service_needs_yield=true; }
#include "../../../src/kernel/core/pci_irq.c"
#include "../../../src/kernel/drivers/net_rtl8139.c"
#include "../../../src/kernel/drivers/net_e100.c"
#include "../../../src/kernel/drivers/net_ne2000.c"
#include "../../../src/kernel/drivers/net_e1000.c"
#include "../../../src/kernel/drivers/net_pcnet.c"
#include "../../../src/kernel/drivers/net_tulip.c"
#include "../../../src/kernel/drivers/net_3c90x.c"
#include "../../../src/kernel/drivers/net_rhine.c"
#include "../../../src/kernel/drivers/net_arp.c"
#include "../../../src/kernel/probes/f6_probes_net.c"
#include "../../../src/kernel/drivers/net_dev.c"
#include "../../../src/kernel/proc/devnodes.c"

static struct px_node net_node = { .kind = PX_NET }, other_node = { .kind = PX_MIXER };
static struct file_description net_description = { .node = &net_node, .flags = O_RDONLY };
static struct file_description other_description = { .node = &other_node };
static struct ciuki_net_state user_state;
static int copy_read_error, copy_write_error;
struct proc_object *file_fd_object(const struct process *p, int32_t fd)
{ (void)p; return fd == 3 ? &net_description.object : fd == 4 ? &other_description.object : 0; }
struct file_description *file_fd(const struct process *p, int32_t fd)
{ (void)p; return fd == 3 ? &net_description : fd == 4 ? &other_description : 0; }
int copy_from_user(void *dst, uint32_t src, uint32_t bytes)
{
    if (src != 0x1000 || copy_read_error) return -EFAULT;
    CHECK(bytes == sizeof(user_state)); memcpy(dst, &user_state, bytes); return 0;
}
int copy_to_user(uint32_t dst, const void *src, uint32_t bytes)
{
    if (dst != 0x1000 || copy_write_error) return -EFAULT;
    CHECK(bytes == sizeof(user_state)); memcpy(&user_state, src, bytes); return 0;
}

static unsigned record_count;
static bool terminal_pass;
static char records[8][240];
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    CHECK(!strcmp(probe, "net-arp") && record_count < 8);
    va_list ap; va_start(ap, fmt);
    char fields[200]; if (fmt) kvsnprintf(fields, sizeof(fields), fmt, ap); else fields[0] = 0;
    va_end(ap);
    snprintf(records[record_count++], sizeof(records[0]), "event=%s %s", event, fields);
    if (!strcmp(event, "END")) terminal_pass = !strcmp(fields, "status=PASS");
}
enum model_kind { M_RTL, M_E100, M_NE };
struct model_scb_write { unsigned reg, width; uint32_t value, flags; };
struct model {
    enum model_kind kind;
    struct netif *device;
    uint8_t reg[256], pci[256], card[32768], page1[16];
    uint32_t cfg_address, pointer, rfd_pointer;
    unsigned producer, writes, packet_io, read_cost, stop_count;
    unsigned ee_pins, ee_count, ee_opcode, ee_address, ee_sample, ee_bits;
    uint16_t ee_words[256], mii_status;
    unsigned mii_reads;
    unsigned rfd_count_flags, scb_write_count, command_busy_reads;
    unsigned rfd_status_extra; /* f6-22: error/information bits a test adds */
    bool trace_scb;
    struct model_scb_write scb_writes[32];
    bool reset_stuck, dma_stuck, command_stuck, mdi_stuck, tx_auto, config_reject, latch_low, ee_absent;
    unsigned dma_address, dma_count;
};
static uint32_t model_get(const uint8_t *p, unsigned width)
{ uint32_t v = 0; for (unsigned i = 0; i < width; i++) v |= (uint32_t)p[i] << (8 * i); return v; }
static void model_put(uint8_t *p, uint32_t v, unsigned width)
{ for (unsigned i = 0; i < width; i++) p[i] = v >> (8 * i); }
static void *model_dma(uint32_t phys, unsigned bytes)
{ CHECK(phys >= HOST_PHYS && phys + bytes <= HOST_PHYS + sizeof(memory)); return P2V(phys); }
static uint32_t model_read(void *arg, uint16_t port, unsigned width)
{
    CHECK(!stack_service_needs_yield); /* TX exhausted its dispatch before RX poll */
    struct model *h = arg; cycles += h->read_cost;
    if (port == 0xcfc) {
        unsigned reg = h->cfg_address & 0xfc;
        if (((h->cfg_address >> 11) & 31) == 1) return reg == 0x60 ? 0x0b0b0b0b : 0;
        return model_get(h->pci + reg, width);
    }
    if (port == 0x4d1) return 8;
    CHECK(port >= 0x1000 && port < 0x1100); unsigned reg = port - 0x1000;
    if (h->kind == M_E100 && reg == 2 && h->command_busy_reads) {
        CHECK(width == 1); unsigned value = h->reg[2];
        if (!--h->command_busy_reads) h->reg[2] = 0;
        return value;
    }
    if (h->kind == M_E100 && reg == 0x0e) return h->ee_pins | ((h->ee_absent ? 1 : h->ee_sample) << 3);
    if (h->kind == M_E100 && reg == 0x10) {
        unsigned phy = (model_get(h->reg + 0x10, 4) >> 21) & 31;
        if (h->mdi_stuck) return 0;
        h->mii_reads++;
        uint16_t value = phy == 1 ? h->mii_status : 0xffff;
        if (phy == 1 && h->latch_low) { value &= ~4u; h->latch_low = false; }
        return (1u << 28) | value;
    }
    if (h->kind == M_NE) {
        unsigned page = (h->reg[0] >> 6) & 3;
        if (reg < 16 && page == 1 && reg) return h->page1[reg];
        if (reg == 0x10) {
            CHECK(h->dma_count >= width && width == 2); h->packet_io++;
            CHECK(h->dma_address + width <= sizeof(h->card));
            uint32_t v = model_get(h->card + h->dma_address, width); h->dma_address += width;
            h->dma_count -= width; if (!h->dma_count && !h->dma_stuck) h->reg[7] |= 0x40;
            return v;
        }
        if (reg == 0x1f) { if (!h->reset_stuck) { h->reg[0] = 0x21; h->reg[7] = 0x80; } return 0; }
    }
    return model_get(h->reg + reg, width);
}
static void model_e100_complete_tx(struct model *h)
{
    struct e100_block *cb = model_dma(h->device->dma_phys + E100_TX, sizeof(*cb));
    cb->status = 0xa000; h->reg[0] &= ~0xc0; h->reg[1] |= 0xa0;
}
static void model_write(void *arg, uint16_t port, uint32_t value, unsigned width)
{
    struct model *h = arg;
    if (port == 0xcf8) { CHECK(width == 4); h->cfg_address = value; return; }
    if (port == 0xcfc) {
        CHECK((h->cfg_address & 0xfc) == 4 && width == 2); config_writes++;
        if (!h->config_reject) model_put(h->pci + 4, value, width);
        return;
    }
    CHECK(port >= 0x1000 && port < 0x1100); unsigned reg = port - 0x1000; h->writes++;
    if (h->kind == M_RTL) {
        if (reg == 0x37 && (value & 0x10)) {
            h->stop_count++; h->reg[0x37] = h->reset_stuck ? 0x10 : 1; return;
        }
        if (reg == 0x37) { h->reg[reg] = (value & ~1u) | (h->reg[reg] & 1); return; }
        if (reg == 0x3e) { model_put(h->reg + reg, model_get(h->reg + reg, 2) & ~value, 2); return; }
        if (reg == 0x38) {
            model_put(h->reg + reg, value, width);
            if (((value + 16) % RTL_RING) == h->producer) h->reg[0x37] |= 1;
            return;
        }
        if (reg >= 0x10 && reg < 0x20 && width == 4) {
            unsigned slot = (reg - 0x10) / 4, bytes = value & 0x1fff;
            CHECK(bytes >= 60 && bytes <= NET_FRAME_MAX);
            uint32_t phys = model_get(h->reg + 0x20 + slot * 4, 4);
            (void)model_dma(phys, bytes); h->packet_io++;
            model_put(h->reg + reg, h->tx_auto ? 0xa000 : 0, 4); return;
        }
    }
    if (h->kind == M_E100) {
        if (h->trace_scb && reg <= 4) {
            CHECK(h->scb_write_count < ARRAY_SIZE(h->scb_writes));
            h->scb_writes[h->scb_write_count++] = (struct model_scb_write){reg,width,value,host_flags};
        }
        if (reg == 8) {
            h->stop_count++;
            if (!h->reset_stuck) { model_put(h->reg, 0, 4); h->rfd_pointer = 0; }
            return;
        }
        if (reg == 1) { CHECK(width == 1); h->reg[1] &= ~value; return; }
        if (reg == 3) { CHECK(width == 1); h->reg[3] = value; return; }
        if (reg == 0x0e) {
            if (!(value & 2)) { h->ee_count = h->ee_opcode = h->ee_address = 0; h->ee_sample = 1; }
            else if ((value & 1) && !(h->ee_pins & 1)) {
                unsigned bit = (value >> 2) & 1;
                if (h->ee_count < 3) h->ee_opcode = (h->ee_opcode << 1) | bit;
                else if (h->ee_count < 3 + h->ee_bits) {
                    CHECK(h->ee_opcode == 6); h->ee_address = (h->ee_address << 1) | bit;
                    h->ee_sample = h->ee_count + 1 == 3 + h->ee_bits ? 0 : 1;
                } else {
                    unsigned i = h->ee_count - 3 - h->ee_bits;
                    CHECK(i < 16); h->ee_sample = (h->ee_words[h->ee_address] >> (15 - i)) & 1;
                }
                h->ee_count++;
            }
            h->ee_pins = value; return;
        }
        if (reg == 4) { CHECK(width == 4); h->pointer = value; model_put(h->reg + reg, value, width); return; }
        if (reg == 2) {
            CHECK(width == 1);
            if (h->command_stuck) { h->reg[2] = value; return; }
            if (value == 0x10) {
                struct e100_block *cb = model_dma(h->pointer, sizeof(*cb));
                CHECK(cb->command & 0x8000);
                if ((cb->command & 7) == 2) {
                    const uint8_t *config = (const uint8_t *)cb + 8;
                    CHECK(config[0] == 22 && config[6] == 0x32 && config[8] == 1 && !(config[18] & 4));
                    CHECK(config[12] == (h->device->pci.device == 0x1229 && h->device->pci.revision < 4 ? 0x60 : 0x61));
                }
                if ((cb->command & 7) == 4) {
                    CHECK(cb->buffer == 0xffffffff && (cb->count & 0x3fff) >= 60);
                    CHECK(cb->size == 8); h->packet_io++;
                    if (!h->tx_auto) { h->reg[0] |= 0x80; return; }
                }
                cb->status = 0xa000; h->reg[0] &= ~0xc0;
                h->reg[1] |= (cb->command & 0x2000) ? 0xa0 : 0x20;
            } else if (value == 1) { h->rfd_pointer = h->pointer; h->reg[0] = (h->reg[0] & 0xc0) | 0x10; }
            h->reg[2] = 0; return;
        }
    }
    if (h->kind == M_NE) {
        unsigned page = (h->reg[0] >> 6) & 3;
        if (reg < 16 && page == 1 && reg) { h->page1[reg] = value; return; }
        if (reg == 7) { h->reg[7] &= ~(value & 0x7f); return; }
        if (reg == 0x1f) return;
        if (reg == 0x10) {
            CHECK(width == 2 && h->dma_count >= width && h->dma_address + width <= sizeof(h->card));
            model_put(h->card + h->dma_address, value, width); h->dma_address += width;
            h->dma_count -= width; h->packet_io++;
            if (!h->dma_count && !h->dma_stuck) h->reg[7] |= 0x40;
            return;
        }
        if (reg == 0) {
            h->reg[0] = value & ~4u;
            if (value & 2) h->reg[7] &= ~0x80u;
            if (value & 4) {
                h->packet_io++;
                if (h->tx_auto) { h->reg[7] |= 2; h->reg[4] = 1; }
            }
            if ((value & 0x38) == 8 || (value & 0x38) == 0x10) {
                h->dma_address = h->reg[8] | (unsigned)h->reg[9] << 8;
                h->dma_count = h->reg[0x0a] | (unsigned)h->reg[0x0b] << 8;
            }
            return;
        }
    }
    model_put(h->reg + reg, value, width);
}
static uint64_t model_now(void *arg) { (void)arg; return g_ticks; }
static struct model *route_model;
static uint32_t route_read(uint16_t p, unsigned width) { return model_read(route_model, p, width); }
static void route_write(uint16_t p, uint32_t value) { model_write(route_model, p, value, 4); }
static void model_sleep(void *arg, uint32_t ms) { (void)arg; task_sleep_ms(ms); }
static int model_delay(void *arg, uint32_t us) { (void)arg; CHECK(host_flags & 0x200); cycles += us; return 0; }
static void model_setup(struct model *h, struct netif *d, enum model_kind kind)
{
    /* Simulate an independent cold boot between fixtures, including cases
     * that retain quarantined pages. No live context gets reset in production. */
    memset(h, 0, sizeof(*h)); memset(d, 0, sizeof(*d)); memset(resources, 0, sizeof(resources));
    route_model = h; memset(route_bios, 0, sizeof(route_bios));
    memset(allocated, 0, sizeof(allocated)); memset(memory, 0, sizeof(memory));
    resource_count = page_count = releases = quarantines = config_writes = irq_adds = irq_removes = 0;
    claim_fail = allocation_fail = irq_add_fail = release_fail = 0;
    map_calls = unmap_calls = 0; map_fail = false;
    g_ticks = cycles = sleeps = yields = 0; host_flags = 0x200;
    sleep_hook = 0; sleep_hook_active = false;
    h->kind = kind; h->device = d; h->ee_bits = 6; h->ee_sample = 1; h->mii_status = 0x782d; h->tx_auto = true;
    h->ee_words[0] = 0x5452; h->ee_words[1] = 0x1200; h->ee_words[2] = 0x5634;
    const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
    memcpy(h->reg, mac, 6);
    for (unsigned i = 0; i < 6; i++) h->card[i * 2] = h->card[i * 2 + 1] = mac[i];
    memset(h->card + 28, 0x57, 4);
    if (kind == M_NE) { h->reg[0] = 0x21; h->reg[7] = 0x80; }
    if (kind == M_E100) memset(h->reg, 0, 8);
    g_boot.flags = CBI_F_SMBIOS_QEMU;
    functions[0] = (struct pci_func){.bus=0,.dev=1,.vendor=0x8086,.device=0x7000,.class_code=6,.subclass=1};
    functions[1] = (struct pci_func){.bus=0,.dev=3,.vendor=kind == M_E100 ? 0x8086 : 0x10ec,
        .device=kind == M_RTL ? 0x8139 : kind == M_NE ? 0x8029 : 0x1209,.class_code=2,.command=1,.irq_pin=1,.irq_line=11};
    functions[1].bar[kind == M_E100 ? 1 : 0] = 0x1001;
    model_put(h->pci + 4, 0xabcd0001, 4);
}
static int model_attach(struct model *h, struct netif *d)
{
    struct net_io io = {h,model_read,model_write,model_now,model_sleep,model_delay,0,0};
    return netif_attach(d, &functions[1], &io);
}
static void model_ring_write(struct model *h, unsigned pos, const void *data, unsigned len)
{
    const uint8_t *p = data;
    for (unsigned i = 0; i < len; i++) h->device->dma[(pos + i) % RTL_RING] = p[i];
}
/* f6-22: RFD status as each device writes it. Physical 8255x (Intel
 * developer manual, table 49): C|OK, bit 5 for a type frame (type/length
 * field 0 or > 1500: Ethernet II, so IPv4 and ARP), bit 1 when the
 * destination is not the individual address (broadcast, multicast), bit 2
 * when no filter matched (promiscuous). QEMU eepro100.c nic_receive: 0xa000,
 * |0x0002 for broadcast/multicast, |0x0004 promiscuous unicast; no TL bit.
 * The former model wrote 0xa000 for every frame, which hid the f6-22 bug. */
static unsigned model_e100_rfd_status(const struct model *h, const uint8_t *data, unsigned len)
{
    unsigned status = 0xa000;
    bool group = data[0] & 1, individual = !memcmp(data, h->device->address, 6);
    if (g_boot.flags & CBI_F_SMBIOS_QEMU) {
        if (group) status |= 2;
        else if (!individual) status |= 4;
    } else {
        unsigned type = len >= 14 ? (unsigned)data[12] << 8 | data[13] : 0;
        if (!type || type > 1500) status |= 0x20;
        if (!individual) status |= 2;
        if (!group && !individual) status |= 4;
    }
    return status | h->rfd_status_extra;
}
static void model_receive(struct model *h, const uint8_t *data, unsigned len, bool error)
{
    struct netif *d = h->device;
    if (h->kind == M_RTL) {
        uint8_t header[4]; model_put(header, error ? 4 : 1, 2); model_put(header + 2, len + 4, 2);
        model_ring_write(h, h->producer, header, 4); model_ring_write(h, h->producer + 4, data, len);
        h->producer = (h->producer + 4 + len + 4 + 3) & ~3u; h->producer %= RTL_RING;
        h->reg[0x37] &= ~1u; h->reg[0x3e] |= 1;
    } else if (h->kind == M_E100) {
        CHECK(h->rfd_pointer); struct e100_block *rfd = model_dma(h->rfd_pointer, sizeof(*rfd));
        CHECK(!rfd->status); memcpy(rfd->data, data, len); rfd->count = h->rfd_count_flags | len;
        rfd->status = error ? 0x8800 : model_e100_rfd_status(h, data, len); h->reg[1] |= 0x40;
        if (rfd->command & 0x8000) { h->reg[0] = (h->reg[0] & 0xc0) | 8; h->rfd_pointer = 0; }
        else h->rfd_pointer = rfd->link;
    } else {
        unsigned page = h->page1[7], count = len + 4;
        unsigned next = NE_FIRST + (page - NE_FIRST + (count + 4 + 255) / 256) % (NE_STOP - NE_FIRST);
        uint8_t header[4] = {error ? 2 : 1,next,0,0}; model_put(header + 2, count, 2);
        memcpy(h->card + (page << 8), header, 4);
        for (unsigned i = 0; i < len; i++) h->card[(NE_FIRST << 8) + (((page - NE_FIRST) << 8) + 4 + i) % ((NE_STOP - NE_FIRST) << 8)] = data[i];
        h->page1[7] = next; h->reg[7] |= 1;
    }
    (void)d;
}
static void test_paths(enum model_kind kind)
{
    struct model h; struct netif d; model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d));
    CHECK(d.ready && d.running && d.irq_attached && irq_adds == 1);
    const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56}; CHECK(!memcmp(d.address, mac, 6));
    CHECK(model_get(h.pci + 6, 2) == 0xabcd); /* no PCI Status W1C echo */
    uint8_t frame[NET_FRAME_MAX]; for (unsigned i = 0; i < sizeof(frame); i++) frame[i] = i;
    CHECK(netif_transmit(&d, d.generation + 1, frame, 60) == -19);
    CHECK(netif_transmit(&d, d.generation, frame, NET_FRAME_MAX + 1) == -EINVAL);
    CHECK(!netif_transmit(&d, d.generation, frame, 42));
    netif_poll(&d); CHECK(d.stats.tx_completed == 1 && d.stats.tx_bytes == 60);
    for (unsigned i = 0; i < 80; i++) {
        unsigned len = i % 2 ? NET_FRAME_MAX : 60;
        model_receive(&h, frame, len, false);
        unsigned packet_io = h.packet_io, slept = sleeps;
        uint32_t flags = irq_save(); CHECK(netif_irq(&d)); irq_restore(flags);
        CHECK(packet_io == h.packet_io && sleeps == slept); /* hard IRQ only masks/acks */
        netif_poll(&d); struct net_packet packet; CHECK(netif_pop(&d, &packet));
        CHECK(packet.length == len && !memcmp(packet.data, frame, len));
    }
    CHECK(d.stats.rx_packets == 80 && !d.quarantined);
    model_receive(&h, frame, 60, true); netif_poll(&d);
    CHECK(d.stats.rx_errors == 1 && !d.queue_count && !d.quarantined);
    if (kind == M_RTL) {
        d.rx_offset = h.producer = RTL_RING - 4; model_receive(&h, frame, NET_FRAME_MAX, false); netif_poll(&d);
        struct net_packet packet; CHECK(netif_pop(&d, &packet) && !memcmp(packet.data, frame, NET_FRAME_MAX));
        CHECK(model_get(h.reg + 0x38, 2) == ((d.rx_offset - 16) & 0xffff));
        h.reg[0x58] = 4; CHECK(d.driver->link_state(&d) == NET_LINK_DOWN);
        h.reg[0x58] = 0; CHECK(d.driver->link_state(&d) == NET_LINK_UP);
    } else if (kind == M_E100) {
        h.mii_status &= ~4u; CHECK(d.driver->link_state(&d) == NET_LINK_DOWN);
        h.mii_status |= 4; h.latch_low = true; unsigned before = h.mii_reads;
        CHECK(d.driver->link_state(&d) == NET_LINK_UP && h.mii_reads == before + 2);
    } else {
        CHECK(d.driver->link_state(&d) == NET_LINK_UNKNOWN);
        d.next_page = h.page1[7] = NE_STOP - 1; model_receive(&h, frame, NET_FRAME_MAX - 1, false); netif_poll(&d);
        struct net_packet packet; CHECK(netif_pop(&d, &packet) && packet.length == NET_FRAME_MAX - 1 && !memcmp(packet.data, frame, packet.length));
        CHECK(h.reg[3] == (d.next_page == NE_FIRST ? NE_STOP - 1 : d.next_page - 1));
        CHECK(yields > 0);
    }
    for (unsigned i = 0; i < NET_QUEUE_MAX + 3; i++) { frame[0] = i; model_receive(&h, frame, 60, false); netif_poll(&d); }
    CHECK(d.queue_count == NET_QUEUE_MAX && d.stats.rx_dropped == 3 && d.stats.queue_peak == NET_QUEUE_MAX);
    struct net_stats stats; netif_snapshot(&d, &stats); CHECK(stats.queued == NET_QUEUE_MAX);
    for (unsigned i = 0; i < NET_QUEUE_MAX; i++) { struct net_packet packet; CHECK(netif_pop(&d, &packet) && packet.data[0] == i); }
    CHECK(!netif_stop(&d) && !page_count && !d.irq_attached && !d.running && releases == (kind == M_NE ? 2u : 3u));
    CHECK(model_get(h.pci + 4, 2) == 1 && irq_removes == 1);
    CHECK(netif_transmit(&d, d.generation, frame, 60) == -19);
    CHECK(model_attach(&h, &d) == -16); /* no live context reset */
}
static void test_faults(enum model_kind kind)
{
    struct model h; struct netif d;
    model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d));
    h.reset_stuck = true;
    if (kind == M_E100) h.reg[0] = 0x80;
    CHECK(netif_stop(&d) && d.quarantined && d.stats.quarantines == 1);
    unsigned pages = page_count, writes = h.writes; CHECK(!releases && quarantines == (kind == M_NE ? 2u : 3u));
    CHECK(netif_stop(&d) && h.writes == writes && page_count == pages);
    uint8_t frame[60] = {0}; CHECK(netif_transmit(&d, d.generation, frame, 60) == -19);

    model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d));
    h.read_cost = 60;
    for (unsigned i = 0; i < IRQ_BUDGET_STRIKES; i++) {
        model_receive(&h, frame, 60, false);
        uint32_t flags = irq_save(); CHECK(netif_irq(&d)); irq_restore(flags);
        CHECK(d.fault_pending == (i + 1 == IRQ_BUDGET_STRIKES));
    }
    CHECK(d.stats.budget_violations == IRQ_BUDGET_STRIKES && d.fault_pending);
    netif_poll(&d); CHECK(d.quarantined && !releases);

    model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d)); h.tx_auto = false;
    CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame)));
    unsigned slots = kind == M_RTL ? 4 : 1;
    for (unsigned i = 1; i < slots; i++) CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame)));
    CHECK(netif_transmit(&d, d.generation, frame, sizeof(frame)) == -16);
    g_ticks += NET_TX_TIMEOUT_MS; netif_poll(&d); CHECK(d.quarantined && d.stats.tx_errors && !releases);

    model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d));
    registry_quarantine(d.claims[0], d.claim_generation[0]); netif_poll(&d); CHECK(d.quarantined);

    model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d));
    model_receive(&h, frame, 60, false);
    if (kind == M_RTL) model_put(d.dma + 2, 0xffff, 2);
    else if (kind == M_E100) e100_rfd(&d, 0)->count = 0xffff;
    else h.card[(d.next_page << 8) + 1] = 0x20;
    netif_poll(&d); CHECK(d.quarantined && !d.queue_count);
}
static void test_admission(enum model_kind kind)
{
    struct model h; struct netif d;
    model_setup(&h, &d, kind); g_boot.flags |= CBI_F_SAFE_MODE;
    CHECK(model_attach(&h, &d) == -19 && !h.writes && !config_writes && !page_count);
    model_setup(&h, &d, kind); functions[1].bar[kind == M_E100 ? 1 : 0] += 4;
    CHECK(model_attach(&h, &d) == -19 && !h.writes && !config_writes);
    model_setup(&h, &d, kind);
    if (kind == M_RTL) h.reg[0x37] = 0x0c;
    else if (kind == M_E100) h.reg[0] = 0x10;
    else h.reg[0] = 0x22;
    CHECK(model_attach(&h, &d) == -16 && !h.writes && !config_writes);
    model_setup(&h, &d, kind); claim_fail = 2;
    CHECK(!model_attach(&h, &d) && d.running && !d.irq_attached && !irq_adds);
    CHECK(!netif_stop(&d) && !page_count);
    if (kind != M_NE) {
        model_setup(&h, &d, kind); allocation_fail = 1;
        CHECK(model_attach(&h, &d) == -ENOMEM && releases == 2 && !h.writes);
        model_setup(&h, &d, kind); claim_fail = 3;
        CHECK(model_attach(&h, &d) == -16 && releases == 2 && !page_count && !h.writes);
    }
    model_setup(&h, &d, kind); g_boot.flags = 0; functions[1].bus = 1;
    CHECK(!model_attach(&h, &d) && !d.irq_attached && !irq_adds);
    CHECK(!netif_stop(&d));
    model_setup(&h, &d, kind); g_boot.flags=0; functions[1].bus=2;
    functions[0].device=0x248c;
    memcpy(route_bios,"$PIR",4);route_bios[5]=1;route_bios[6]=48;route_bios[9]=8;
    route_bios[32]=2;route_bios[33]=3*8;route_bios[34]=0x60;route_bios[36]=8;
    for(unsigned i=0;i<48;i++)if(i!=31)route_bios[31]-=route_bios[i];
    CHECK(!model_attach(&h,&d) && d.irq_attached && irq_adds==1);
    CHECK(!netif_stop(&d));
    model_setup(&h, &d, kind); irq_add_fail = 1;
    CHECK(!model_attach(&h, &d) && d.running && !d.quarantined && !d.irq_attached);
    CHECK(!netif_stop(&d) && !page_count);
    model_setup(&h, &d, kind); h.reset_stuck = true;
    if (kind == M_RTL) CHECK(model_attach(&h, &d) == -110 && sleeps <= 100);
    else if (kind == M_NE) { h.reg[7] = 0; CHECK(model_attach(&h, &d) == -110 && sleeps <= 100); }
    else { h.mdi_stuck = true; CHECK(model_attach(&h, &d) == -110 && sleeps <= 21); }
    CHECK(d.quarantined);
}
static void test_arp(void)
{
    uint8_t frame[60], local[6] = {0x52,0x54,0,0x12,0x34,0x56}, peer[6] = {0x52,0x55,10,0,2,2}, result[6];
    CHECK(net_arp_request(frame, local) == 60 && frame[21] == 1);
    memcpy(frame, local, 6); memcpy(frame + 6, peer, 6); frame[21] = 2;
    memcpy(frame + 22, peer, 6); memcpy(frame + 32, local, 6);
    frame[31] = 2; frame[41] = 15;
    CHECK(net_arp_reply(frame, 60, local, result) && !memcmp(result, peer, 6));
    for (unsigned i = 0; i < 42; i++) {
        unsigned saved = frame[i]; frame[i] ^= 1;
        CHECK(!net_arp_reply(frame, 60, local, result)); frame[i] = saved;
    }
    CHECK(!net_arp_reply(frame, 41, local, result));
}
static struct model *probe_hardware;
static bool inject_reply, reply_injected, wrong_target;
static void probe_step(void)
{
    if (probe_hardware->device->mutex.owner) return;
    if (inject_reply && !reply_injected) {
        uint8_t frame[60], peer[6] = {0x52,0x55,10,0,2,2};
        struct netif *d = probe_hardware->device; net_arp_request(frame, d->address);
        memcpy(frame, d->address, 6); memcpy(frame + 6, peer, 6); frame[21] = 2;
        memcpy(frame + 22, peer, 6); memcpy(frame + 32, d->address, 6);
        frame[31] = 2; frame[41] = wrong_target ? 16 : 15;
        model_receive(probe_hardware, frame, sizeof(frame), false); reply_injected = true;
        uint32_t flags = irq_save(); CHECK(netif_irq(d)); irq_restore(flags);
    }
    net_worker_step();
}
static void test_probe(enum model_kind kind)
{
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        struct model h; struct netif *d = &devices[0]; model_setup(&h, d, kind);
        CHECK(!model_attach(&h, d)); device_count = 1;
        probe_hardware = &h; inject_reply = scenario != 1; reply_injected = false; wrong_target = scenario == 2;
        record_count = 0; terminal_pass = false; memset(records, 0, sizeof(records));
        sleep_hook = probe_step;
        CHECK(probe_net_arp() == (scenario ? -5 : 0));
        sleep_hook = 0; CHECK(terminal_pass == !scenario && record_count == 6);
        CHECK(strstr(records[2], scenario ? "arp=fail" : "arp=ok mac=52:55:0a:00:02:02"));
        CHECK(!d->running && !d->irq_attached && !page_count && !input_active && !input);
        CHECK(!scenario || g_ticks >= 5000);
    }
    device_count = 0;
}
static void test_additional_faults(void)
{
    struct model h; struct netif d;
    model_setup(&h, &d, M_NE); h.dma_stuck = true;
    CHECK(model_attach(&h, &d) == -110 && d.quarantined && sleeps <= 20);
    model_setup(&h, &d, M_NE);
    const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
    memset(h.card, 0, 32); memcpy(h.card, mac, 6); memcpy(h.card + 16, mac, 6);
    h.card[14] = h.card[15] = 0x57; h.card[30] = h.card[31] = 0x42;
    CHECK(!model_attach(&h, &d) && !memcmp(d.address, mac, 6)); CHECK(!netif_stop(&d));
    model_setup(&h, &d, M_NE); memset(h.card + 28, 0, 4);
    CHECK(model_attach(&h, &d) == -5 && d.quarantined);
    model_setup(&h, &d, M_E100); h.ee_absent = true;
    CHECK(model_attach(&h, &d) == -5 && d.quarantined);
    model_setup(&h, &d, M_E100); h.command_stuck = true;
    CHECK(model_attach(&h, &d) == -110 && d.quarantined && sleeps <= 101);
    model_setup(&h, &d, M_E100); memset(h.ee_words, 0, sizeof(h.ee_words));
    CHECK(model_attach(&h, &d) == -5 && d.quarantined);
    model_setup(&h, &d, M_RTL); h.config_reject = true;
    CHECK(model_attach(&h, &d) == -5 && d.quarantined);
    model_setup(&h, &d, M_RTL); CHECK(!model_attach(&h, &d)); release_fail = 1;
    CHECK(netif_stop(&d) && d.quarantined && page_count == 5 && !releases);
    model_setup(&h, &d, M_E100); CHECK(!model_attach(&h, &d));
    uint8_t frame[60] = {0};
    for (unsigned i = 0; i < E100_RX; i++) model_receive(&h, frame, sizeof(frame), false);
    CHECK(!h.rfd_pointer); /* end marker stops the device, never overwrites */
    for (unsigned i = 0; i < E100_RX; i++) netif_poll(&d);
    CHECK(d.stats.rx_packets == E100_RX && h.rfd_pointer && !d.quarantined);
    CHECK(!netif_stop(&d));
    model_setup(&h, &d, M_NE); CHECK(!model_attach(&h, &d));
    CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame))); h.reg[7] = 8; h.reg[4] = 0x10;
    netif_poll(&d); CHECK(d.stats.tx_errors == 1 && !d.tx_used[0] && !d.quarantined); CHECK(!netif_stop(&d));
    model_setup(&h, &d, M_RTL); CHECK(!model_attach(&h, &d));
    CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame))); model_put(h.reg + 0x10, 1u << 30, 4);
    netif_poll(&d); CHECK(d.quarantined && d.stats.tx_errors == 1);
    model_setup(&h, &d, M_RTL); CHECK(!model_attach(&h, &d)); h.reg[0x58] = 0xff; g_ticks += 1000;
    netif_poll(&d); CHECK(d.quarantined);
    model_setup(&h, &d, M_E100); CHECK(!model_attach(&h, &d)); h.mdi_stuck = true; g_ticks += 1000;
    netif_poll(&d); CHECK(d.quarantined);
    for (enum model_kind kind = M_RTL; kind <= M_NE; kind++) {
        model_setup(&h, &d, kind); model_put(h.pci + 4, 0xabdd0001, 4); h.pci[0x34] = 0x40;
        model_put(h.pci + 0x40, 1, 4); model_put(h.pci + 0x44, 3, 4);
        CHECK(model_attach(&h, &d) == -16 && !h.writes && !config_writes);
        model_setup(&h, &d, kind); CHECK(!model_attach(&h, &d)); h.tx_auto = false;
        CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame)));
        CHECK(!netif_stop(&d) && d.stats.tx_errors == 1 && !page_count);
    }
}
static int state_query(void)
{ return net_devctl(0, 3, NET_GET_STATE, 0x1000, sizeof(user_state)); }
static void test_public_state(void)
{
    device_count = 0; g_boot.flags = 0;
    user_state = (struct ciuki_net_state){ .version = 1, .size = sizeof(user_state) };
    CHECK(!device_open(PX_NET, O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    CHECK(device_open(PX_NET, O_WRONLY) == -EACCES && device_open(PX_NET, O_RDWR) == -EACCES);
    const uint32_t forbidden[] = {O_CREAT, O_EXCL, O_TRUNC, O_DIRECTORY, O_APPEND};
    for (unsigned i = 0; i < ARRAY_SIZE(forbidden); i++) CHECK(device_open(PX_NET, forbidden[i]) == -EINVAL);
    CHECK(!state_query() && !user_state.present && user_state.link == CIUKI_NET_LINK_UNKNOWN);
    CHECK(user_state.state == CIUKI_NET_STOPPED && !strcmp(user_state.reason, "no_adapter"));
    CHECK(!user_state.driver[0] && !user_state.address_configured && !memcmp(user_state.ipv4, "\0\0\0\0", 4));
    CHECK(!memcmp(user_state.mac, "\0\0\0\0\0\0", 6));
    g_boot.flags = CBI_F_SAFE_MODE; CHECK(!state_query() && !strcmp(user_state.reason, "safe_mode"));
    for (enum model_kind kind = M_RTL; kind <= M_NE; kind++) {
        struct model h; struct netif *d = &devices[0]; model_setup(&h, d, kind);
        CHECK(!model_attach(&h, d)); device_count = 1;
        d->stats.rx_packets = 0x100000001ull; d->stats.tx_completed = 0x200000002ull;
        unsigned writes = h.writes, pci_writes = config_writes;
        user_state = (struct ciuki_net_state){ .version = 1, .size = sizeof(user_state) };
        CHECK(!state_query() && user_state.present && user_state.link == (uint32_t)d->link);
        CHECK(user_state.link == (kind == M_NE ? CIUKI_NET_LINK_UNKNOWN : CIUKI_NET_LINK_UP));
        CHECK(!strcmp(user_state.driver, d->driver->name) && !memcmp(user_state.mac, d->address, 6));
        CHECK(user_state.state == CIUKI_NET_RUNNING && !strcmp(user_state.reason, "admission"));
        CHECK(user_state.rx_frames == 0x100000001ull && user_state.tx_frames == 0x200000002ull);
        CHECK(!user_state.address_configured && !memcmp(user_state.ipv4, "\0\0\0\0", 4));
        CHECK(h.writes == writes && config_writes == pci_writes);
        d->link = NET_LINK_DOWN; CHECK(!state_query() && user_state.link == CIUKI_NET_LINK_DOWN);
        CHECK(!netif_stop(d)); CHECK(!state_query() && user_state.state == CIUKI_NET_STOPPED && user_state.present);
        netif_quarantine(d, "test_fault");
        CHECK(!state_query() && user_state.state == CIUKI_NET_QUARANTINED && user_state.present);
        CHECK(!strcmp(user_state.reason, "test_fault"));
    }
    user_state.version = 2; CHECK(state_query() == -EOPNOTSUPP); user_state.version = 1;
    user_state.size--; CHECK(state_query() == -EINVAL); user_state.size++;
    user_state.reserved0[0] = 1; CHECK(state_query() == -EINVAL); user_state.reserved0[0] = 0;
    user_state.reserved1 = 1; CHECK(state_query() == -EINVAL); user_state.reserved1 = 0;
    CHECK(net_devctl(0, 3, NET_GET_STATE, 0x1000, sizeof(user_state) - 1) == -EINVAL);
    CHECK(net_devctl(0, 3, MIXER_SET_MASTER, 0x1000, sizeof(user_state)) == -ENOTTY);
    CHECK(net_devctl(0, 4, NET_GET_STATE, 0x1000, sizeof(user_state)) == -ENOTTY);
    CHECK(net_devctl(0, -1, NET_GET_STATE, 0x1000, sizeof(user_state)) == -EBADF);
    CHECK(net_devctl(0, 3, NET_GET_STATE, 0, sizeof(user_state)) == -EFAULT);
    copy_read_error = 1; CHECK(state_query() == -EFAULT); copy_read_error = 0;
    copy_write_error = 1; CHECK(state_query() == -EFAULT); copy_write_error = 0;
    /* A failed first adapter must not hide a working second one. */
    devices[1] = devices[0]; devices[1].quarantined = false; devices[1].running = true;
    devices[1].link = NET_LINK_UP; device_count = 2;
    CHECK(!state_query() && user_state.state == CIUKI_NET_RUNNING && user_state.link == CIUKI_NET_LINK_UP);
    device_count = 0;
    puts("PASS /dev/net: absent, safe mode, three adapters, carrier, stopped/quarantined, counters, readonly, ABI and copy failures");
}
#include "second_set_test.c"
#include "e100_irq_test.c"
static unsigned callback_work_us;
static bool timed_e100_interrupt(struct netif *d)
{
    bool handled = net_e100.interrupt(d);
    cycles += callback_work_us;
    return handled;
}
static void test_smi_budget(void)
{
    struct model h; struct netif d;
    model_setup(&h, &d, M_E100); CHECK(!model_attach(&h, &d));
    uint8_t frame[60] = {0};
    model_receive(&h, frame, sizeof(frame), false);
    /* Fourth serialized timestamp: second read of the end probe, no driver
     * work between the two reads. Production e100 interrupt still runs. */
    stall_read = 4;
    uint32_t f = irq_save(); CHECK(netif_irq(&d)); irq_restore(f);
    CHECK(d.stats.smi_attributed == 1 && !d.stats.budget_violations && !d.fault_pending);
    CHECK(d.stats.max_irq_us == 2100);
    netif_poll(&d); CHECK(d.running && !d.quarantined);
    struct netif_driver slow = net_e100; slow.interrupt = timed_e100_interrupt; d.driver = &slow;
    callback_work_us = 2100; /* same elapsed magnitude, actual callback work */
    for (unsigned i = 0; i < 3; i++) {
        model_receive(&h, frame, sizeof(frame), false);
        f = irq_save(); CHECK(netif_irq(&d)); irq_restore(f);
        CHECK(d.stats.smi_attributed == 1 && d.stats.budget_violations == i + 1);
        CHECK(d.fault_pending == (i == 2));
        if (i < 2) { netif_poll(&d); CHECK(!d.quarantined); }
    }
    netif_poll(&d); CHECK(d.quarantined);
    model_setup(&h, &d, M_E100); CHECK(!model_attach(&h, &d)); d.driver = &slow;
    for (unsigned i = 0; i < 5; i++) {
        cycles += 101000;
        model_receive(&h, frame, sizeof(frame), false);
        f = irq_save(); CHECK(netif_irq(&d)); irq_restore(f);
        CHECK(!d.fault_pending);
        netif_poll(&d); CHECK(!d.quarantined);
    }
    CHECK(!netif_stop(&d)); callback_work_us = 0;
    puts("net F0-39: 2100us no-work SMI vs same-length real work; third strike quarantines, sparse overruns survive PASS");
}
int main(void)
{
    test_smi_budget();
    test_e100_irq();
    for (enum model_kind kind = M_RTL; kind <= M_NE; kind++) { test_paths(kind); test_faults(kind); test_admission(kind); }
    struct model h; struct netif d; model_setup(&h, &d, M_E100); h.ee_bits = 8;
    CHECK(!model_attach(&h, &d) && d.eeprom_bits == 8); CHECK(!netif_stop(&d));
    test_additional_faults();
    for (enum model_kind kind = M_RTL; kind <= M_NE; kind++) test_probe(kind);
    test_arp();
    test_public_state();
    test_second_set();
    printf("net: %u checks PASS (production framework, eight NIC register/DMA models, ARP)\n", checks);
    return 0;
}
