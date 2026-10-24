/* SPDX-License-Identifier: MIT
 * Internal native printing API; shared by the native and Win32 spool producers. */
#ifndef CIUKI_NATIVE_PRINT_H
#define CIUKI_NATIVE_PRINT_H
#include <stddef.h>
#include <stdint.h>
#define CP_ROOT "/WINDOWS/SPOOL"
#define CP_PRINTERS 8u
#define CP_JOBS 128u
#define CP_TITLE 96u
#define CP_PATH 256u
#define CP_JOB_MAGIC 0x314a5043u
#define CP_CONFIG_MAGIC 0x31435043u
#define CP_MAX_BYTES (16u * 1024u * 1024u)
enum cp_driver { CP_TEXT, CP_POSTSCRIPT, CP_PCL };
enum cp_state { CP_QUEUED, CP_PAUSED, CP_PRINTING, CP_OFFLINE, CP_ERROR };
enum cp_control { CP_PAUSE = 1, CP_RESUME, CP_CANCEL };
/* Dimensions and margins are points (1/72 inch); ranges are inclusive, 0=all. */
struct cp_page {
    uint32_t width, height, landscape;
    uint32_t left, top, right, bottom;
    uint32_t copies, first, last;
};
struct cp_printer {
    uint32_t present, driver, paused;
    char name[64];
    char port[CP_PATH];
    struct cp_page page;
};
struct cp_config {
    uint32_t magic, version, default_printer;
    struct cp_printer printers[CP_PRINTERS];
};
/* Fixed little-endian record, no pointers/size_t. Slot id is 1..128.
 * /WINDOWS/SPOOL/j%03u/{data,job}; slot directory is exclusively mkdir'd.
 * Publish data then job.tmp -> job; worker ignores directories without job.
 * Control is separate ctl file (ASCII p/r/c) to avoid worker metadata races. */
struct cp_job {
    uint32_t magic, version, id, printer, state, bytes, offset, completed;
    struct cp_page page;
    char title[CP_TITLE];
    char target[CP_PATH];
};
void cp_page_default(struct cp_page *);
int cp_page_valid(const struct cp_page *);
int cp_init(const char *root);
int cp_config_load(const char *root, struct cp_config *);
int cp_config_save(const char *root, const struct cp_config *);
int cp_add_printer(const char *root, const char *name, const char *port, unsigned driver, unsigned *index);
int cp_set_default(const char *root, unsigned printer);
int cp_printer_pause(const char *root, unsigned printer, int pause);
int cp_job_read(const char *root, unsigned id, struct cp_job *);
int cp_job_control(const char *root, unsigned id, enum cp_control);
int cp_purge(const char *root, unsigned printer);
/* printer=CP_PRINTERS selects default; target overrides port for Print to File.
 * Return positive job id, or -1/errno. Text and images rendered before publish.
 * Image pixels are tightly packed 0x00RRGGBB, row-major, top-to-bottom. */
int cp_submit_text(const char *root, unsigned printer, const char *title, const void *text, size_t bytes, const struct cp_page *, const char *target);
int cp_submit_image(const char *root, unsigned printer, const char *title, const uint32_t *pixels, unsigned width, unsigned height, const struct cp_page *, const char *target);
int cp_submit_raw(const char *root, unsigned printer, const char *title, const void *data, size_t bytes, const struct cp_page *, const char *target);
/* Stream large raw jobs without loading them into memory. */
int cp_submit_file(const char *root, unsigned printer, const char *title, const char *source, const struct cp_page *, const char *target);
/* One bounded (1024 bytes) background service step. Returns 1 progress, 0 idle,
 * -1 error; offline jobs remain queued and are retried on later calls. */
int cp_service_step(const char *root);
int cp_service_lock(const char *root);
void cp_service_unlock(const char *root);
const char *cp_state_name(unsigned state);
#endif
