#ifndef CIUKIOS_STAGE2_V86_DISPATCH_H
#define CIUKIOS_STAGE2_V86_DISPATCH_H

#include <stdint.h>

#if defined(__has_include)
#  if __has_include("legacy_v86.h")
#    include "legacy_v86.h"
#    define CIUKIOS_HAVE_LEGACY_V86_H 1
#  endif
#endif

#ifndef CIUKIOS_HAVE_LEGACY_V86_H
#define CIUKIOS_HAVE_LEGACY_V86_H 0

#define LEGACY_V86_ARM_MAGIC   0xC1D39450u
#define LEGACY_V86_SENTINEL    0x0450u

typedef struct {
    uint16_t cs, ip;
    uint16_t ss, sp;
    uint16_t ds, es, fs, gs;
    uint32_t eflags;
    uint32_t reserved[4];
} legacy_v86_frame_t;

typedef enum {
    LEGACY_V86_EXIT_NORMAL = 0,
    LEGACY_V86_EXIT_GP_INT,
    LEGACY_V86_EXIT_HALT,
    LEGACY_V86_EXIT_FAULT,
} legacy_v86_exit_reason_t;

typedef struct {
    legacy_v86_exit_reason_t reason;
    uint8_t int_vector;
    legacy_v86_frame_t frame;
    uint32_t fault_code;
} legacy_v86_exit_t;

int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out);
int legacy_v86_arm(uint32_t magic);
void legacy_v86_disarm(void);
int legacy_v86_is_armed(void);
int legacy_v86_probe(void);
#endif

#define V86_DISPATCH_ARM_MAGIC   0xC1D39460u
#define V86_DISPATCH_SENTINEL    0x0460u

typedef enum {
    V86_DISPATCH_CONT,
    V86_DISPATCH_EXIT_OK,
    V86_DISPATCH_EXIT_ERR,
    V86_DISPATCH_EXEC_REQUEST,
} v86_dispatch_result_t;

v86_dispatch_result_t v86_dispatch_int(uint8_t vector, legacy_v86_frame_t *frame);

int  v86_dispatch_get_exec_path(char *out, uint32_t out_size);
int  v86_dispatch_get_exec_tail(char *out, uint32_t out_size);
uint16_t v86_dispatch_get_exec_env_seg(void);
void v86_dispatch_clear_exec_path(void);

int  v86_dispatch_arm(uint32_t magic);
void v86_dispatch_disarm(void);
int  v86_dispatch_is_armed(void);
int  v86_dispatch_probe(void);

#endif