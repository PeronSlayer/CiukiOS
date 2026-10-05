/*
 * Minimal VirtIO GPU 2D driver for the CiukiOS V86 session.
 *
 * Sources: OASIS VirtIO 1.2 PCI transport and GPU 2D protocol:
 * https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
 * QEMU's virtio-vga wrapper retains the VGA-compatible device and places the
 * modern VirtIO regions in BAR2 (including when it forces VirtIO 1.0):
 * https://github.com/qemu/qemu/blob/v11.1.1/hw/display/virtio-vga.c
 * The guest uses VERSION_1 and 2D resource commands only; no guest 3D command stream or Mesa dependency.
 */
#include <stdint.h>
#include "session_gpu.h"

extern uint32_t cvdev_in(uint32_t port, uint32_t size);
extern void cvdev_out(uint32_t port, uint32_t value, uint32_t size);
extern uint32_t cvgpu_clock_khz(void);
extern void cvvid_rdtsc(uint32_t *low, uint32_t *high);
extern void cvgpu_barrier(void);

#define GPU_MAX_BYTES       0x01000000UL
#define GPU_PAGE            4096UL
#define GPU_QUEUE_SIZE      32U
#define GPU_INIT_TIMEOUT    50000UL
#define GPU_STATUS_ACK      1U
#define GPU_STATUS_DRIVER   2U
#define GPU_STATUS_DRIVER_OK 4U
#define GPU_DESC_NEXT       1U
#define GPU_DESC_WRITE      2U

#define PCI_ADDR 0xCF8U
#define PCI_DATA 0xCFCU
#define PCI_CAP_VENDOR 0x09U
#define VIRTIO_VENDOR 0x1AF4U
#define VIRTIO_GPU_DEVICE 0x1050U

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100U
#define VIRTIO_GPU_CMD_GET_EDID 0x010AU
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101U
#define VIRTIO_GPU_RESP_OK_EDID 0x1104U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101U
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103U
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105U
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100U
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2U

/* Host-provided V86-safe services; map returns a linear address or zero. */
extern uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes);
extern void cvgpu_unmap_mmio(uint32_t linear);
extern uint32_t cvgpu_alloc(uint32_t bytes);
extern void cvgpu_free(uint32_t linear);
extern uint32_t cvgpu_phys(uint32_t linear);
static uint16_t cvgpu_in16(uint16_t port) { return (uint16_t)cvdev_in(port, 2U); }
static uint32_t cvgpu_in32(uint16_t port) { return cvdev_in(port, 4U); }
static void cvgpu_out16(uint16_t port, uint16_t value) { cvdev_out(port, value, 2U); }
static void cvgpu_out32(uint16_t port, uint32_t value) { cvdev_out(port, value, 4U); }

typedef struct { uint32_t address_lo, address_hi, length; uint16_t flags, next; } gpu_desc;
typedef struct { uint16_t flags, index; uint16_t ring[GPU_QUEUE_SIZE]; uint16_t used_event; } gpu_avail;
typedef struct { uint16_t flags, index; struct { uint32_t id, length; } ring[GPU_QUEUE_SIZE]; uint16_t avail_event; } gpu_used;

typedef struct {
    uint32_t type, flags, fence_lo, fence_hi, context, padding;
} gpu_ctrl_header;
typedef struct { gpu_ctrl_header h; uint32_t resource, format, width, height; } gpu_create;
typedef struct { gpu_ctrl_header h; uint32_t resource, entries; } gpu_attach;
typedef struct { uint32_t address_lo, address_hi, length, padding; } gpu_mem_entry;
typedef struct { gpu_ctrl_header h; uint32_t x, y, width, height, offset_lo, offset_hi, resource, padding; } gpu_transfer;
typedef struct { gpu_ctrl_header h; uint32_t x, y, width, height, scanout, resource; } gpu_scanout;
typedef struct { gpu_ctrl_header h; uint32_t x, y, width, height, resource, padding; } gpu_flush;
typedef struct { uint32_t type, flags, fence_lo, fence_hi, context, padding; } gpu_response;

static uint32_t g_common, g_notify, g_isr, g_device;
static uint32_t g_common_len, g_notify_len, g_isr_len;
static uint32_t g_queue, g_command, g_response, g_framebuffer, g_staging;
static uint32_t g_framebuffer_bytes, g_pitch, g_virtual_width, g_virtual_height;
static uint32_t g_width, g_height, g_resource, g_fence;
static uint8_t g_pci_bus, g_pci_dev, g_pci_fn;
static uint16_t g_pci_command;
static uint8_t g_pci_saved;
static uint32_t g_notify_multiplier;
static uint16_t g_queue_size;
static uint8_t g_bound, g_dirty, g_pending, g_fatal, g_staging_valid;
static uint16_t g_wait_used;
static uint32_t g_last_y;
static uint8_t g_initializing;
static uint32_t g_init_start_lo, g_init_start_hi;
static uint32_t g_features, g_init_response_bytes;
uint8_t cvgpu_edid[128];
uint32_t cvgpu_edid_bytes;
volatile cvgpu_status_info cvgpu_status = { 'C','V','G','P','U','0','0','1', sizeof(cvgpu_status_info), 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };

/* Bounded timing telemetry. Timestamps are guest TSC, not monitor vsync.
 * A reader can snapshot 32 KiB once; no serial output or growing files. */
volatile struct {
    uint8_t magic[8];
    uint32_t bytes, head, khz;
    uint32_t entries[2048][4];
} cvgpu_cadence = {{'C','V','P','A','C','E','0','1'}, 32788U, 0U, 0U, {{0U}}};
void cvgpu_trace(uint32_t event, uint32_t value)
{
    uint32_t lo, hi, n = cvgpu_cadence.head;
    cvvid_rdtsc(&lo, &hi);
    cvgpu_cadence.khz = cvgpu_clock_khz();
    cvgpu_cadence.entries[n & 2047U][0] = lo;
    cvgpu_cadence.entries[n & 2047U][1] = hi;
    cvgpu_cadence.entries[n & 2047U][2] = event;
    cvgpu_cadence.entries[n & 2047U][3] = value;
    cvgpu_cadence.head = n + 1U;
}

static void gpu_zero(uint32_t address, uint32_t bytes)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)address;
    for (i = 0; i < bytes; ++i) p[i] = 0;
}

static void gpu_copy(uint32_t destination, uint32_t source, uint32_t bytes)
{
    uint32_t i, words = bytes >> 2;
    uint32_t *d = (uint32_t *)destination;
    uint32_t *s = (uint32_t *)source;
    for (i = 0; i < words; ++i) d[i] = s[i];
}

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t address = 0x80000000UL | ((uint32_t)bus << 16) |
                       ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                       ((uint32_t)reg & 0xFCU);
    cvgpu_out32(PCI_ADDR, address);
    return cvgpu_in32(PCI_DATA);
}

static uint32_t gpu_deadline(uint32_t usec)
{
    uint32_t khz = cvgpu_clock_khz();
    uint32_t cycles_per_usec = khz / 1000UL;
    if (cycles_per_usec == 0) return 0;
    if (usec > 0xFFFFFFFFUL / cycles_per_usec) return 0xFFFFFFFFUL;
    return cycles_per_usec * usec;
}

static uint8_t gpu_expired(uint32_t start_hi, uint32_t start_lo, uint32_t ticks)
{
    uint32_t hi, lo, elapsed;
    cvvid_rdtsc(&lo, &hi);
    if (hi == start_hi && lo >= start_lo) elapsed = lo - start_lo;
    else if (hi == start_hi + 1U && lo < start_lo) elapsed = lo - start_lo;
    else return 1;
    return elapsed >= ticks;
}

static uint32_t pci_find_gpu(uint8_t *bus_out, uint8_t *dev_out, uint8_t *fn_out)
{
    uint16_t dev;
    uint8_t fn;
    uint32_t id, hdr;
    /* QEMU's supported single-segment PC profile places onboard devices on bus 0. */
    for (dev = 0; dev < 32; ++dev) {
        id = pci_read(0, (uint8_t)dev, 0, 0);
        if ((id & 0xFFFFU) == 0xFFFFU) continue;
        hdr = pci_read(0, (uint8_t)dev, 0, 0x0C);
        for (fn = 0; fn < (((hdr >> 16) & 0x80U) ? 8U : 1U); ++fn) {
            id = pci_read(0, (uint8_t)dev, fn, 0);
            if ((id & 0xFFFFU) == VIRTIO_VENDOR && (id >> 16) == VIRTIO_GPU_DEVICE) {
                *bus_out = 0; *dev_out = (uint8_t)dev; *fn_out = fn; return 1;
            }
        }
    }
    return 0;
}

static void gpu_unmap_all(void)
{
    if (g_device) cvgpu_unmap_mmio(g_device);
    if (g_isr) cvgpu_unmap_mmio(g_isr);
    if (g_notify) cvgpu_unmap_mmio(g_notify);
    if (g_common) cvgpu_unmap_mmio(g_common);
    g_device = g_isr = g_notify = g_common = 0;
    g_common_len = g_notify_len = g_isr_len = 0;
}

static uint32_t gpu_map_cap(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar,
                            uint32_t offset, uint32_t length)
{
    uint32_t barlo, barhi, phys;
    if (length == 0 || length > 0x10000UL || offset > 0x01000000UL ||
        offset > 0xFFFFFFFFUL - length) return 0;
    if (bar > 5) return 0;
    barlo = pci_read(bus, dev, fn, (uint8_t)(0x10U + bar * 4U));
    if ((barlo & 1U) || (barlo & 6U) == 2U || (barlo & 6U) == 6U) return 0;
    if ((barlo & 6U) == 4U) {
        if (bar == 5) return 0;
        barhi = pci_read(bus, dev, fn, (uint8_t)(0x14U + bar * 4U));
        if (barhi != 0) return 0; /* DMA/MMIO must be below 4 GiB in this driver. */
    }
    phys = (barlo & 0xFFFFFFF0UL) + offset;
    if (!(barlo & 0xFFFFFFF0UL) || phys < (barlo & 0xFFFFFFF0UL) ||
        phys > 0xFFFFFFFFUL - length) return 0;
    return cvgpu_map_mmio(phys, length);
}

static uint32_t gpu_find_caps(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t command, status, cap, next, header, offset, length, value;
    uint8_t seen = 0, type, bar;
    command = pci_read(bus, dev, fn, 4);
    {
        uint32_t address = 0x80000000UL | ((uint32_t)bus << 16) |
                           ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | 4U;
        cvgpu_out32(PCI_ADDR, address);
        /* This driver polls its queues. NO_INTERRUPT in avail is advisory;
           mask this function's INTx so a shared NIC line stays independent. */
        cvgpu_out16(PCI_DATA, (uint16_t)((command & 0xFFFFU) | 0x0406U));
    }
    status = pci_read(bus, dev, fn, 4);
    if (!(status & 0x00100000UL)) return 0;
    header = pci_read(bus, dev, fn, 0x34);
    cap = header & 0xFCU;
    while (cap >= 0x40U && cap < 0x100U && seen++ < 48U) {
        value = pci_read(bus, dev, fn, (uint8_t)cap);
        if ((value & 0xFFU) != PCI_CAP_VENDOR) { cap = (value >> 8) & 0xFCU; continue; }
        next = (value >> 8) & 0xFFU;
        header = pci_read(bus, dev, fn, (uint8_t)(cap + 4U));
        type = (uint8_t)(value >> 24);
        bar = (uint8_t)(header & 0xFFU);
        offset = pci_read(bus, dev, fn, (uint8_t)(cap + 8U));
        length = pci_read(bus, dev, fn, (uint8_t)(cap + 12U));
        if (type == 1U && !g_common) {
            g_common_len = length; g_common = gpu_map_cap(bus, dev, fn, bar, offset, length);
        } else if (type == 2U && !g_notify) {
            g_notify_len = length; g_notify = gpu_map_cap(bus, dev, fn, bar, offset, length);
            g_notify_multiplier = pci_read(bus, dev, fn, (uint8_t)(cap + 16U));
        } else if (type == 3U && !g_isr) {
            g_isr_len = length; g_isr = gpu_map_cap(bus, dev, fn, bar, offset, length);
        } else if (type == 4U && !g_device) {
            g_device = gpu_map_cap(bus, dev, fn, bar, offset, length);
        }
        cap = next & 0xFCU;
    }
    return g_common && g_notify && g_isr && g_device && g_common_len >= 56U &&
           g_notify_len >= 20U && g_isr_len >= 1U;
}

static volatile uint8_t *gpu_common8(uint32_t off) { return (volatile uint8_t *)(g_common + off); }
static uint32_t gpu_queue_init(void)
{
    uint32_t phys, queue_linear;
    uint16_t max_size;
    g_queue = cvgpu_alloc(GPU_PAGE);
    if (!g_queue) return 0;
    phys = cvgpu_phys(g_queue);
    if ((g_queue & 4095U) || (phys & 4095U) || phys > 0xFFFFF000UL) return 0;
    queue_linear = g_queue;
    *(volatile uint16_t *)(g_common + 16U) = 0xFFFFU;
    *(volatile uint16_t *)(g_common + 22U) = 0;
    max_size = *(volatile uint16_t *)(g_common + 24U);
    if (max_size < GPU_QUEUE_SIZE) return 0;
    g_queue_size = GPU_QUEUE_SIZE;
    *(volatile uint16_t *)(g_common + 24U) = GPU_QUEUE_SIZE;
    /* One page: desc[0..511], avail at 512, used at 608 (16-byte aligned). */
    *(volatile uint32_t *)(g_common + 32U) = phys;
    *(volatile uint32_t *)(g_common + 36U) = 0;
    *(volatile uint32_t *)(g_common + 40U) = phys + 512U;
    *(volatile uint32_t *)(g_common + 44U) = 0;
    *(volatile uint32_t *)(g_common + 48U) = phys + 608U;
    *(volatile uint32_t *)(g_common + 52U) = 0;
    ((volatile gpu_avail *)(queue_linear + 512U))->flags = 1U; /* VIRTQ_AVAIL_F_NO_INTERRUPT */
    *(volatile uint16_t *)(g_common + 26U) = 0xFFFFU;
    *(volatile uint16_t *)(g_common + 28U) = 1;
    if (*(volatile uint16_t *)(g_common + 28U) != 1U) return 0;
    return queue_linear;
}

static void gpu_set_desc(uint16_t index, uint32_t linear, uint32_t length, uint16_t flags, uint16_t next)
{
    volatile gpu_desc *d = (volatile gpu_desc *)g_queue + index;
    uint32_t p = cvgpu_phys(linear);
    d->address_lo = p; d->address_hi = 0; d->length = length;
    d->flags = flags; d->next = next;
}

static uint32_t gpu_submit(uint32_t command_linear, uint32_t command_bytes,
                           uint32_t response_linear, uint32_t response_bytes,
                           uint32_t timeout_usec)
{
    volatile gpu_desc *desc = (volatile gpu_desc *)g_queue;
    volatile gpu_avail *avail = (volatile gpu_avail *)(g_queue + 512U);
    volatile gpu_used *used = (volatile gpu_used *)(g_queue + 608U);
    uint32_t start_hi, start_lo, ticks, status;
    uint16_t old_used, notify_off, ndesc, i;
    uint32_t left, ptr, page_left, chunk, spins;
    uint16_t idx;
    if (command_bytes == 0 || response_bytes == 0) return 0;
    /* Build page-bounded readable descriptors, then a writable response. */
    notify_off = *(volatile uint16_t *)(g_common + 30U);
    status = (uint32_t)notify_off * g_notify_multiplier;
    if (g_notify_len < 2U || status > g_notify_len - 2U) return 0;
    ndesc = 0; ptr = command_linear; left = command_bytes;
    while (left) {
        page_left = GPU_PAGE - (ptr & (GPU_PAGE - 1U));
        chunk = left < page_left ? left : page_left;
        if (ndesc >= GPU_QUEUE_SIZE - 1U) return 0;
        idx = ndesc;
        gpu_set_desc(idx, ptr, chunk, GPU_DESC_NEXT, (uint16_t)(idx + 1U));
        ptr += chunk; left -= chunk; ++ndesc;
    }
    gpu_set_desc(ndesc, response_linear, response_bytes, GPU_DESC_WRITE, 0);
    for (i = 0; i < ndesc; ++i) desc[i].flags |= GPU_DESC_NEXT;
    desc[ndesc - 1U].next = ndesc;
    desc[ndesc - 1U].flags = GPU_DESC_NEXT;
    desc[ndesc].flags = GPU_DESC_WRITE;
    old_used = used->index;
    avail->ring[avail->index % g_queue_size] = 0;
    cvgpu_barrier();
    ++avail->index;
    cvgpu_barrier();
    /* Compiler/CPU ordering before notifying device. */
    *(volatile uint16_t *)(g_notify + status) = 0;
    if (!timeout_usec) {
        g_wait_used = old_used;
        cvvid_rdtsc(&g_init_start_lo, &g_init_start_hi);
        return 1;
    }
    cvvid_rdtsc(&start_lo, &start_hi);
    ticks = gpu_deadline(timeout_usec);
    spins = 0;
    while (used->index == old_used) {
        if (ticks && gpu_expired(start_hi, start_lo, ticks)) return 0;
        if (!ticks && ++spins >= 1000000UL) return 0;
    }
    cvgpu_barrier();
    if (used->ring[old_used % g_queue_size].id != 0 ||
        used->ring[old_used % g_queue_size].length < sizeof(gpu_response)) return 0;
    return 1;
}

static uint32_t gpu_command(uint32_t request, uint32_t request_size, uint32_t timeout)
{
    volatile gpu_response *resp = (volatile gpu_response *)g_response;
    gpu_ctrl_header *h = (gpu_ctrl_header *)request;
    gpu_zero(g_response, 64U);
    if (!gpu_submit(request, request_size, g_response, sizeof(gpu_response), timeout)) {
        cvgpu_status.error_stage |= 0x10000UL;
        return 0;
    }
    return resp->type == VIRTIO_GPU_RESP_OK_NODATA &&
           (!(h->flags & 1U) || ((resp->flags & 1U) &&
            resp->fence_lo == h->fence_lo && resp->fence_hi == h->fence_hi));
}

/* Submit one command and poll its fenced response on a later desktop pass. */
static uint32_t gpu_submit_frame(void)
{
    volatile gpu_desc *desc = (volatile gpu_desc *)g_queue;
    volatile gpu_avail *avail = (volatile gpu_avail *)(g_queue + 512U);
    volatile gpu_used *used = (volatile gpu_used *)(g_queue + 608U);
    uint32_t req[2], res[2], sizes[2], i, notify;
    uint16_t notify_off, head, available;
    gpu_transfer *transfer = (gpu_transfer *)(g_command + 0U);
    gpu_flush *flush = (gpu_flush *)(g_command + 64U);
    req[0] = g_command; req[1] = g_command + 64U;
    res[0] = g_response; res[1] = g_response + 32U;
    sizes[0] = sizeof(gpu_transfer); sizes[1] = sizeof(gpu_flush);
    notify_off = *(volatile uint16_t *)(g_common + 30U);
    notify = (uint32_t)notify_off * g_notify_multiplier;
    if (g_notify_len < 2U || notify > g_notify_len - 2U) return 0;
    ((volatile gpu_ctrl_header *)transfer)->flags = 0U;
    ((volatile gpu_ctrl_header *)flush)->flags = 1U;
    gpu_zero(g_response, 64U);
    for (i = 0; i < 2U; ++i) {
        head = (uint16_t)(i * 2U);
        gpu_set_desc(head, req[i], sizes[i], GPU_DESC_NEXT, (uint16_t)(head + 1U));
        gpu_set_desc((uint16_t)(head + 1U), res[i], sizeof(gpu_response), GPU_DESC_WRITE, 0);
        desc[head].flags = GPU_DESC_NEXT;
        desc[head].next = (uint16_t)(head + 1U);
        desc[head + 1U].flags = GPU_DESC_WRITE;
        desc[head + 1U].next = 0;
    }
    g_wait_used = used->index;
    available = avail->index;
    for (i = 0; i < 2U; ++i) {
        avail->ring[(available + i) % g_queue_size] = (uint16_t)(i * 2U);
    }
    cvgpu_barrier();
    avail->index = (uint16_t)(available + 2U);
    cvgpu_barrier();
    *(volatile uint16_t *)(g_notify + notify) = 0;
    cvvid_rdtsc(&g_init_start_lo, &g_init_start_hi);
    g_pending = 1;
    cvgpu_status.submits += 2U;
    cvgpu_trace(3U, cvgpu_status.submits);
    return 1;
}

static int gpu_poll_frame(void)
{
    volatile gpu_used *used = (volatile gpu_used *)(g_queue + 608U);
    uint16_t completed, i;
    if (g_isr) (void)*(volatile uint8_t *)g_isr;
    if (!g_pending) return 1;
    completed = (uint16_t)(used->index - g_wait_used);
    if (completed < 2U) {
        uint32_t ticks = gpu_deadline(1000000UL);
        if (ticks && gpu_expired(g_init_start_hi, g_init_start_lo, ticks)) {
            g_fatal = 1;
            cvgpu_status.enabled = 3U;
            cvgpu_status.error_stage = 0x1000cUL;
            return -1;
        }
        return 0;
    }
    cvgpu_barrier();
    if (completed > 2U) { g_pending = 0; g_fatal = 1; ++cvgpu_status.error_stage; return -1; }
    {
        uint8_t seen = 0;
        for (i = 0; i < 2U; ++i) {
            uint32_t id = used->ring[(g_wait_used + i) % g_queue_size].id;
            uint8_t bit;
            gpu_ctrl_header *request;
            volatile gpu_response *response;
            if (id > 2U || (id & 1U)) { g_pending = 0; g_fatal = 1; ++cvgpu_status.error_stage; return -1; }
            bit = (uint8_t)(1U << (id / 2U));
            request = (gpu_ctrl_header *)(g_command + (id / 2U) * 64U);
            response = (volatile gpu_response *)(g_response + (id / 2U) * 32U);
            if (seen & bit || used->ring[(g_wait_used + i) % g_queue_size].length < sizeof(*response) ||
                response->type != VIRTIO_GPU_RESP_OK_NODATA ||
                ((request->flags & 1U) && (!(response->flags & 1U) ||
                 response->fence_lo != request->fence_lo || response->fence_hi != request->fence_hi))) {
                g_pending = 0; g_fatal = 1; ++cvgpu_status.error_stage; return -1;
            }
            seen |= bit;
        }
    }
    g_pending = 0;
    cvgpu_status.completions += 2U;
    cvgpu_trace(4U, cvgpu_status.completions);
    return 1;
}

static uint32_t dispi_read(uint16_t index)
{
    cvgpu_out16(0x01CEU, index);
    return cvgpu_in16(0x01CFU);
}

/* Initialization is also asynchronous. The host may have to initialize its
 * GL renderer on the first command; waiting in a V86 service would hold IF=0
 * throughout that operation. Metadata and DMA backing stay owned until the
 * fenced reply, while the desktop can keep composing its first RAM frame. */
static uint32_t gpu_init_submit(void)
{
    uint32_t bytes = 0, i, pages;
    gpu_create *create = (gpu_create *)g_command;
    gpu_attach *attach = (gpu_attach *)g_command;
    gpu_transfer *transfer = (gpu_transfer *)g_command;
    gpu_scanout *scanout = (gpu_scanout *)g_command;
    gpu_flush *flush = (gpu_flush *)g_command;
    gpu_mem_entry *entry;
    gpu_zero(g_command, 64U);
    gpu_zero(g_response, 1056U);
    g_init_response_bytes = sizeof(gpu_response);
    ((gpu_ctrl_header *)g_command)->fence_lo = g_fence++;
    /* Metadata needs its used response; the final flush fence protects the
     * staging bytes through all preceding uploads in this ordered queue. */
    ((gpu_ctrl_header *)g_command)->flags = g_initializing == 5U ? 1U : 0U;
    cvgpu_status.error_stage = 6U + g_initializing;
    switch (g_initializing) {
    case 1:
        create->h.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        create->resource = g_resource;
        create->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        create->width = g_width; create->height = g_height;
        bytes = sizeof(*create);
        break;
    case 2:
        attach->h.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        attach->resource = g_resource;
        pages = (g_width * g_height * 4U + GPU_PAGE - 1U) / GPU_PAGE;
        attach->entries = pages;
        entry = (gpu_mem_entry *)(g_command + sizeof(*attach));
        for (i = 0; i < pages; ++i) {
            entry[i].address_lo = cvgpu_phys(g_staging + i * GPU_PAGE);
            entry[i].address_hi = 0;
            entry[i].length = (i + 1U == pages) ? g_width * g_height * 4U - i * GPU_PAGE : GPU_PAGE;
            entry[i].padding = 0;
        }
        bytes = sizeof(*attach) + pages * sizeof(*entry);
        break;
    case 3:
        transfer->h.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        transfer->width = g_width; transfer->height = g_height;
        transfer->resource = g_resource;
        bytes = sizeof(*transfer);
        break;
    case 4:
        scanout->h.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        scanout->width = g_width; scanout->height = g_height;
        scanout->resource = g_resource;
        bytes = sizeof(*scanout);
        break;
    case 5:
        flush->h.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        flush->width = g_width; flush->height = g_height;
        flush->resource = g_resource;
        bytes = sizeof(*flush);
        break;
    case 6:
        ((gpu_ctrl_header *)g_command)->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
        bytes = sizeof(gpu_ctrl_header);
        g_init_response_bytes = 408U;
        break;
    case 7:
        ((gpu_ctrl_header *)g_command)->type = VIRTIO_GPU_CMD_GET_EDID;
        bytes = sizeof(gpu_ctrl_header) + 8U; /* scanout 0, reserved 0 */
        g_init_response_bytes = 1056U;
        break;
    default: return 0;
    }
    return bytes <= 0x11000UL && gpu_submit(g_command, bytes, g_response,
                                           g_init_response_bytes, 0);
}

static int gpu_init_poll(void)
{
    volatile gpu_used *used = (volatile gpu_used *)(g_queue + 608U);
    volatile gpu_response *response = (volatile gpu_response *)g_response;
    gpu_ctrl_header *request = (gpu_ctrl_header *)g_command;
    uint32_t ticks;
    if (used->index == g_wait_used) {
        ticks = gpu_deadline(1000000UL);
        if (ticks && gpu_expired(g_init_start_hi, g_init_start_lo, ticks)) {
            cvgpu_status.error_stage |= 0x10000UL;
            g_fatal = 1;
            cvgpu_status.enabled = 3U;
            return -1;
        }
        return 0;
    }
    cvgpu_barrier();
    if ((uint16_t)(used->index - g_wait_used) != 1U ||
        used->ring[g_wait_used % g_queue_size].id != 0 ||
        used->ring[g_wait_used % g_queue_size].length < sizeof(*response) ||
        (g_initializing <= 5U && response->type != VIRTIO_GPU_RESP_OK_NODATA) ||
        ((request->flags & 1U) && (!(response->flags & 1U) ||
         response->fence_lo != request->fence_lo || response->fence_hi != request->fence_hi))) {
        g_fatal = 1;
        cvgpu_status.enabled = 3U;
        cvgpu_status.error_stage |= 0x20000UL;
        return -1;
    }
    /* Display metadata is optional. An error response leaves identification
     * unavailable, while a valid completed reply can safely release the slot. */
    if (g_initializing == 7U && response->type == VIRTIO_GPU_RESP_OK_EDID &&
        used->ring[g_wait_used % g_queue_size].length >= 160U) {
        uint32_t size = *(volatile uint32_t *)(g_response + 24U);
        if (size >= 128U && size <= 1024U &&
            used->ring[g_wait_used % g_queue_size].length >= 32U + size) {
            uint32_t i;
            for (i = 0; i < 128U; ++i) cvgpu_edid[i] = *(volatile uint8_t *)(g_response + 32U + i);
            cvgpu_edid_bytes = 128U;
        }
    }
    ++g_initializing;
    if (g_initializing <= 5U || ((g_features & 2U) && g_initializing <= 7U)) {
        if (gpu_init_submit()) return 0;
        g_fatal = 1;
        cvgpu_status.enabled = 3U;
        return -1;
    }
    g_initializing = 0;
    g_dirty = 1;
    cvgpu_status.enabled = 1;
    cvgpu_status.error_stage = 0;
    return 1;
}

uint32_t cvgpu_bind(uint32_t framebuffer_bytes)
{
    uint8_t bus, dev, fn, status;
    uint16_t dispi_index;
    uint32_t features1, rows, reset_spins;
    if (g_bound) return g_framebuffer;
    if (cvgpu_owned()) return 0;
    g_initializing = 0;
    g_staging_valid = 0;
    cvgpu_edid_bytes = 0; g_features = 0;
    g_pending = g_fatal = g_dirty = 0; g_wait_used = 0; g_last_y = 0; g_fence = 0;
    cvgpu_status.enabled = 0U; cvgpu_status.error_stage = 1U;
    if (!framebuffer_bytes || framebuffer_bytes > GPU_MAX_BYTES) return 0;
    if (!pci_find_gpu(&bus, &dev, &fn)) return 0;
    g_pci_bus = bus; g_pci_dev = dev; g_pci_fn = fn;
    g_pci_command = (uint16_t)pci_read(bus, dev, fn, 4);
    g_pci_saved = 1;
    cvgpu_status.error_stage = 2U;
    if (!gpu_find_caps(bus, dev, fn)) goto fail;
    cvgpu_status.error_stage = 3U;
    dispi_index = cvgpu_in16(0x01CEU);
    g_width = dispi_read(1U); g_height = dispi_read(2U);
    g_virtual_width = dispi_read(6U);
    status = (uint8_t)dispi_read(3U);
    cvgpu_out16(0x01CEU, dispi_index);
    if (status != 32U || g_width == 0 || g_height == 0 || g_virtual_width < g_width) goto fail;
    if (g_virtual_width > 4096U || g_height > 4096U) goto fail;
    if (g_virtual_width > 0xFFFFFFFFUL / 4U) goto fail;
    g_pitch = g_virtual_width * 4U;
    if (framebuffer_bytes % g_pitch) goto fail;
    rows = framebuffer_bytes / g_pitch;
    if (rows < g_height || rows > 4096U) goto fail;
    g_virtual_height = rows;
    g_framebuffer_bytes = framebuffer_bytes;
    cvgpu_status.error_stage = 4U;
    g_framebuffer = cvgpu_alloc(g_framebuffer_bytes);
    if (!g_framebuffer) goto fail;
    cvgpu_status.error_stage = 5U;
    *gpu_common8(20U) = 0;
    reset_spins = 0;
    while (*gpu_common8(20U) != 0 && reset_spins++ < 1000000UL) { }
    if (*gpu_common8(20U) != 0) goto fail;
    *gpu_common8(20U) = GPU_STATUS_ACK;
    *gpu_common8(20U) = GPU_STATUS_ACK | GPU_STATUS_DRIVER;
    *(volatile uint32_t *)(g_common + 0U) = 0;
    g_features = *(volatile uint32_t *)(g_common + 4U) & 2U; /* optional EDID */
    *(volatile uint32_t *)(g_common + 0U) = 1;
    features1 = *(volatile uint32_t *)(g_common + 4U);
    if (!(features1 & 1U)) goto fail;
    *(volatile uint32_t *)(g_common + 8U) = 0;
    *(volatile uint32_t *)(g_common + 12U) = g_features;
    *(volatile uint32_t *)(g_common + 8U) = 1;
    *(volatile uint32_t *)(g_common + 12U) = 1U; /* VIRTIO_F_VERSION_1 */
    *gpu_common8(20U) = GPU_STATUS_ACK | GPU_STATUS_DRIVER | 8U;
    cvgpu_status.error_stage = 6U;
    if (!(*gpu_common8(20U) & 8U) || !gpu_queue_init()) goto fail;
    *gpu_common8(20U) = GPU_STATUS_ACK | GPU_STATUS_DRIVER | 8U | GPU_STATUS_DRIVER_OK;
    if (!(*gpu_common8(20U) & GPU_STATUS_DRIVER_OK)) goto fail;
    g_command = cvgpu_alloc(0x11000UL);
    g_response = cvgpu_alloc(GPU_PAGE);
    if (!g_command || !g_response) goto fail;
    g_staging = cvgpu_alloc(g_width * g_height * 4U);
    if (!g_staging) goto fail;
    g_resource = 1U; g_fence = 1U;
    g_initializing = 1;
    if (!gpu_init_submit()) goto fail;
    g_bound = 1;
    g_dirty = 1;
    cvgpu_status.enabled = 2U; cvgpu_status.width = g_width;
    cvgpu_status.height = g_height; cvgpu_status.pitch = g_pitch;
    cvgpu_status.device_status = *gpu_common8(20U);
    return g_framebuffer;
fail:
    /* Do not release DMA memory until the device has acknowledged reset. */
    cvgpu_status.enabled = 0U;
    if (g_common) {
        *gpu_common8(20U) = 0;
        if (*gpu_common8(20U) != 0) return 0;
    }
    if (g_queue) cvgpu_free(g_queue);
    if (g_command) cvgpu_free(g_command);
    if (g_response) cvgpu_free(g_response);
    if (g_staging) cvgpu_free(g_staging);
    if (g_framebuffer) cvgpu_free(g_framebuffer);
    g_queue = g_command = g_response = g_framebuffer = g_staging = 0;
    gpu_unmap_all();
    if (g_pci_saved) {
        uint32_t address = 0x80000000UL | ((uint32_t)g_pci_bus << 16) |
                           ((uint32_t)g_pci_dev << 11) | ((uint32_t)g_pci_fn << 8) | 4U;
        cvgpu_out32(PCI_ADDR, address); cvgpu_out16(PCI_DATA, g_pci_command);
        g_pci_saved = 0;
    }
    (void)status;
    return 0;
}

uint32_t cvgpu_present(uint32_t yoffset)
{
    gpu_transfer *transfer;
    gpu_flush *flush;
    uint32_t source_offset, row, left, top, right, bottom;
    int poll;
    poll = gpu_poll_frame();
    if (poll == 0) return 0;
    if (poll < 0) return 2;
    if (g_fatal) return 2;
    if (g_initializing) {
        poll = gpu_init_poll();
        if (poll <= 0) return poll < 0 ? 2U : 0U;
    }
    if (!g_bound || yoffset > g_virtual_height || g_height > g_virtual_height - yoffset) return 1;
    if (yoffset && g_pitch > 0xFFFFFFFFUL / yoffset) return 1;
    source_offset = yoffset * g_pitch;
    if (source_offset > g_framebuffer_bytes || g_height * g_pitch > g_framebuffer_bytes - source_offset) return 1;
    if (yoffset != g_last_y) g_dirty = 1;
    g_last_y = yoffset;
    if (!g_dirty) return 0;
    left = g_width; top = g_height; right = bottom = 0;
    for (row = 0; row < g_height; ++row) {
        uint32_t first = 0, last = g_width;
        uint32_t *dest = (uint32_t *)(g_staging + row * g_width * 4U);
        const uint32_t *src = (const uint32_t *)(g_framebuffer + source_offset + row * g_pitch);
        if (g_staging_valid) {
            while (first < last && src[first] == dest[first]) ++first;
            while (last > first && src[last - 1U] == dest[last - 1U]) --last;
        }
        if (first == last) continue;
        gpu_copy((uint32_t)(dest + first), (uint32_t)(src + first), (last - first) * 4U);
        if (first < left) left = first;
        if (last > right) right = last;
        if (row < top) top = row;
        bottom = row + 1U;
    }
    g_dirty = 0;
    if (top == g_height) { ++cvgpu_status.skipped; return 0; }
    /* ATTACH_BACKING occupied these slots during initialization. Never reuse
     * those page addresses as reserved/context fields in a frame header. */
    gpu_zero(g_command, 128U);
    transfer = (gpu_transfer *)(g_command + 0U);
    transfer->h.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D; transfer->h.fence_lo = g_fence++;
    transfer->x = left; transfer->y = top;
    transfer->width = right - left; transfer->height = bottom - top;
    transfer->offset_lo = (top * g_width + left) * 4U;
    transfer->offset_hi = 0; transfer->resource = g_resource; transfer->padding = 0;
    flush = (gpu_flush *)(g_command + 64U);
    flush->h.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH; flush->h.fence_lo = g_fence++;
    flush->x = left; flush->y = top;
    flush->width = right - left; flush->height = bottom - top;
    flush->resource = g_resource; flush->padding = 0;
    if (!gpu_submit_frame()) { g_staging_valid = 0; g_dirty = 1; cvgpu_status.error_stage = 5U; return 2; }
    g_staging_valid = 1;
    return 0;
}

void cvgpu_damage(uint32_t offset, uint32_t bytes)
{
    if (g_bound && bytes && offset < g_framebuffer_bytes && bytes <= g_framebuffer_bytes - offset)
        g_dirty = 1;
}

uint32_t cvgpu_owned(void)
{
    return g_queue || g_command || g_response || g_framebuffer || g_staging ||
           g_common || g_notify || g_isr || g_device || g_pci_saved;
}

uint32_t cvgpu_release(void)
{
    volatile uint8_t *status;
    uint32_t spins;
    if (!g_common && !cvgpu_owned()) return 0;
    if (g_common && g_bound && !g_pending && !g_initializing && !g_fatal) {
        gpu_scanout *scanout = (gpu_scanout *)(g_command + 64U);
        scanout->h.type = VIRTIO_GPU_CMD_SET_SCANOUT; scanout->h.fence_lo = g_fence++;
        scanout->h.flags = 0;
        scanout->x = 0; scanout->y = 0; scanout->width = 0; scanout->height = 0;
        scanout->scanout = 0; scanout->resource = 0;
        (void)gpu_command((uint32_t)scanout, sizeof(gpu_scanout), GPU_INIT_TIMEOUT);
    }
    if (g_common) {
        status = (uint8_t *)g_common + 20U;
        *status = 0;
        spins = 0;
        while (*status != 0 && spins++ < 1000000UL) { }
        if (*status != 0) return 1;
    }
    if (g_queue) cvgpu_free(g_queue);
    if (g_command) cvgpu_free(g_command);
    if (g_response) cvgpu_free(g_response);
    if (g_staging) cvgpu_free(g_staging);
    if (g_framebuffer) cvgpu_free(g_framebuffer);
    g_bound = 0;
    g_queue = g_command = g_response = g_framebuffer = g_staging = 0;
    g_initializing = 0;
    cvgpu_edid_bytes = 0; g_features = 0;
    g_pending = g_fatal = g_dirty = 0; g_wait_used = 0; g_last_y = 0; g_fence = 0;
    gpu_unmap_all();
    if (g_pci_saved) {
        uint32_t address = 0x80000000UL | ((uint32_t)g_pci_bus << 16) |
                           ((uint32_t)g_pci_dev << 11) | ((uint32_t)g_pci_fn << 8) | 4U;
        cvgpu_out32(PCI_ADDR, address); cvgpu_out16(PCI_DATA, g_pci_command);
        g_pci_saved = 0;
    }
    cvgpu_status.enabled = 0U; cvgpu_status.device_status = 0U;
    cvgpu_status.width = cvgpu_status.height = cvgpu_status.pitch = 0;
    cvgpu_status.submits = cvgpu_status.completions = cvgpu_status.skipped = 0;
    cvgpu_status.error_stage = 0;
    return 0;
}
