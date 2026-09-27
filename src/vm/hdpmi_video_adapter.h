#ifndef CIUKIOS_HDPMI_VIDEO_ADAPTER_H
#define CIUKIOS_HDPMI_VIDEO_ADAPTER_H

#include <stdint.h>

/* Stack image built by cvdpmi_video_fault before entering freestanding C. */
typedef struct cvdpmi_fault_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, host_esp, ebx, edx, ecx, eax;
    uint32_t return_eip;
    uint32_t error, eip, cs, eflags, esp, ss;
} cvdpmi_fault_frame;

/* 0 = the protected-mode VGA access was executed; nonzero = chain #PF. */
int cvdpmi_video_execute(cvdpmi_fault_frame *frame, void *shared_block);
uint8_t cvdpmi_video_port_read(void *shared_block, uint16_t port,
                               uint8_t status1);
void cvdpmi_video_port_write(void *shared_block, uint16_t port, uint8_t value);

#endif
