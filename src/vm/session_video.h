/* CiukiOS CVSESSION video monitor: the ring-0 half of the monitored VGA.
 *
 * Linked into CVSESSION.DLL. Called only from session_video.inc with flat
 * DS=ES=SS, DF=0, interrupts disabled and a private stack. It owns no
 * allocation and no Jemm service: the assembler glue reserves the shared
 * block, installs traps and calls cvvid_attach/cvvid_detach.
 *
 * Guest memory is reached through the pinned JLOAD page-table self-map
 * (FF800000h): conventional pages are touched only when their PTE is
 * present and user-accessible, so a bad guest pointer can never fault ring 0.
 */
#ifndef CIUKIOS_SESSION_VIDEO_H
#define CIUKIOS_SESSION_VIDEO_H

#include "virtual_vga_bios.h"
#include "vga_presenter.h"

/* Jemm/JLOAD Client_Reg_Struc (Include/JLM.INC at the pinned revision). */
typedef struct cvvid_client {
    uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    uint32_t int_number, error, eip, cs, eflags, esp, ss, es, ds, fs, gs;
} cvvid_client;
typedef char cvvid_client_size_must_be_76[sizeof(cvvid_client) == 76 ? 1 : -1];

/* First page of the shared block; cvga_state follows at CVVID_MODEL_OFFSET.
 * A protected-mode adapter maps the same physical pages (VIDEO_SHARE), so
 * both halves operate on one device state. */
#define CVVID_SHARED_MAGIC 0x53565643UL        /* CVVS */
#define CVVID_MODEL_OFFSET 4096UL
#define CVVID_BLOCK_BYTES (CVVID_MODEL_OFFSET + ((CVGA_STATE_BYTES + 4095UL) & ~4095UL))
typedef struct cvvid_shared {
    uint32_t magic, version, bytes, generation;
    uint32_t tsc_khz, tsc_base_lo, tsc_base_hi, fatal;
    uint32_t pm_faults, pm_instructions, pm_elements, pm_unsupported;
    uint32_t pm_port_reads, pm_port_writes, pm_attached, reserved;
    /* VIDEO_STATE layout, refreshed on every timer tick for observers. */
    uint32_t live[64];
} cvvid_shared;

typedef struct cvvid_config {
    uint32_t magic;
    uint16_t version, bytes;
    uint32_t tsc_khz;
    uint32_t font[CVBIOS_FONT_COUNT];
    uint32_t flags;
    uint32_t reserved[8];
} cvvid_config;
typedef char cvvid_config_size_must_be_64[sizeof(cvvid_config) == 64 ? 1 : -1];

typedef struct cvvid_present_packet {
    uint32_t magic;
    uint16_t version, bytes;
    uint16_t band_segment, band_top;
    uint32_t band_bytes;
    cvp_format format;
    cvp_request request;
    cvp_stats stats;
    uint32_t interval_us, ticks, presents, last_tsc, max_tsc;
    uint8_t reserved[256 - 232];
} cvvid_present_packet;
typedef char cvvid_present_packet_size[sizeof(cvvid_present_packet) == 256 ? 1 : -1];
typedef char cvp_format_size[sizeof(cvp_format) == 16 ? 1 : -1];
typedef char cvp_request_size[sizeof(cvp_request) == 148 ? 1 : -1];
typedef char cvp_stats_size[sizeof(cvp_stats) == 32 ? 1 : -1];

/* Error codes mirror session_abi.inc / session_video_abi.inc. */
enum {
    CVVID_OK = 0, CVVID_E_OPERATION = 1, CVVID_E_INACTIVE = 4, CVVID_E_ADDRESS = 7,
    CVVID_E_ABI = 9, CVVID_E_FB_UNBOUND = 11,
    CVVID_E_FORMAT = 0x20, CVVID_E_GEOMETRY = 0x21, CVVID_E_SHARED = 0x22,
    CVVID_E_FATAL = 0x23, CVVID_E_HOST = 0x24
};
enum {
    CVVID_FATAL_NONE = 0, CVVID_FATAL_INSTRUCTION = 1, CVVID_FATAL_BUS = 2,
    CVVID_FATAL_FETCH = 3, CVVID_FATAL_DIVIDE = 4
};

/* Linear framebuffer binding owned by the core JLM (bind_framebuffer). */
typedef struct cvvid_fb {
    uint8_t *linear;
    uint32_t bytes;
} cvvid_fb;

int CVGA_CALL cvvid_attach(cvvid_shared *block, uint32_t generation);
/* Restores BDA/IVT video state; the caller restores PTEs and frees memory. */
void CVGA_CALL cvvid_detach(void);
int CVGA_CALL cvvid_attached(void);
int CVGA_CALL cvvid_share_count(void);
/* 0: handled, RET to Jemm. 1: not ours or unsupported, chain. */
int CVGA_CALL cvvid_v86_fault(cvvid_client *client, uint32_t cr2, const cvvid_fb *fb);
uint32_t CVGA_CALL cvvid_port_read(uint32_t port);
void CVGA_CALL cvvid_port_write(uint32_t port, uint32_t value);
/* String port I/O (INS/OUTS, optional REP) on a trapped VGA port. type is
 * Jemm's decoded I/O type: bit 2 output, 3 word, 4 dword, 5 string, 6 REP,
 * high word = DS (OUTS) or ES (INS). Performs the whole transfer against the
 * model and updates SI/DI/CX. Returns 0 when handled, 1 to reject. */
uint32_t CVGA_CALL cvvid_port_string(uint32_t port, uint32_t type, cvvid_client *client);
/* Always handles INT 10h while attached (unsupported calls return unchanged). */
void CVGA_CALL cvvid_int10(cvvid_client *client);
/* VM_OP_VIDEO_* dispatcher; returns a CVVID_E_* code. fb may be unbound. */
uint32_t CVGA_CALL cvvid_operation(uint32_t op, cvvid_client *client, const cvvid_fb *fb);
/* V86 INT 08h hook body: bounded presentation of an ARMed request. */
void CVGA_CALL cvvid_timer(const cvvid_fb *fb);
/* Side-effect-free CPU-view peek for the legacy READBACK operation. */
uint32_t CVGA_CALL cvvid_readback(uint32_t offset, uint32_t count, uint32_t destination);

/* Provided by session_video.inc (ring-0 glue). */
extern void CVGA_CALL cvvid_rdtsc(uint32_t *low, uint32_t *high);
extern uint32_t CVGA_CALL cvvid_io_in(uint32_t port, uint32_t size);
extern void CVGA_CALL cvvid_io_out(uint32_t port, uint32_t value, uint32_t size);

#endif
