/* Host-only VirtIO GPU wire/queue regression fixture. Do not run in DOS. */
#define _GNU_SOURCE
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/mman.h>

extern "C" {
#include "session_gpu.c"
}

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, __FILE__, __LINE__, #x); \
    std::exit(1); } } while (0)

static uint32_t pci_addr;
static uint8_t dispi_reg;
static uint32_t mmio_base;
static uint32_t device_common;
static uint32_t mock_pci[64];
static uint32_t original_command = 1U;
static std::map<uint32_t, uint32_t> allocations;
static std::atomic<bool> device_running(false);
static std::atomic<bool> device_active(false);
static std::thread device_thread;
static std::mutex command_lock;
static std::vector<uint32_t> command_types;
static std::atomic<uint16_t> device_consumed(0);
static std::atomic<unsigned> transfers_seen(0);
static std::atomic<uint32_t> expected_pixel(0);
static uint32_t protocol_errors;
static uint8_t mock_edid[128];

static uint32_t low_map(uint32_t bytes)
{
    size_t size = (bytes + 4095U) & ~4095U;
    void *p = mmap(0, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return 0;
    if ((uintptr_t)p > 0xFFFFFFFFUL) { munmap(p, size); return 0; }
    return (uint32_t)(uintptr_t)p;
}

extern "C" uint32_t cvdev_in(uint32_t port, uint32_t size)
{
    if (port == 0xCFCU && size == 4U) {
        uint8_t reg = (uint8_t)(pci_addr & 0xFCU);
        if (reg == 4U) return (mock_pci[1] & 0xFFFF0000UL) | (uint32_t)original_command;
        return mock_pci[reg / 4U];
    }
    if (port == 0x01CEU && size == 2U) return dispi_reg;
    if (port == 0x01CFU && size == 2U) {
        if (dispi_reg == 1U) return 64U;
        if (dispi_reg == 2U) return 48U;
        if (dispi_reg == 3U) return 32U;
        if (dispi_reg == 6U) return 64U;
    }
    return 0;
}

extern "C" void cvdev_out(uint32_t port, uint32_t value, uint32_t size)
{
    if (port == 0xCF8U && size == 4U) pci_addr = value;
    else if (port == 0xCFCU && size == 2U && (pci_addr & 0xFCU) == 4U)
        original_command = (uint16_t)value;
    else if (port == 0x01CEU && size == 2U) dispi_reg = (uint8_t)value;
}

extern "C" uint32_t cvgpu_clock_khz(void) { return 3000000UL; }
extern "C" void cvgpu_barrier(void) { std::atomic_thread_fence(std::memory_order_seq_cst); }
extern "C" void cvvid_rdtsc(uint32_t *low, uint32_t *high)
{
    uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t ticks = ns * 3U;
    *low = (uint32_t)ticks; *high = (uint32_t)(ticks >> 32);
}
extern "C" uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes)
{
    if (physical < 0xF0000000UL || physical - 0xF0000000UL > 0x10000UL - bytes) return 0;
    return mmio_base + physical - 0xF0000000UL;
}
extern "C" void cvgpu_unmap_mmio(uint32_t linear) { (void)linear; }
extern "C" uint32_t cvgpu_alloc(uint32_t bytes)
{
    uint32_t p = low_map(bytes);
    if (p) allocations[p] = (bytes + 4095U) & ~4095U;
    return p;
}
extern "C" void cvgpu_free(uint32_t linear)
{
    std::map<uint32_t, uint32_t>::iterator it = allocations.find(linear);
    if (it != allocations.end()) {
        munmap((void *)(uintptr_t)it->first, it->second);
        allocations.erase(it);
    }
}
extern "C" uint32_t cvgpu_phys(uint32_t linear) { return linear; }

static void pci_set(uint8_t reg, uint32_t value) { mock_pci[reg / 4U] = value; }

static void configure_mock_pci(void)
{
    static const uint8_t caps[] = { 0x40, 0x50, 0x64, 0x74 };
    static const uint8_t next[] = { 0x50, 0x64, 0x74, 0x00 };
    static const uint8_t types[] = { 1, 2, 3, 4 };
    static const uint32_t offsets[] = { 0x0000, 0x1000, 0x2000, 0x3000 };
    static const uint32_t lengths[] = { 0x100, 0x100, 0x100, 0x100 };
    unsigned i;
    pci_set(0x00, (VIRTIO_GPU_DEVICE << 16) | VIRTIO_VENDOR);
    pci_set(0x04, 0x00100000UL | original_command);
    pci_set(0x0C, 0);
    pci_set(0x10, 0xF0000000UL);
    pci_set(0x34, 0x40U);
    for (i = 0; i < 4; ++i) {
        uint8_t c = caps[i];
        uint32_t cap = PCI_CAP_VENDOR | ((uint32_t)next[i] << 8) |
                       ((uint32_t)(i == 1 ? 20 : 16) << 16) | ((uint32_t)types[i] << 24);
        pci_set(c, cap);
        pci_set((uint8_t)(c + 4), 0U);
        pci_set((uint8_t)(c + 8), offsets[i]);
        pci_set((uint8_t)(c + 12), lengths[i]);
        if (i == 1) pci_set((uint8_t)(c + 16), 4U);
    }
}

static void device_complete_one(uint16_t avail_slot)
{
    volatile gpu_avail *avail = (volatile gpu_avail *)(g_queue + 512U);
    volatile gpu_used *used = (volatile gpu_used *)(g_queue + 608U);
    volatile gpu_desc *desc = (volatile gpu_desc *)g_queue;
    uint16_t head = avail->ring[avail_slot % g_queue_size], at = head;
    uint32_t steps = 0, request_addr, response_addr = 0, type;
    gpu_ctrl_header *request;
    gpu_response *response;
    for (;;) {
        if (++steps > GPU_QUEUE_SIZE) { ++protocol_errors; return; }
        if (desc[at].address_hi != 0 || desc[at].address_lo == 0) { ++protocol_errors; return; }
        if (at == head) request_addr = desc[at].address_lo;
        if (desc[at].flags & GPU_DESC_WRITE) response_addr = desc[at].address_lo;
        if (!(desc[at].flags & GPU_DESC_NEXT)) break;
        at = desc[at].next;
    }
    if (!response_addr) { ++protocol_errors; return; }
    request = (gpu_ctrl_header *)(uintptr_t)request_addr;
    response = (gpu_response *)(uintptr_t)response_addr;
    type = request->type;
    if (type != VIRTIO_GPU_CMD_RESOURCE_CREATE_2D &&
        type != VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING &&
        type != VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D &&
        type != VIRTIO_GPU_CMD_SET_SCANOUT &&
        type != VIRTIO_GPU_CMD_RESOURCE_FLUSH &&
        type != VIRTIO_GPU_CMD_GET_DISPLAY_INFO &&
        type != VIRTIO_GPU_CMD_GET_EDID) ++protocol_errors;
    if ((request->flags & 1U) != (request->type == VIRTIO_GPU_CMD_RESOURCE_FLUSH ? 1U : 0U)) ++protocol_errors;
    if (request->context != 0U || request->padding != 0U) ++protocol_errors;
    if (type == VIRTIO_GPU_CMD_RESOURCE_CREATE_2D) {
        gpu_create *create = (gpu_create *)request;
        if (create->resource != 1U || create->format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM ||
            create->width != 64U || create->height != 48U) ++protocol_errors;
    } else if (type == VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING) {
        gpu_attach *attach = (gpu_attach *)request;
        gpu_mem_entry *entry = (gpu_mem_entry *)((uint8_t *)request + sizeof(gpu_attach));
        uint32_t i;
        if (attach->resource != 1U || attach->entries != 3U) ++protocol_errors;
        for (i = 0; i < attach->entries && i < 3U; ++i)
            if ((entry[i].address_lo & 4095U) || entry[i].address_hi ||
                entry[i].length != 4096U || entry[i].padding) ++protocol_errors;
    }
    if (type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D && transfers_seen.fetch_add(1U) != 0U &&
        *(volatile uint32_t *)g_staging != expected_pixel.load()) ++protocol_errors;
    {
        std::lock_guard<std::mutex> guard(command_lock);
        command_types.push_back(type);
    }
    std::memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;
    response->flags = request->flags;
    response->fence_lo = request->fence_lo;
    response->fence_hi = request->fence_hi;
    uint32_t response_bytes = sizeof(*response);
    if (type == VIRTIO_GPU_CMD_GET_DISPLAY_INFO) {
        std::memset(response, 0, 408U);
        response->type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
        response->flags = request->flags;
        response_bytes = 408U;
    } else if (type == VIRTIO_GPU_CMD_GET_EDID) {
        std::memset(response, 0, 160U);
        response->type = VIRTIO_GPU_RESP_OK_EDID;
        response->flags = request->flags;
        *(uint32_t *)((uint8_t *)response + 24U) = 128U;
        std::memcpy((uint8_t *)response + 32U, mock_edid, sizeof(mock_edid));
        response_bytes = 160U;
    }
    if (response->context != 0U || response->padding != 0U) ++protocol_errors;
    used->ring[used->index % g_queue_size].id = head;
    used->ring[used->index % g_queue_size].length = response_bytes;
    ++used->index;
}

static void device_pump(void)
{
    volatile gpu_avail *avail = (volatile gpu_avail *)(g_queue + 512U);
    uint16_t consumed = device_consumed.load();
    uint16_t pending = (uint16_t)(avail->index - consumed);
    /* Reverse-complete a transfer/flush frame batch to exercise id mapping. */
    if (pending == 2U) {
        device_complete_one((uint16_t)(consumed + 1U));
        device_complete_one(consumed);
        device_consumed.store((uint16_t)(consumed + 2U));
    } else {
        if (device_consumed.load() != avail->index) {
            consumed = device_consumed.load();
            device_complete_one(consumed);
            device_consumed.store((uint16_t)(consumed + 1U));
        }
    }
}

static void mock_device_loop(void)
{
    while (device_running.load()) {
        if (!device_active.load()) {
            if (g_common && (*(volatile uint8_t *)(g_common + 20U) & GPU_STATUS_DRIVER_OK)) {
                device_common = g_common;
                device_active.store(true);
            } else { std::this_thread::yield(); continue; }
        }
        /* Reset is written only after any synchronous in-flight response. Stop
           touching queue memory before the driver releases its DMA allocation. */
        if (!*(volatile uint8_t *)(device_common + 20U)) {
            device_running.store(false);
            break;
        }
        if (g_queue && ((volatile gpu_avail *)(g_queue + 512U))->index != device_consumed.load())
            device_pump();
        else std::this_thread::yield();
    }
}

static void test_gpu_protocol(void)
{
    uint32_t fb;
    mmio_base = low_map(0x10000U);
    CHECK(mmio_base != 0);
    configure_mock_pci();
    /* Common config: VERSION_1, queue 0 with 32 entries, notify offset 0. */
    *(volatile uint32_t *)(mmio_base + 4U) = 1U;
    *(volatile uint32_t *)(mmio_base + 8U) = 1;
    *(volatile uint32_t *)(mmio_base + 12U) = 1;
    *(volatile uint16_t *)(mmio_base + 24U) = GPU_QUEUE_SIZE;
    *(volatile uint16_t *)(mmio_base + 30U) = 0;
    std::memset(mock_edid, 0, sizeof(mock_edid));
    const uint8_t edid_header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    std::memcpy(mock_edid, edid_header, sizeof(edid_header));
    mock_edid[8] = 0x4c; mock_edid[9] = 0x2d;
    mock_edid[18] = 1; mock_edid[19] = 4;
    uint8_t edid_sum = 0;
    for (unsigned i = 0; i < 127U; ++i) edid_sum = (uint8_t)(edid_sum + mock_edid[i]);
    mock_edid[127] = (uint8_t)(0U - edid_sum);
    device_running.store(true);
    device_thread = std::thread(mock_device_loop);

    fb = cvgpu_bind(64U * 48U * 4U);
    CHECK(fb != 0);
    /* Exercise optional metadata handling without changing the baseline
       feature negotiation fixture: enable EDID before init stages 6/7. */
    g_features = 2U;
    CHECK(cvgpu_owned() != 0);
    CHECK(cvgpu_status.enabled == 2U); /* RAM framebuffer owned; init is nonblocking. */
    CHECK(cvgpu_status.width == 64U && cvgpu_status.height == 48U);
    CHECK(cvgpu_status.pitch == 256U);
    ((uint32_t *)(uintptr_t)fb)[0] = 0x12345678UL;
    expected_pixel.store(0x12345678UL);
    cvgpu_damage(0, 4);
    {
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cvgpu_status.enabled != 1U && std::chrono::steady_clock::now() < deadline) {
            CHECK(cvgpu_present(0) == 0U);
            std::this_thread::yield();
        }
    }
    CHECK(cvgpu_status.enabled == 1U);
    CHECK(cvgpu_edid_bytes == 128U);
    CHECK(std::memcmp(cvgpu_edid, mock_edid, sizeof(mock_edid)) == 0);
    for (unsigned i = 0; i < 1000000U && device_consumed.load() < 9U; ++i)
        std::this_thread::yield();
    CHECK(device_consumed.load() >= 9U);
    CHECK(cvgpu_present(0) == 0U); /* collect first full frame if init queued it */
    CHECK(protocol_errors == 0);
    {
        std::lock_guard<std::mutex> guard(command_lock);
        CHECK(command_types.size() == 9U);
        CHECK(command_types[0] == VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
        CHECK(command_types[1] == VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
        CHECK(command_types[2] == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        CHECK(command_types[3] == VIRTIO_GPU_CMD_SET_SCANOUT);
        CHECK(command_types[4] == VIRTIO_GPU_CMD_RESOURCE_FLUSH);
        CHECK(command_types[5] == VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
        CHECK(command_types[6] == VIRTIO_GPU_CMD_GET_EDID);
        CHECK(command_types[7] == VIRTIO_GPU_CMD_RESOURCE_FLUSH);
        CHECK(command_types[8] == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    }

    ((uint32_t *)(uintptr_t)fb)[0] = 0x89abcdefUL;
    expected_pixel.store(0x89abcdefUL);
    cvgpu_damage(0, 4);
    CHECK(cvgpu_present(0) == 0);
    CHECK(cvgpu_status.submits == 4U);
    for (unsigned i = 0; i < 1000000U && device_consumed.load() < 11U; ++i)
        std::this_thread::yield();
    CHECK(device_consumed.load() >= 11U);
    CHECK(cvgpu_present(0) == 0);
    CHECK(cvgpu_status.completions == 4U);
    CHECK(protocol_errors == 0);
    {
        std::lock_guard<std::mutex> guard(command_lock);
        CHECK(command_types.size() == 11U);
        CHECK(command_types[9] == VIRTIO_GPU_CMD_RESOURCE_FLUSH);
        CHECK(command_types[10] == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    }
    /* Identical damage, including compositor page reuse, must not publish
       another texture. A later non-origin pixel still reaches its exact rect. */
    uint32_t sent = cvgpu_status.submits;
    cvgpu_damage(0, 64U * 48U * 4U);
    CHECK(cvgpu_present(0) == 0U);
    CHECK(cvgpu_status.submits == sent);
    CHECK(cvgpu_status.skipped != 0U);
    ((uint32_t *)(uintptr_t)fb)[7U * 64U + 11U] = 0x0badcafeU;
    cvgpu_damage((7U * 64U + 11U) * 4U, 4U);
    CHECK(cvgpu_present(0) == 0U);
    gpu_transfer *changed = (gpu_transfer *)(uintptr_t)g_command;
    CHECK(changed->x == 11U && changed->y == 7U);
    CHECK(changed->width == 1U && changed->height == 1U);
    CHECK(changed->offset_lo == (7U * 64U + 11U) * 4U);
    CHECK(((uint32_t *)(uintptr_t)g_staging)[7U * 64U + 11U] == 0x0badcafeU);
    CHECK(((uint32_t *)(uintptr_t)g_staging)[0] == 0x89abcdefU);
    for (unsigned i = 0; i < 1000000U && device_consumed.load() < 13U; ++i)
        std::this_thread::yield();
    CHECK(cvgpu_present(0) == 0U);
    CHECK(cvgpu_status.completions == sent + 2U);
    CHECK(protocol_errors == 0);
    CHECK(cvgpu_release() == 0U);
    CHECK(cvgpu_owned() == 0U);
    CHECK(original_command == 1U); /* restore prior PCI command incl. bus-master bit */
    CHECK(allocations.empty());
    device_running.store(false);
    device_thread.join();

    /* Unsupported VERSION_1 must unwind the framebuffer and partial mappings. */
    configure_mock_pci();
    *(volatile uint32_t *)(mmio_base + 4U) = 0;
    CHECK(cvgpu_bind(64U * 48U * 4U) == 0U);
    CHECK(cvgpu_owned() == 0U);
    CHECK(allocations.empty());
    CHECK(original_command == 1U);
    munmap((void *)(uintptr_t)mmio_base, 0x10000U);
    std::fprintf(stdout, "session_gpu host mock: %u checks passed\n", checks);
}

int main(void)
{
    CHECK(sizeof(gpu_desc) == 16U);
    CHECK(sizeof(gpu_avail) == 70U);
    CHECK(sizeof(gpu_used) == 264U);
    CHECK(sizeof(gpu_create) == 40U);
    CHECK(sizeof(gpu_attach) == 32U);
    CHECK(sizeof(gpu_mem_entry) == 16U);
    CHECK(sizeof(gpu_transfer) == 56U);
    CHECK(sizeof(gpu_scanout) == 48U);
    CHECK(sizeof(gpu_flush) == 48U);
    CHECK(sizeof(gpu_response) == 24U);
    test_gpu_protocol();
    return 0;
}
