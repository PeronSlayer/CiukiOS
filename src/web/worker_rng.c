#include "worker_rng.h"

#include <dpmi.h>
#include <dos.h>
#include <pc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/segments.h>

#define WRNG_PCI_ADDR       0x0CF8
#define WRNG_PCI_DATA       0x0CFC
#define WRNG_PCI_VENDOR     0x1AF4u
#define WRNG_PCI_DEVICE     0x1005u
#define WRNG_VIRTIO_QUEUE   0u
#define WRNG_DMA_BYTES      65536u
#define WRNG_DMA_BUFFER     16384u
#define WRNG_MAX_QUEUE      256u
#define WRNG_WAIT_SECONDS   5u
#define WRNG_EMPTY_RETRIES  16u

#define VIRTIO_STATUS_ACK       1u
#define VIRTIO_STATUS_DRIVER    2u
#define VIRTIO_STATUS_DRIVEROK  4u
#define VIRTQ_DESC_F_WRITE      2u
#define VIRTQ_AVAIL_F_NO_IRQ    1u

struct wrng_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct wrng_used_element {
    uint32_t id;
    uint32_t length;
} __attribute__((packed));

static __dpmi_meminfo wrng_mapping;
static uint32_t wrng_physical;
static uint32_t wrng_dma_bytes;
static uint16_t wrng_port;
static uint16_t wrng_queue_size;
static uint16_t wrng_available_index;
static uint16_t wrng_used_index;
static uint32_t wrng_available_offset;
static uint32_t wrng_used_offset;
static uint32_t wrng_buffer_offset;
static int wrng_started;
static int wrng_configured;
static int wrng_mapped;
static int wrng_selector = -1;
static int wrng_error_stage;
static int wrng_pci_device = -1;
static uint16_t wrng_pci_command;

static uint32_t align_page(uint32_t value);

static uint32_t pci_address(unsigned device, unsigned function, unsigned offset)
{
    return 0x80000000UL | (device << 11) | (function << 8) | (offset & 0xFCu);
}

static uint32_t pci_read32(unsigned device, unsigned function, unsigned offset)
{
    outportl(WRNG_PCI_ADDR, pci_address(device, function, offset));
    return inportl(WRNG_PCI_DATA);
}

static void pci_write16(unsigned device, unsigned function, unsigned offset,
    uint16_t value)
{
    outportl(WRNG_PCI_ADDR, pci_address(device, function, offset));
    outportw(WRNG_PCI_DATA + (offset & 2u), value);
}

static int pci_find_rng(uint32_t *bar0)
{
    unsigned device;

    for (device = 0; device < 32u; device++) {
        uint32_t id = pci_read32(device, 0, 0);
        if ((id & 0xFFFFu) == WRNG_PCI_VENDOR &&
            (id >> 16) == WRNG_PCI_DEVICE) {
            *bar0 = pci_read32(device, 0, 0x10);
            return (int)device;
        }
    }
    return -1;
}

static uint32_t align_page(uint32_t value)
{
    return (value + 4095u) & ~4095u;
}

static void wrng_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

static int wrng_map(uint32_t physical_base)
{
    uint32_t map_base = physical_base & ~4095u;
    uint32_t queue_base = align_page(physical_base);
    uint32_t queue_offset = queue_base - map_base;
    if (queue_offset >= WRNG_DMA_BYTES) {
        wrng_error_stage = 5;
        return -1;
    }
    memset(&wrng_mapping, 0, sizeof wrng_mapping);
    wrng_mapping.address = map_base;
    wrng_mapping.size = WRNG_DMA_BYTES;
    wrng_error_stage = 5;
    if (__dpmi_physical_address_mapping(&wrng_mapping) != 0) return -1;
    wrng_mapped = 1;
    wrng_error_stage = 6;
    wrng_selector = __dpmi_allocate_ldt_descriptors(1);
    if (wrng_selector < 0) return -1;
    wrng_error_stage = 7;
    wrng_dma_bytes = WRNG_DMA_BYTES - queue_offset;
    if (__dpmi_set_segment_base_address(wrng_selector,
            wrng_mapping.address + queue_offset) != 0 ||
        __dpmi_set_segment_limit(wrng_selector, wrng_dma_bytes - 1u) != 0)
        return -1;
    wrng_physical = queue_base;
    return 0;
}

static void wrng_copy_to(unsigned offset, const void *source, unsigned bytes)
{
    movedata(_my_ds(), (unsigned)source, wrng_selector, offset, bytes);
}

static void wrng_copy_from(void *destination, unsigned offset, unsigned bytes)
{
    movedata(wrng_selector, offset, _my_ds(), (unsigned)destination, bytes);
}

static void wrng_zero_mapping(void)
{
    unsigned char zero[256];
    unsigned offset;
    memset(zero, 0, sizeof zero);
    for (offset = 0; offset < wrng_dma_bytes; offset += sizeof zero)
        wrng_copy_to(offset, zero, sizeof zero);
}

static void wrng_unmap(void)
{
    if (wrng_selector >= 0) {
        __dpmi_free_ldt_descriptor(wrng_selector);
        wrng_selector = -1;
    }
    if (wrng_mapped) {
        __dpmi_free_physical_address_mapping(&wrng_mapping);
        wrng_mapped = 0;
    }
    wrng_physical = 0;
    wrng_dma_bytes = 0;
}

static int wrng_layout(void)
{
    uint32_t descriptor_bytes;
    uint32_t available_bytes;
    uint32_t used_bytes;

    if (wrng_queue_size == 0 || wrng_queue_size > WRNG_MAX_QUEUE) return -1;
    descriptor_bytes = (uint32_t)wrng_queue_size * 16u;
    available_bytes = 4u + (uint32_t)wrng_queue_size * 2u;
    used_bytes = 4u + (uint32_t)wrng_queue_size * 8u;
    wrng_available_offset = descriptor_bytes;
    wrng_used_offset = align_page(wrng_available_offset + available_bytes);
    wrng_buffer_offset = align_page(wrng_used_offset + used_bytes);
    if (wrng_buffer_offset > wrng_dma_bytes ||
        WRNG_DMA_BUFFER > wrng_dma_bytes - wrng_buffer_offset) return -1;
    return 0;
}

static void wrng_publish_request(void)
{
    struct wrng_descriptor descriptor;
    uint16_t ring_entry = 0;
    uint16_t index;

    descriptor.address = (uint64_t)(wrng_physical + wrng_buffer_offset);
    descriptor.length = WRNG_DMA_BUFFER;
    descriptor.flags = VIRTQ_DESC_F_WRITE;
    descriptor.next = 0;
    wrng_copy_to(0, &descriptor, sizeof descriptor);
    wrng_copy_to(wrng_available_offset + 4u +
        (uint32_t)(wrng_available_index % wrng_queue_size) * 2u,
        &ring_entry, sizeof ring_entry);
    wrng_barrier();
    wrng_available_index++;
    index = wrng_available_index;
    wrng_copy_to(wrng_available_offset + 2u, &index, sizeof index);
    wrng_barrier();
    outportw(wrng_port + 0x10u, WRNG_VIRTIO_QUEUE);
}

static int wrng_timed_out(clock_t start, unsigned long *fallback)
{
    clock_t now;

    if (++*fallback >= 5000UL) return 1;
    if (start != (clock_t)-1) {
        now = clock();
        if (now != (clock_t)-1 &&
            now - start >= (clock_t)(WRNG_WAIT_SECONDS * CLOCKS_PER_SEC))
            return 1;
    }
    return 0;
}

static int wrng_wait_used(struct wrng_used_element *element)
{
    uint16_t used_index;
    uint32_t slot;
    clock_t start = clock();
    unsigned long fallback = 0;
    unsigned short retries = 0;

    for (;;) {
        wrng_copy_from(&used_index, wrng_used_offset + 2u, sizeof used_index);
        while (used_index == wrng_used_index) {
            if (wrng_timed_out(start, &fallback)) return -1;
            delay(1);
            wrng_copy_from(&used_index, wrng_used_offset + 2u,
                sizeof used_index);
        }
        wrng_barrier();
        slot = wrng_used_offset + 4u +
            (uint32_t)(wrng_used_index % wrng_queue_size) * 8u;
        wrng_copy_from(element, slot, sizeof *element);
        wrng_used_index = used_index;
        (void)inportb(wrng_port + 0x13u); /* read-to-clear legacy ISR */
        if (element->id != 0 || element->length > WRNG_DMA_BUFFER) return -1;
        if (element->length != 0) return 0;
        if (retries++ >= WRNG_EMPTY_RETRIES ||
            wrng_timed_out(start, &fallback)) return -1;
        wrng_publish_request();
    }
}

int wrng_init(uint32_t physical_dma_base)
{
    uint32_t bar0, command_status;
    uint32_t device_features;
    uint16_t command;
    uint16_t queue_size;
    int device;

    wrng_shutdown();
    wrng_error_stage = 1;
    device = pci_find_rng(&bar0);
    if (device < 0) return -1;
    wrng_error_stage = 2;
    if (bar0 == 0xFFFFFFFFUL || (bar0 & 1u) == 0) return -1;
    if ((bar0 & 0xFFFFFFFCUL) == 0 || (bar0 & 0xFFFFFFFCUL) > 0xFFEBu)
        return -1;
    wrng_port = (uint16_t)(bar0 & 0xFFFCu);

    command_status = pci_read32((unsigned)device, 0, 4);
    command = (uint16_t)command_status;
    wrng_pci_device = device;
    wrng_pci_command = command;
    /* This queue is polled. NO_INTERRUPT alone is advisory; disable this
       PCI function's INTx without masking any shared PIC line. */
    command |= 0x0405u;
    pci_write16((unsigned)device, 0, 4, command);
    wrng_error_stage = 3;
    device_features = inportl(wrng_port);
    if (device_features == 0xFFFFFFFFUL) goto fail;

    outportb(wrng_port + 0x12u, 0); /* reset */
    outportb(wrng_port + 0x12u, VIRTIO_STATUS_ACK);
    outportb(wrng_port + 0x12u, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    wrng_configured = 1;
    outportl(wrng_port + 4u, 0); /* negotiate no optional features */
    wrng_error_stage = 4;
    outportw(wrng_port + 0x0Eu, WRNG_VIRTIO_QUEUE);
    queue_size = inportw(wrng_port + 0x0Cu);
    if (queue_size == 0 || queue_size > WRNG_MAX_QUEUE) goto fail;
    wrng_queue_size = queue_size;
    if (wrng_map(physical_dma_base) != 0) goto fail;
    wrng_error_stage = 8;
    if (wrng_layout() != 0) goto fail;

    wrng_zero_mapping();
    {
        uint16_t flags = VIRTQ_AVAIL_F_NO_IRQ;
        wrng_copy_to(wrng_available_offset, &flags, sizeof flags);
    }
    wrng_available_index = 0;
    wrng_used_index = 0;
    outportl(wrng_port + 8u, wrng_physical >> 12);
    outportb(wrng_port + 0x12u,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVEROK);
    wrng_error_stage = 9;
    if ((inportb(wrng_port + 0x12u) & 7u) != 7u) goto fail;
    wrng_started = 1;
    wrng_error_stage = 0;
    wrng_publish_request();
    return 0;

fail:
    wrng_shutdown();
    return -1;
}

int wrng_last_error(void)
{
    return wrng_error_stage;
}

int wrng_read(void *output, uint32_t bytes)
{
    unsigned char *dst = (unsigned char *)output;
    struct wrng_used_element element;
    uint32_t copied = 0;

    if (!wrng_started || (bytes != 0 && dst == NULL) || bytes > 65536UL)
        return -1;
    while (copied < bytes) {
        uint32_t amount;
        if (wrng_wait_used(&element) != 0) {
            wrng_shutdown();
            return -1;
        }
        amount = element.length;
        if (amount > bytes - copied) amount = bytes - copied;
        wrng_copy_from(dst + copied, wrng_buffer_offset, amount);
        copied += amount;
        wrng_publish_request();
    }
    return 0;
}

void wrng_shutdown(void)
{
    if (wrng_configured && wrng_port != 0) {
        outportl(wrng_port + 8u, 0);
        outportb(wrng_port + 0x12u, 0);
        (void)inportb(wrng_port + 0x13u);
    }
    if (wrng_pci_device >= 0)
        pci_write16((unsigned)wrng_pci_device, 0, 4, wrng_pci_command);
    wrng_pci_device = -1;
    wrng_started = 0;
    wrng_configured = 0;
    wrng_port = 0;
    wrng_queue_size = 0;
    wrng_available_index = 0;
    wrng_used_index = 0;
    wrng_available_offset = 0;
    wrng_used_offset = 0;
    wrng_buffer_offset = 0;
    wrng_unmap();
}
