/* Ciuki glue for the two Haiku r1beta5 S3 ports. GPL-2.0-only.
 * Private kernel interface; no userspace ABI. Register layouts stay in each
 * backend. Shared policy does not treat either engine as a Savage. */
#ifndef CIUKI_S3LEGACY_PRIVATE_H
#define CIUKI_S3LEGACY_PRIVATE_H
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>

struct s3_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned, unsigned);
    void (*write)(void *, unsigned, uint32_t, unsigned);
    uint8_t (*cr_read)(void *, uint8_t);
    void (*cr_write)(void *, uint8_t, uint8_t);
    uint64_t (*now)(void *);
    int (*delay)(void *, unsigned);
    void (*drain)(void *);
};
struct s3_device;
struct s3_backend {
    bool (*accept)(const struct pci_func *);
    bool (*format)(const struct fb_device *);
    bool (*firmware)(struct s3_device *);
    bool (*available)(struct s3_device *, unsigned);
    int (*configure)(struct s3_device *);
    int (*fill)(struct s3_device *, const struct fb_rect *, uint32_t);
    int (*copy)(struct s3_device *, const struct fb_rect *, int32_t, int32_t);
    bool cursor_low_first;
};
struct s3_device {
    const struct s3_backend *backend;
    const char *id, *name, *reason;
    struct s3_io io;
    struct fb_device fb;
    struct pci_func pci;
    struct accel_ops ops;
    bool active, quarantined, cpu_safe, touched, authorized, blocked;
    bool cursor_installed, cursor_hidden, cursor_available, cursor_unsafe;
    bool mmio_changed;
    uint8_t saved_cr53;
    unsigned timeouts;
    uint64_t fills, copies;
    int (*pause)(void *);
    void *pause_arg;
    volatile uint8_t *registers;
    uint8_t *cursor;
    void *reg_window, *cursor_window;
    uint32_t cursor_offset;
    int reg_claim, port_claim, cursor_claim;
    gen_t reg_generation, port_generation, cursor_generation;
};
extern const struct s3_backend s3trio64_backend, s3virge_backend;
int s3_wait(struct s3_device *, unsigned);
void s3_fault(struct s3_device *, const char *);
void s3_launch(struct s3_device *);
bool s3_rect(const struct s3_device *, const struct fb_rect *);
int s3_engine_init(struct s3_device *);
int s3_sync(void *);
int s3_fill(void *, const struct fb_rect *, uint32_t);
int s3_copy(void *, const struct fb_rect *, int32_t, int32_t);
int s3_cursor_set(void *, const struct accel_cursor *);
int s3_cursor_move(void *, int32_t, int32_t);
int s3_cursor_show(void *, bool);
int s3_stop(void *);
unsigned s3_caps(void *);
bool s3_cpu_ready(void *);
void s3_pause(void *, int (*)(void *), void *);
bool s3_eligible(const struct fb_device *, const struct s3_backend *);
const struct accel_ops *s3_start(struct s3_device *, const struct fb_device *, struct vfs *);
const struct accel_ops *s3_fallback(struct s3_device *);
int s3_attempt_begin(struct s3_device *, struct vfs *);
int s3_attempt_end(struct s3_device *, struct vfs *);
int s3_attempt_failed(struct s3_device *, struct vfs *);
int s3_check_begin(struct s3_device *, struct vfs *, bool *);
void s3_snapshot(struct s3_device *, struct ciuki_accel_info *);
bool s3_summary(struct s3_device *, bool *, bool *, const char **);
void s3_record(struct s3_device *);
int s3_cursor_test(const struct accel_ops *, uint64_t, bool (*)(void *), void *);
/* Native platform separated from the engine to allow fake-register tests. */
int s3_access_begin(struct s3_device *);
int s3_access_end(struct s3_device *);

#define S3_ADAPTER(symbol, ident, label, engine, table) \
static struct s3_device state = {.backend=&engine,.id=ident,.name=label, \
    .reason="pending",.cpu_safe=true,.cursor_hidden=true, \
    .reg_claim=-1,.port_claim=-1,.cursor_claim=-1}; \
static bool probe(const struct pci_func *p) { return engine.accept(p); } \
static bool eligible(const struct fb_device *d) { return s3_eligible(d,&engine); } \
static const struct accel_ops *start(const struct fb_device *d,struct vfs *v) { return s3_start(&state,d,v); } \
static const struct accel_ops *fallback(void) { return s3_fallback(&state); } \
static int begin(struct vfs *v) { return s3_attempt_begin(&state,v); } \
static int end(struct vfs *v) { return s3_attempt_end(&state,v); } \
static int failed(struct vfs *v) { return s3_attempt_failed(&state,v); } \
static int check(struct vfs *v,bool *r) { return s3_check_begin(&state,v,r); } \
static void quarantine(const char *r) { s3_fault(&state,r); } \
static bool summary(bool *r,bool *u,const char **why) { return s3_summary(&state,r,u,why); } \
static void snapshot(struct ciuki_accel_info *i) { s3_snapshot(&state,i); } \
static void record(void) { s3_record(&state); } \
const struct display_driver symbol = { .id=ident,.attempt_files={"C:/system/settings/accel.cfg"}, \
    .name=label,.matches=table, \
    .match_count=ARRAY_SIZE(table),.framebuffer_bar=0,.probe=probe,.eligible=eligible, \
    .start=start,.fallback=fallback,.attempt_begin=begin,.attempt_end=end, \
    .attempt_failed=failed,.check_begin=check,.quarantine=quarantine,.summary=summary, \
    .snapshot=snapshot,.record=record,.cursor_test=s3_cursor_test }
#endif
