#ifndef CIUK_WEB_WORKER_PAGE_H
#define CIUK_WEB_WORKER_PAGE_H

#include <stdint.h>

#define WPAGE_OK 0
#define WPAGE_E_ARGUMENT 1
#define WPAGE_E_IO 2
#define WPAGE_E_LIMIT 3
#define WPAGE_E_SCRIPT 4
#define WPAGE_E_FORMAT 5

/* Scan returns a u16 count followed by (u16 external-index, u16 URL bytes,
   URL bytes) records. The output is always in document order. */
int wpage_scan(const char *path, uint8_t *output, uint32_t capacity,
               uint32_t *output_bytes);
int wpage_resource(uint32_t external_index, const char *path);
int wpage_run(const char *output_path, char *error, uint32_t error_capacity);

#endif
