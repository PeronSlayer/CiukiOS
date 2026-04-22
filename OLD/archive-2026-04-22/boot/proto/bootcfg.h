#ifndef BOOTCFG_H
#define BOOTCFG_H

#include <stdint.h>

#define BOOTCFG_VERSION 1U
#define BOOTCFG_FLAG_ENABLED 0x01U
#define BOOTCFG_FLAG_MAX_HINT 0x02U
#define BOOTCFG_MODE_ID_NONE 0xFFFFFFFFU

#define BOOTCFG_CMOS_BASE 0x40U
#define BOOTCFG_CMOS_SIZE 24U

typedef struct __attribute__((packed)) bootcfg_data {
    uint8_t magic[4];
    uint8_t version;
    uint8_t flags;
    uint16_t reserved;
    uint32_t mode_id;
    uint32_t width;
    uint32_t height;
    uint32_t crc32;
} bootcfg_data_t;

static inline uint32_t bootcfg_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (uint32_t b = 0; b < 8U; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static inline void bootcfg_set_defaults(bootcfg_data_t *cfg) {
    if (!cfg) {
        return;
    }

    cfg->magic[0] = 'C';
    cfg->magic[1] = 'I';
    cfg->magic[2] = 'U';
    cfg->magic[3] = 'K';
    cfg->version = (uint8_t)BOOTCFG_VERSION;
    cfg->flags = 0U;
    cfg->reserved = 0U;
    cfg->mode_id = BOOTCFG_MODE_ID_NONE;
    cfg->width = 0U;
    cfg->height = 0U;
    cfg->crc32 = 0U;
}

static inline void bootcfg_finalize(bootcfg_data_t *cfg) {
    if (!cfg) {
        return;
    }

    cfg->crc32 = bootcfg_crc32((const void *)cfg, (uint32_t)(BOOTCFG_CMOS_SIZE - sizeof(uint32_t)));
}

static inline uint8_t bootcfg_valid(const bootcfg_data_t *cfg) {
    uint32_t expected_crc;

    if (!cfg) {
        return 0U;
    }

    if (cfg->magic[0] != 'C' || cfg->magic[1] != 'I' ||
        cfg->magic[2] != 'U' || cfg->magic[3] != 'K') {
        return 0U;
    }

    if (cfg->version != (uint8_t)BOOTCFG_VERSION) {
        return 0U;
    }

    expected_crc = bootcfg_crc32((const void *)cfg, (uint32_t)(BOOTCFG_CMOS_SIZE - sizeof(uint32_t)));
    if (cfg->crc32 != expected_crc) {
        return 0U;
    }

    if ((cfg->flags & BOOTCFG_FLAG_ENABLED) == 0U) {
        return 0U;
    }

    if (cfg->mode_id == BOOTCFG_MODE_ID_NONE && (cfg->width == 0U || cfg->height == 0U)) {
        return 0U;
    }

    return 1U;
}

#endif
