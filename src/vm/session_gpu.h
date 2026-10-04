#ifndef CIUKIOS_SESSION_GPU_H
#define CIUKIOS_SESSION_GPU_H

#include <stdint.h>

/* Optional VirtIO GPU 2D path. A zero bind result means keep the VBE path. */
uint32_t cvgpu_bind(uint32_t framebuffer_bytes);
void cvgpu_trace(uint32_t event, uint32_t value);
uint32_t cvgpu_present(uint32_t yoffset);
void cvgpu_damage(uint32_t offset, uint32_t bytes);
uint32_t cvgpu_owned(void);
uint32_t cvgpu_release(void);

typedef struct {
    char magic[8];
    uint32_t size, version, enabled, width, height, pitch, device_status;
    uint32_t submits, completions, skipped, error_stage;
} cvgpu_status_info;
extern volatile cvgpu_status_info cvgpu_status;
extern uint8_t cvgpu_edid[128];
extern uint32_t cvgpu_edid_bytes;

#endif
