/* Host regression for the VBE framebuffer PCI BAR alias allowlist. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_gpu_legacy.c"

static uint32_t pci_address;
static uint32_t config[8][32][8][64];
static unsigned checks;

uint32_t cvdev_in(uint32_t port, uint32_t size)
{
    uint8_t bus, dev, fn, reg;
    if (port != 0x0cfcU || size != 4U) return 0U;
    bus = (uint8_t)(pci_address >> 16);
    dev = (uint8_t)((pci_address >> 11) & 31U);
    fn = (uint8_t)((pci_address >> 8) & 7U);
    reg = (uint8_t)(pci_address & 0xfcU);
    if (bus >= 8U) return 0xffffffffUL;
    if (config[bus][dev][fn][0] == 0U && reg == 0U) return 0xffffffffUL;
    return config[bus][dev][fn][reg / 4U];
}

void cvdev_out(uint32_t port, uint32_t value, uint32_t size)
{
    if (port == 0x0cf8U && size == 4U) pci_address = value;
}

uint32_t cvgpu_map_mmio(uint32_t physical, uint32_t bytes)
{
    (void)physical;
    (void)bytes;
    return 0U;
}

void cvgpu_unmap_mmio(uint32_t linear) { (void)linear; }

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "check %u failed at %s:%d: %s\n", \
                checks, __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static void clear_devices(void)
{
    memset(config, 0, sizeof(config));
    pci_address = 0U;
}

static void add_vga(uint8_t bus, uint8_t dev, uint16_t vendor,
                    uint16_t device, uint8_t bar, uint32_t physical)
{
    uint32_t *cfg = config[bus][dev][0];
    cfg[0] = ((uint32_t)device << 16) | vendor;
    cfg[1] = 0x00000002U; /* PCI memory decoding enabled */
    cfg[2] = 0x03000000U; /* VGA-compatible display controller */
    cfg[4U + bar] = physical;
}

int main(void)
{
    const uint32_t alias = 0x70000000UL; /* below 2 GiB and above 1 MiB */

    clear_devices();
    add_vga(0U, 2U, 0x5333U, 0x8c2eU, 0U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 0U); /* banked S3 VBE PhysBasePtr */

    clear_devices();
    add_vga(0U, 2U, 0x5333U, 0x8c2eU, 1U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 0U); /* SuperSavage framebuffer BAR1 */

    clear_devices();
    add_vga(0U, 2U, 0x5333U, 0x8c2eU, 2U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 0U); /* SuperSavage aperture BAR2 */

    clear_devices();
    add_vga(0U, 3U, 0x10deU, 0x0020U, 0U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 0U); /* unrelated VGA-class GPU */

    clear_devices();
    add_vga(0U, 4U, 0x1234U, 0x1111U, 1U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 0U); /* standard VGA BAR1 is not VBE BAR0 */

    clear_devices();
    add_vga(0U, 5U, 0x1af4U, 0x1050U, 0U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 0U); /* virtio-vga BAR0 is unrelated */

    clear_devices();
    add_vga(1U, 6U, 0x1234U, 0x1111U, 0U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 1U); /* QEMU standard VGA BAR0 on bus 1 */

    clear_devices();
    add_vga(1U, 7U, 0x1af4U, 0x1050U, 2U, alias);
    CHECK(cvlegacy_vbe_alias(alias) == 1U); /* virtio-vga BAR2 on bus 1 */

    CHECK(cvlegacy_vbe_alias(0U) == 0U);
    CHECK(cvlegacy_vbe_alias(0x000fffffUL) == 0U);
    printf("session_gpu_legacy host mock: %u checks passed\n", checks);
    return 0;
}
