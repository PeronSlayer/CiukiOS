/* Link the existing register model through the production registry.
 * Only the hardware boundary and frozen driver's entry point are substituted. */
#include "../../src/kernel/drivers/video/s3savage/savage2d.h"
#pragma weak savage2d_init
#pragma weak savage2d_supported
#pragma weak savage2d_eligible
#pragma weak savage2d_fallback_ops
#pragma weak savage2d_attempt_begin
#pragma weak savage2d_attempt_end
#pragma weak savage2d_attempt_failed
#pragma weak savage2d_quarantine
#pragma weak savage2d_record
#pragma weak savage2d_summary
#pragma weak savage2d_snapshot
#ifndef DISPLAY_DRIVER_FIXTURE_PCI
unsigned pci_count(void) { return 0; }
const struct pci_func *pci_get(unsigned i) { (void)i; return NULL; }
#endif
static const struct display_match fixture_match[]={{0x5333,0x8c2e,3,255},{0x5333,0x8a22,3,255}};
const struct display_driver s3savage_driver = {
    .id="s3savage",.name="S3 Savage",.matches=fixture_match,.match_count=2,.framebuffer_bar=1,
    .probe=savage2d_supported,.eligible=savage2d_eligible,.start=savage2d_init,.fallback=savage2d_fallback_ops,
    .attempt_begin=savage2d_attempt_begin,.attempt_end=savage2d_attempt_end,.attempt_failed=savage2d_attempt_failed,
    .quarantine=savage2d_quarantine,.record=savage2d_record,.summary=savage2d_summary,.snapshot=savage2d_snapshot
};
const struct display_driver mach64_driver = {0};
const struct display_driver rage128_driver = {0};
const struct display_driver radeon2d_driver = {0};
const struct display_driver sis300_driver = {0};
const struct display_driver sis315_driver = {0};
const struct display_driver cirrus_driver = {0};
const struct display_driver trident_blade_driver = {0};
const struct display_driver chipshiqv_driver = {0};
const struct display_driver matrox_driver = {0};
const struct display_driver nv2d_driver = {0};
const struct display_driver neomagic_driver = {0};
const struct display_driver unichrome_driver = {0};
const struct display_driver tdfx_driver = {0};
const struct display_driver vmwaresvga_driver = {0};
const struct display_driver s3trio64_driver = {0};
const struct display_driver s3virge_driver = {0};
const struct display_driver i740_driver = {0};
const struct display_driver i810_driver = {0};
const struct display_driver intel2d_gen2_driver = {0};
const struct display_driver intel2d_gen3_driver = {0};
#include "../../src/kernel/drivers/video/registry.c"
