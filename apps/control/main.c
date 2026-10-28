#include <ciuki/recovery.h>
/* SPDX-License-Identifier: MIT
 * /Programs/control: CiukiOS Control Panel. Usage: control [--channel-fd=N] [--panel=NAME]
 * NAME: home, display, mouse, keyboard, sounds, datetime, system, devices, drivers, network, power, adapter, access. */
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include "control.h"
#include "internet_storage.h"
#include "../files/operations.h"
#include "../desktop/win32_adapter.h"
#include "programs.h"
#include "decoder.h"
#include "job.h"
#include "../desktop/users_worker.h"
#include "../desktop/profile.h"
#include <ciuki/channel.h>
#include "../../sdk/sysroot-overlay/include/ciuki/dosvm.h"
#ifdef __i386__
#include <ciuki/raw.h>
#endif
#include "../desktop/sound_storage.h"
#include "../desktop/settings_io.h"
#include <ciuki/audio.h>
#include <ciuki/proc.h>
#include <ciuki/devices.h>
#include <ciuki/net.h>
#include <ciuki/power.h>
#include <ciuki/timezone.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "network_io.inc"
#include "../dosbox/port_settings.h"

struct cp_main { unsigned icon_generation; /* fx-exeicons: du_file_icons_generation() shown */ struct users_worker users_worker; char settings_temp[160]; struct cp_network_worker network_worker; struct cp_app app; struct du_ui ui; struct du_window window; struct cfg_writer writer;
    int writer_started, reload_pending, hwid_loaded, usb_hwid_loaded; char *hwid_pci, *hwid_usb; uint32_t portrait[256 * 256]; uint32_t install_serial, serial, display_serial, audio_serial, engine_serial;
    struct ciuki_display_engine_info generic_engine;
    struct ciuki_accel_info engine; int engine_ok; uint64_t engine_next; int title_page;
    uint32_t joystick_serial;
    struct cp_job menu_job;
    struct cp_menu_result menu_result;
    struct sh_edit_request menu_edit;
    char menu_args[1024];
    struct cp_job game_job;
    struct cp_game_result game_result;
    int saved_w, saved_h;
    const char *disp_state, *disp_decode, *disp_wait;
    unsigned disp_done;
    struct cp_job assets; int assets_requested, assets_ready;
    char wall_cache[CP_WALLPAPERS][CFG_PATH_MAX]; unsigned wall_count;
    char *pci_cache,*usb_cache; int portrait_ready;
    struct cp_job program_scan; int programs_requested,programs_ready,program_count,next_program_count;
    struct desk_uninstall_entry program_cache[CP_PROGRAM_MAX], *program_next;
    pthread_mutex_t completion_lock;
    int completion[2];
    uint32_t applet_serial;
    uint64_t tick_next;
};

/* Optional in host-only panel harnesses; production links startup.c. */
__attribute__((weak)) int desk_startup_list_at(const char *folder,const char *registry,struct desk_startup_entry *out,unsigned cap)
{ (void)folder; (void)registry; (void)out; (void)cap; return -ENOSYS; }
__attribute__((weak)) int desk_startup_enable_at(const char *registry,const struct desk_startup_entry *entry,int enabled)
{ (void)registry; (void)entry; (void)enabled; return -ENOSYS; }
static int startup_list(void *ctx,struct desk_startup_entry *out,unsigned cap)
{ (void)ctx; return desk_startup_list_at(DESK_STARTUP_FOLDER,"/System/Registry",out,cap); }
static int startup_enable(void *ctx,const struct desk_startup_entry *entry,int enabled)
{ (void)ctx; return desk_startup_enable_at("/System/Registry",entry,enabled); }
static void programs_poll(struct cp_main *);
static void notify_completion(void *);
static int user_begin(void *ctx,const struct users_request *q)
{ struct cp_main *m=ctx; return users_begin(&m->users_worker,q,notify_completion,m); }
static int user_poll(void *ctx,struct users_view *v,int *e)
{ struct cp_main *m=ctx; return users_poll(&m->users_worker,v,e); }
/* All applet jobs share one bounded notification channel. Queue saturation
 * already means readable; other send failures close the peer and wake HUP.
 * Serialize send/close so concurrent producers cannot use a recycled fd. */
static void notify_completion(void *ctx)
{
    struct cp_main *m=ctx;
    pthread_mutex_lock(&m->completion_lock);
    if (m->completion[1]>=0) {
        struct ciuki_message token={.length=4};
        int r;
        do r=ciuki_channel_send(m->completion[1],&token,DONTWAIT); while (r<0 && errno==EINTR);
        if (r<0 && errno!=EAGAIN) { close(m->completion[1]); m->completion[1]=-1; }
    }
    pthread_mutex_unlock(&m->completion_lock);
}
static int completion_start(struct cp_main *m)
{
    m->completion[0]=m->completion[1]=-1;
    int error=pthread_mutex_init(&m->completion_lock,NULL);
    if (error) { errno=error; return -1; }
    if (ciuki_channel_pair(m->completion)) { pthread_mutex_destroy(&m->completion_lock); return -1; }
    for (unsigned i=0;i<2;i++) (void)fcntl(m->completion[i],F_SETFD,FD_CLOEXEC);
    return 0;
}
/* Only after every producer is joined. */
static void completion_stop(struct cp_main *m)
{
    close(m->completion[0]);
    if (m->completion[1]>=0) close(m->completion[1]);
    pthread_mutex_destroy(&m->completion_lock);
}
/* The shared writer's desktop API uses trylock. An event consumer must read
 * its published result exactly once, without depending on a retry timer.
 * These critical sections never contain disk I/O. */
static int settings_busy(struct cp_main *m)
{
    if (!m->writer_started) return 0;
    pthread_mutex_lock(&m->writer.lock);
    int busy=m->writer.busy || m->writer.pending;
    pthread_mutex_unlock(&m->writer.lock);
    return busy;
}
static int settings_result(struct cp_main *m, int *error)
{
    if (!m->writer_started) return 0;
    pthread_mutex_lock(&m->writer.lock);
    int ready=m->writer.completed;
    if (ready) { *error=m->writer.error; m->writer.completed=0; }
    pthread_mutex_unlock(&m->writer.lock);
    return ready;
}
/* Panel geometry is independent of desktop preferences and display rollback. */
static void panel_size(int *width, int *height)
{
    *width=CP_WIDTH; *height=CP_HEIGHT;
    FILE *f=fopen(CFG_DIR "/control.size","r");
    int w,h; char extra;
    if (f) {
        if (fscanf(f,"%d %d %c",&w,&h,&extra)==2 && w>=CP_WIDTH && h>=CP_HEIGHT && w<=1600 && h<=1200) { *width=w; *height=h; }
        fclose(f);
    }
}
static void remember_size(struct cp_main *m)
{
    if (!m->window.resizable) return; /* property applets do not replace the browser geometry */
    int w=m->ui.canvas.width,h=m->ui.canvas.height;
    if (w==m->saved_w && h==m->saved_h) return;
    FILE *f=fopen(CFG_DIR "/control.tmp","w");
    if (!f) return;
    int ok=fprintf(f,"%d %d\n",w,h)>0 && !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok=0;
    if (ok && !rename(CFG_DIR "/control.tmp",CFG_DIR "/control.size")) { m->saved_w=w; m->saved_h=h; }
    else unlink(CFG_DIR "/control.tmp");
}

static int load(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx;
    int error=cfg_read_at(profile_settings(),text,size,length);
    if (error) { errno=error; return -1; }
    return 0;
}
static int save(void *ctx, const char *text, size_t length)
{
    struct cp_main *m=ctx;
    if (!m->writer_started) { errno=EIO; return -1; }
    return cfg_writer_submit(&m->writer,text,length);
}
/* BOOT.CFG is a pre-existing bounded boot file. In-place replacement fits
 * its existing cluster and works with the native VFS's rename semantics. */
static int boot_load(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx;
    int fd = open("/system/BOOT.CFG", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0; int ok = 1;
    while (n < size) {
        ssize_t got = read(fd, text + n, size - n);
        if (got < 0) { ok = 0; break; }
        if (!got) break;
        n += (size_t)got;
    }
    if (close(fd)) ok = 0;
    if (n >= size) ok = 0;
    if (ok) { text[n] = 0; *length = n; }
    return ok ? 0 : -1;
}
static int boot_write(int fd, const char *text, size_t length)
{
    size_t at = 0;
    while (at < length) {
        ssize_t n = pwrite(fd, text + at, length - at, (off_t)at);
        if (n <= 0) return -1;
        at += (size_t)n;
    }
    return ftruncate(fd, (off_t)length) || fsync(fd) ? -1 : 0;
}
static int boot_save(void *ctx, const char *text, size_t length)
{
    char previous[CP_BOOT_MAX]; size_t n = 0;
    if (length >= CP_BOOT_MAX || boot_load(ctx, previous, sizeof(previous), &n)) return -1;
    /* sec-sysvc: BOOT.CFG is a sealed boot input; an administrator saves it
     * through the SYSTEM boot-configuration service (it restores the old
     * text itself after a failed write). ENOSYS: an older kernel; ENODEV:
     * no managed system volume (Setup media), where the file is written. */
    if (!ciuki_boot_config_set(text, length)) return 0;
    if (errno != ENOSYS && errno != ENODEV) return -1;
    int fd = open("/system/BOOT.CFG", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = boot_write(fd, text, length);
    if (r) (void)boot_write(fd, previous, n); /* best effort after an I/O error */
    if (close(fd)) r = -1;
    return r;
}
#define CP_HW_FILE SYSTEM_HW_PATH
static int hw_load(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx;
    int fd = open(CP_HW_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) fd = open(CFG_DIR "/hwprofiles.cfg", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    size_t n = 0; int error = 0;
    while (n < size) {
        ssize_t got = read(fd, text + n, size - n);
        if (got < 0) { error = errno; break; }
        if (!got) break;
        n += (size_t)got;
    }
    if (close(fd) && !error) error = errno;
    if (!error && n >= size) error = EFBIG;
    if (error) return -error;
    text[n] = 0; *length = n;
    return 0;
}
/* Atomic replace: temporary file, fsync, rename (cfg_write_at). */
static int hw_save(void *ctx, const char *text, size_t length)
{ (void)ctx; int e = cfg_write_at(CP_HW_FILE, "/system/HWPROF.TMP", text, length); return e ? -e : 0; }
static uint32_t boot_serial(void *ctx)
{
    (void)ctx; struct ciuki_recovery_status status;
    return ciuki_recovery_status(&status) ? 0 : status.boots;
}
static int perf_load(void *ctx, struct system_perf *out)
{
    (void)ctx;
    FILE *f = fopen(SYSTEM_PERF_PATH, "rb");
    if (!f) return -errno;
    char text[512]; size_t n = fread(text, 1, sizeof(text), f);
    int e = ferror(f) || n == sizeof(text) ? -EIO : 0;
    fclose(f);
    if (!e && !system_perf_parse(out, text, n)) e = -EINVAL;
    return e;
}
static int perf_save(void *ctx, const struct system_perf *p)
{
    (void)ctx;
    if (!system_perf_valid(p)) return -EINVAL;
    char text[256];
    int n = snprintf(text, sizeof(text), "version=1\nrole=%u\nread_ahead=%u\ngraphics=%u\nvm=%u\nmin_mb=%u\nmax_mb=%u\n",
        p->role, p->read_ahead, p->graphics, p->vm, p->min_mb, p->max_mb);
    int e = cfg_write_at(SYSTEM_PERF_PATH, CFG_DIR "/performance.tmp", text, (size_t)n);
    return e ? -e : 0;
}
static int memory_query(void *ctx, uint64_t *total_k, uint64_t *free_k)
{
    (void)ctx;
    static uint8_t buffer[CIUKI_PROC_LIST_MIN + CIUKI_PROCESS_MAX * sizeof(struct ciuki_proc_record)];
    if (ciuki_proc_list(buffer, sizeof(buffer)) <= 0) return -EIO;
    struct ciuki_proc_system sys; memcpy(&sys, buffer, sizeof(sys));
    if (sys.version != CIUKI_PROC_VERSION) return -EIO;
    *total_k = (uint64_t)sys.usable_pages * 4u; *free_k = (uint64_t)sys.free_pages * 4u;
    return 0;
}
static int driver_catalog(void *ctx, struct aw_model *out, unsigned capacity)
{
    (void)ctx;
    struct dp_entry *e = calloc(AW_CATALOG_MAX, sizeof(*e));
    if (!e) return -ENOMEM;
    int n = dp_catalog(NDP_ROOT, e, AW_CATALOG_MAX), kept = 0;
    for (int i = 0; i < n && (unsigned)kept < capacity; i++) {
        size_t len = strlen(e[i].service); int ok = len > 0 && len < sizeof(out->driver);
        for (size_t k = 0; ok && k < len; k++) if ((unsigned char)e[i].service[k] <= ' ' || e[i].service[k] == '=' || (unsigned char)e[i].service[k] > 126) ok = 0;
        if (!ok) continue;
        memset(&out[kept], 0, sizeof(out[kept]));
        out[kept].class_id = CIUKI_DEVICE_NETWORK;
        snprintf(out[kept].name, sizeof(out[kept].name), "%s", e[i].name);
        snprintf(out[kept].driver, sizeof(out[kept].driver), "%s", e[i].service);
        kept++;
    }
    free(e);
    return n < 0 ? n : kept;
}
static int devices(void *ctx, struct ciuki_devices_info *out) { (void)ctx; return ciuki_devices(out); }
static int firmware(void *ctx, struct ciuki_firmware_page *out) { (void)ctx; return ciuki_firmware_inventory(out); }
static int pack_scan(void *ctx,const char *root,char names[][192],unsigned capacity)
{ (void)ctx; return dp_scan(root,names,capacity); }
static int pack_query(const struct ndp_device *d,const struct ndp_manifest *m,struct ciuki_driver_pack_query *q)
{
    memset(q,0,sizeof(*q)); q->version=CIUKI_DRIVER_PACK_VERSION; q->size=sizeof(*q);
    q->bus=d->bus; q->slot=d->slot; q->function=d->function; q->vendor=d->vendor; q->device=d->device;
    q->sub_vendor=d->sub_vendor; q->sub_device=d->sub_device; q->revision=d->revision;
    if(m) { memcpy(q->binary_hash,m->files[m->binary].hash,32); memcpy(q->package_seal,m->seal,32); }
    int fd=open("/dev/net",O_RDONLY|O_CLOEXEC); if(fd<0) return -errno;
    int r=ciuki_devctl(fd,NET_QUERY_DRIVER_PACK,q,sizeof(*q));
    if(close(fd) && !r) r=-errno;
    return r;
}
static int pack_preview(void *ctx,struct dp_plan *plan,const char *root,const char *inf,const struct ndp_device *d,char reason[160])
{
    (void)ctx; int r=dp_preview(plan,root,inf,d,reason); if(r) return r;
    struct ndp_manifest *m=&plan->manifest; struct ciuki_driver_pack_query q;
    m->enabled=1; snprintf(m->reason,sizeof(m->reason),"%s",NDP_QUALIFIED); ndp_seal(m);
    if(pack_query(d,m,&q) || !q.qualified) { m->enabled=0; snprintf(m->reason,sizeof(m->reason),"%s",NDP_DISABLED); ndp_seal(m); }
    snprintf(reason,160,"%s",m->reason); return 0;
}
static int pack_present(const struct ndp_device *d)
{
    struct ciuki_devices_info *info=calloc(1,sizeof(*info)); if(!info) return 0;
    info->version=CIUKI_DEVICES_VERSION; info->size=sizeof(*info); int found=0;
    if(!ciuki_devices(info) && cp_snapshot_valid(info)) for(unsigned i=0;i<info->count;i++) {
        const struct ciuki_device_record *r=&info->devices[i];
        struct ndp_device actual={r->bus,r->slot,r->function,r->vendor,r->device,r->sub_vendor,r->sub_device,r->revision};
        if(r->bus_type==CIUKI_BUS_PCI && r->class_id==CIUKI_DEVICE_NETWORK &&
           r->state!=CIUKI_DEVICE_QUARANTINED && r->state!=CIUKI_DEVICE_ABSENT && !memcmp(&actual,d,sizeof(*d))) found=1;
    }
    free(info); return found;
}
/* sec-sysvc: the driver store is sealed (K-Q4); the SYSTEM driver-pack
 * service installs and removes packages after an administrator check, reads
 * the package folder as this user and verifies every byte it writes. */
static int pack_service(struct ciuki_driver_pack_job *job,char reason[160])
{
    job->version=CIUKI_DRIVER_PACK_JOB_VERSION; job->size=sizeof(*job);
    int r=ciuki_driver_pack(job) ? -errno : 0;
    snprintf(reason,160,"%s",job->reason[0] ? job->reason : r==-EPERM ? "You must be an administrator to change driver packages." :
             r ? "The driver package service is unavailable." : "");
    return r;
}
static int pack_install(void *ctx,const struct dp_plan *plan,const char *root,char reason[160])
{
    (void)ctx;
    if(!pack_present(&plan->manifest.device)) { snprintf(reason,160,"Device changed or is quarantined. Refresh the inventory."); return -EBUSY; }
    if(plan->manifest.enabled) {
        struct ciuki_driver_pack_query q;
        if(pack_query(&plan->manifest.device,&plan->manifest,&q) || !q.qualified || q.busy) {
            snprintf(reason,160,"Qualification changed or the driver host is busy. Select the package again."); return -EBUSY;
        }
    }
    struct ciuki_driver_pack_job *job=calloc(1,sizeof(*job));
    if(!job) { snprintf(reason,160,"Not enough memory to install the package."); return -ENOMEM; }
    if(strlen(root)>=sizeof(job->root)) { free(job); snprintf(reason,160,"The location is too long."); return -ENAMETOOLONG; }
    job->action=CIUKI_DRIVER_PACK_INSTALL; strcpy(job->root,root);
    job->manifest=(uint32_t)(uintptr_t)&plan->manifest; job->manifest_bytes=sizeof(plan->manifest);
    job->sources=(uint32_t)(uintptr_t)plan->source; job->source_bytes=plan->manifest.file_count*(uint32_t)sizeof(plan->source[0]);
    int r=pack_service(job,reason);
    free(job);
    return r;
}
static int pack_remove(void *ctx,const struct ndp_device *d,char reason[160])
{
    (void)ctx;
    if(!pack_present(d)) { snprintf(reason,160,"Device changed or is quarantined. Refresh the inventory."); return -EBUSY; }
    /* The service checks the host as SYSTEM: an unreadable host state counts as busy. */
    struct ciuki_driver_pack_job job; memset(&job,0,sizeof(job));
    job.action=CIUKI_DRIVER_PACK_REMOVE; job.bus=d->bus; job.slot=d->slot; job.function=d->function;
    job.vendor=d->vendor; job.device=d->device; job.sub_vendor=d->sub_vendor; job.sub_device=d->sub_device; job.revision=d->revision;
    return pack_service(&job,reason);
}
/* The id database is intentionally opened on first Device Manager lookup.
 * Its generator emits a flat, deterministic JSON object with no escaped
 * strings, so lookup needs only bounded string scanning. */
static int hwid_name(void *ctx, unsigned vendor, unsigned device, char *out, size_t size)
{
    struct cp_main *m=ctx; m->assets_requested=1;
    return cp_hwid_lookup_json(m->hwid_pci,vendor,device,out,size);
}
static int usb_hwid_name(void *ctx, unsigned vendor, unsigned device, char *out, size_t size)
{
    struct cp_main *m=ctx; m->assets_requested=1;
    return cp_hwid_lookup_json(m->hwid_usb,vendor,device,out,size);
}
static int sound_validate(void *ctx, const char *path)
{ (void)ctx; return sound_validate_file(path); }
static int sound_schemes(void *ctx, char names[][SOUND_SCHEME_NAME], unsigned cap)
{ (void)ctx; return sound_scheme_list_at(SOUND_SCHEMES_DIR,names,cap); }
static int sound_scheme(void *ctx, unsigned op, const char *name, struct sound_scheme *s)
{ (void)ctx; return sound_scheme_at(SOUND_SCHEMES_DIR,op,name,s); }
static int load_sounds(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx; struct sound_scheme s;
    (void)sound_load_at(&s,SOUND_CFG,SOUND_LEGACY_CFG,REG_DIRECTORY);
    *length = sound_format(&s,text,size); return *length ? 0 : -1;
}
static int save_sounds(void *ctx, const char *text, size_t length)
{
    (void)ctx; struct sound_scheme s; sound_defaults(&s);
    if (sound_parse(&s,text,length)) return -1;
    return sound_save_at(&s,REG_DIRECTORY);
}
static int image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.') return 0;
    char ext[8]; size_t n = 0;
    for (dot++; *dot && n < sizeof(ext) - 1; dot++) ext[n++] = (char)(*dot >= 'A' && *dot <= 'Z' ? *dot + 32 : *dot);
    ext[n] = 0;
    return !strcmp(ext, "bmp") || !strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg");
}
static int scan_wallpapers(void *ctx, char names[][CFG_PATH_MAX], unsigned capacity)
{
    (void)ctx;
    DIR *dir = opendir(CFG_WALLPAPER_DIR);
    if (!dir) return 0;
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < capacity) {
        if (!image_name(e->d_name) || strlen(e->d_name) + sizeof(CFG_WALLPAPER_DIR) + 1 >= CFG_PATH_MAX) continue;
        snprintf(names[n], CFG_PATH_MAX, "%s/%s", CFG_WALLPAPER_DIR, e->d_name);
        /* Sorted insertion: the directory order is not meaningful. */
        for (unsigned k = n; k > 0 && strcmp(names[k - 1], names[k]) > 0; k--) {
            char t[CFG_PATH_MAX]; memcpy(t, names[k], CFG_PATH_MAX); memcpy(names[k], names[k - 1], CFG_PATH_MAX); memcpy(names[k - 1], t, CFG_PATH_MAX);
        }
        n++;
    }
    closedir(dir);
    return (int)n;
}
static char *read_ids(const char *path)
{
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    char *text=NULL;
    if (!fseek(f,0,SEEK_END)) {
        long n=ftell(f);
        if (n>0 && n<=400*1024 && !fseek(f,0,SEEK_SET)) {
            text=malloc((size_t)n+1);
            if (text && fread(text,1,(size_t)n,f)==(size_t)n) text[n]=0;
            else { free(text); text=NULL; }
        }
    }
    fclose(f); return text;
}
static void read_assets(void *ctx)
{
    struct cp_main *m=ctx;
    m->wall_count=(unsigned)scan_wallpapers(NULL,m->wall_cache,CP_WALLPAPERS);
    m->pci_cache=read_ids("/System/hwids/pci-ids.json");
    m->usb_cache=read_ids("/System/hwids/usb-ids.json");
    FILE *f=fopen("/system/assets/ciuki-portrait.xrgb","rb");
    if (f) { m->portrait_ready=fread(m->portrait,4,256*256,f)==256*256; fclose(f); }
}
static int wallpapers(void *ctx, char names[][CFG_PATH_MAX], unsigned capacity)
{
    struct cp_main *m=ctx;
    if (!m->assets_ready) return 0;
    unsigned count=m->wall_count<capacity ? m->wall_count : capacity;
    memcpy(names,m->wall_cache,count*CFG_PATH_MAX); return (int)count;
}
static int decode_begin(void *ctx, const struct cp_preview_req *req) { (void)ctx; return cp_decoder_begin(req); }
static int decode_poll(void *ctx, struct cp_preview_done *out) { (void)ctx; return cp_decoder_poll(out); }
static void decode_cancel(void *ctx) { (void)ctx; cp_decoder_cancel(); }
static int mixer(void *ctx, int *volume, int *muted)
{
    (void)ctx;
    int fd = open("/dev/mixer", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct ciuki_mixer_master m = {.version = CIUKI_AUDIO_VERSION, .size = sizeof(m)};
    int r = ciuki_devctl(fd, MIXER_GET_MASTER, &m, sizeof(m));
    close(fd);
    if (r < 0) return -1;
    *volume = (int)((m.left + m.right) / 2); *muted = m.mute != 0;
    return 0;
}
static int apply(void *ctx, unsigned request, int volume, int muted)
{
    struct cp_main *m = ctx;
    if ((request==DESK_SETTINGS_RELOAD || request==DESK_SETTINGS_PROGRAMS) && settings_busy(m)) {
        m->reload_pending=(int)request; return 0;
    }
    struct ciuki_message msg;
    if (!++m->serial) ++m->serial;
    cp_settings_message(&msg, m->serial, request, volume, muted);
    int r = du_window_queue(&m->window, &msg);
    if (!r && (request == DESK_SETTINGS_DISPLAY || request == DESK_SETTINGS_DISPLAY_PREVIEW)) m->display_serial = m->serial;
    if (!r && request == DESK_SETTINGS_AUDIO_TEST) m->audio_serial = m->serial;
    return r;
}
static void game_work(void *arg)
{
    struct cp_main *m=arg;struct cp_game_result *r=&m->game_result;
    r->error=r->saved ? dosbox_ports_save(r->game,&r->ports) : dosbox_ports_load(r->game,&r->ports,&r->rejected);
}
static int game_begin(void *ctx,unsigned game,const struct game_ports_settings *p)
{
    struct cp_main *m=ctx;
    if (m->game_job.started) return -EBUSY;
    m->game_result=(struct cp_game_result){.game=game,.saved=p!=NULL};
    if (p) m->game_result.ports=*p;
    return cp_job_start(&m->game_job,game_work,m,notify_completion,m);
}
static int game_poll_result(void *ctx,struct cp_game_result *r)
{
    struct cp_main *m=ctx;if (!cp_job_ready(&m->game_job)) return 0;
    cp_job_finish(&m->menu_job);
    cp_job_finish(&m->game_job);*r=m->game_result;return 1;
}
static int joystick_request(void *ctx, const struct ciuki_joystick_control *c)
{
    struct cp_main *m=ctx; struct ciuki_message msg;
    if (m->joystick_serial) return -EBUSY;
    if (!++m->serial) ++m->serial;
    desk_message(&msg,DESK_JOYSTICK,108); msg.data[0]=DESK_SHELL_VERSION;
    desk_put32(msg.data+4,m->serial); memcpy(msg.data+8,c,sizeof(*c));
    int e=du_window_queue(&m->window,&msg);
    if (!e) m->joystick_serial=m->serial;
    return e;
}
static int joystick_save_game(void *ctx,unsigned game,const struct ciuki_joystick_control *c)
{
    struct ciuki_joystick_control request=*c;
    request.operation=CIUKI_JOY_SAVE_GAME; request.axis=game;
    return joystick_request(ctx,&request);
}
static int autoplay_list(void *ctx,struct ap_preference *out,unsigned capacity)
{ (void)ctx; return ap_preference_list(NULL,out,capacity); }
static int autoplay_forget(void *ctx,const char *id)
{ (void)ctx; return ap_preference_forget(NULL,id); }
static int monitor_query(void *ctx,struct ciuki_monitor_info *q) { (void)ctx; return ciuki_display_monitor(q); }
static int refresh_query(void *ctx,struct ciuki_display_refresh *q) { (void)ctx; return ciuki_display_refresh(q); }
static int display_query(void *ctx, struct ciuki_display_info *info)
{ (void)ctx; return ciuki_display_query(info); }
static int generic_engine_query(void *ctx,struct ciuki_display_engine_info *info)
{
    struct cp_main *m=ctx;
    if (!m->engine_ok || m->generic_engine.version!=CIUKI_DISPLAY_ENGINE_VERSION) return -EAGAIN;
    *info=m->generic_engine; return 0;
}
#include "accel_retry.h"
static int accel_retry(void *ctx)
{
    struct cp_main *m=ctx;
    if (!m->engine_ok || m->generic_engine.version!=CIUKI_DISPLAY_ENGINE_VERSION) return -EAGAIN;
    return cp_accel_retry_request(CP_ACCEL_RETRY_PATH,&m->generic_engine);
}
static int engine_query(void *ctx, struct ciuki_accel_info *info)
{
    struct cp_main *m=ctx;
    if (!m->engine_ok) return -EAGAIN;
    *info=m->engine; return 0;
}
static int network_query(void *ctx, struct ciuki_net_state *state)
{
    (void)ctx; int fd=open("/dev/net",O_RDONLY|O_CLOEXEC);
    if (fd<0) return -errno;
    *state=(struct ciuki_net_state){.version=CIUKI_NET_VERSION,.size=sizeof(*state)};
    int r=ciuki_devctl(fd,NET_GET_STATE,state,sizeof(*state));
    close(fd); return r;
}
static int network_begin(void *ctx,unsigned op,const struct ciuki_net_control *record,const char *name)
{ return network_worker_begin(&((struct cp_main *)ctx)->network_worker,op,record,name); }
static int network_poll(void *ctx,struct cp_network_result *result)
{ return network_worker_poll(&((struct cp_main *)ctx)->network_worker,result); }
static int log_read(void *ctx,uint32_t offset,char *text,size_t cap,size_t *length,uint32_t *total)
{
    /* sec-sysvc: the boot log is a system log; administrators read it
     * through the SYSTEM log service (Event Viewer model). ENOSYS (an older
     * kernel) or ENODEV (no system volume for the service): read the file. */
    size_t got=0; uint32_t size=0;
    if (cap>CIUKI_SYSTEM_LOG_DATA_MAX) cap=CIUKI_SYSTEM_LOG_DATA_MAX;
    if (!ciuki_system_log_read(CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",offset,text,cap,&got,&size)) {
        *length=got; *total=size; return 0;
    }
    if (errno!=ENOSYS && errno!=ENODEV) return -errno;
    (void)ctx; int fd=open(CP_LOG_FILE,O_RDONLY|O_CLOEXEC);
    if (fd<0) return -errno;
    struct stat st;
    if (fstat(fd,&st)) { int error=errno; close(fd); return -error; }
    if (st.st_size<0 || (uint64_t)st.st_size>UINT32_MAX) { close(fd); return -EFBIG; }
    *total=(uint32_t)st.st_size; *length=0;
    size_t wanted=offset<*total ? *total-offset : 0;
    if (wanted>cap) wanted=cap;
    while (*length<wanted) {
        ssize_t got=pread(fd,text+*length,wanted-*length,(off_t)offset+(off_t)*length);
        if (got<0 && errno==EINTR) continue;
        if (got<0) { int error=errno; close(fd); return -error; }
        if (!got) break;
        *length+=(size_t)got;
    }
    return close(fd) ? -errno : 0;
}
static int open_applet(void *ctx,int page)
{
    struct cp_main *m=ctx;struct ciuki_message request;
    if (m->applet_serial) return -EBUSY;
    if (!++m->serial) ++m->serial;
    int r=cp_applet_message(&request,m->serial,page);
    if (!r) r=du_window_queue(&m->window,&request);
    if (!r) m->applet_serial=m->serial;
    return r;
}
static int open_power(void *ctx) { return apply(ctx,DESK_SETTINGS_OPEN_POWER,0,0); }
static int read_power_status(void *ctx, struct ciuki_power_status *out)
{ (void)ctx; return ciuki_power_status(out) ? -errno : 0; }
static int capture_caps(void *ctx,struct ciuki_audio_capture *out)
{
    (void)ctx; int fd=open("/dev/audio",O_RDONLY|O_CLOEXEC);
    if (fd<0) return -errno;
    *out=(struct ciuki_audio_capture){.version=1,.size=sizeof(*out)};
    int error=ciuki_devctl(fd,AUDIO_CAPTURE_CAPS,out,sizeof(*out)) ? -errno : 0;
    close(fd); return error;
}
static int audio_line(void *ctx,struct ciuki_mixer_line *line,int set)
{
    struct cp_main *m=ctx;
    if (!set) {
        int fd=open("/dev/mixer",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
        int result=ciuki_devctl(fd,MIXER_GET_LINE,line,sizeof(*line)) ? -errno : 0;
        close(fd); return result;
    }
    struct ciuki_message request; desk_message(&request,DESK_MIXER_SET,28);
    desk_put32(request.data+4,++m->serial); desk_put32(request.data+8,line->left);
    desk_put32(request.data+12,line->right); desk_put32(request.data+16,line->mute);
    desk_put32(request.data+20,line->line); desk_put32(request.data+24,0);
    return du_window_queue(&m->window,&request) ? -errno : 0;
}
static int multimedia_open(void *ctx,unsigned which)
{
    struct cp_main *m=ctx;
    static const char *const names[]={"mixer","recorder","music"};
    if (which>=3) return -EINVAL;
    unsigned n=(unsigned)strlen(names[which]); struct ciuki_message request;
    desk_message(&request,DESK_LAUNCH,16+n);
    desk_put32(request.data+4,++m->serial); desk_put32(request.data+8,n);
    memcpy(request.data+12,names[which],n); desk_put32(request.data+12+n,0);
    return du_window_queue(&m->window,&request) ? -errno : 0;
}
static int read_power_events(void *ctx, struct ciuki_power_events *out)   /* G11 */
{ (void)ctx; return ciuki_power_events(out) ? -errno : 0; }

static int audio_query(void *ctx, struct ciuki_audio_diagnostics *out)
{
    (void)ctx;
    int fd = open("/dev/mixer", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    *out = (struct ciuki_audio_diagnostics){ .version=1, .size=sizeof(*out) };
    int r = ciuki_devctl(fd, MIXER_GET_DIAGNOSTICS, out, sizeof(*out));
    int error = errno;
    close(fd);
    return r ? -error : 0;
}
static void on_message(struct du_window *w, const struct desk_packet *p, void *user)
{
    (void)w;
    struct cp_main *m = user;
    if ((p->opcode==DESK_GFX_INFO || p->opcode==DESK_GFX_INFO_ENGINE) && p->serial==m->engine_serial) {
        m->engine_serial=0; m->engine_ok=!p->result; m->engine=p->accelerator; m->generic_engine=p->engine;
        cp_engine_refresh(&m->app);
        struct du_widget *line=du_get(&m->ui,974);
        if (line) du_set_text(line,m->app.engine_line);
        return;
    }
    if (p->opcode==DESK_TASK_RESULT && (p->request==DESK_GFX_QUERY || p->request==DESK_GFX_QUERY_ENGINE) && p->serial==m->engine_serial) {
        m->engine_serial=0; m->engine_ok=0; m->engine_next=du_now()+2000000000ull;
        cp_engine_refresh(&m->app); return;
    }
    if (p->opcode==DESK_JOYSTICK_RESULT && p->serial==m->joystick_serial) {
        m->joystick_serial=0; cp_joystick_result(&m->app,&p->joystick,p->result); return;
    }
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && m->audio_serial && p->serial == m->audio_serial) {
        m->audio_serial = 0; cp_audio_result(&m->app, p->result); return;
    }
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && m->display_serial && p->serial == m->display_serial) {
        m->display_serial = 0; cp_display_result(&m->app, p->result); return;
    }
    if (p->opcode==DESK_TASK_RESULT && p->request==DESK_LAUNCH && m->applet_serial && p->serial==m->applet_serial) {
        m->applet_serial=0;
        if (p->result<0) { struct du_widget *s=du_get(&m->ui,3);
            if (s) { du_set_text(s,"The applet could not be opened.");du_invalidate(&m->ui,s->box); }
        }
        return;
    }
    if(p->opcode==DESK_TASK_RESULT && p->request==DESK_LAUNCH && p->serial==m->install_serial){
        m->install_serial=0;struct du_widget *s=du_get(&m->ui,3);
        if(s){du_set_text(s,p->result<0 ? "The installer could not be started. Check the selected media and try again." : "Installer started.");du_invalidate(&m->ui,s->box);}
        return;
    }
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && p->result < 0) {
        struct du_widget *s = du_get(&m->ui, 3);   /* the status line */
        if (s) { du_set_text(s, p->result == -ENODEV ? "No writable mixer: the volume is read-only." : "The desktop refused the change."); du_invalidate(&m->ui,s->box); }
    }
}
static uint64_t monotonic(void *ctx) { (void)ctx; return du_now(); }
static uint64_t now(void *ctx) { (void)ctx; struct timespec t; return clock_gettime(CLOCK_REALTIME, &t) || t.tv_sec < 0 ? 0 : (uint64_t)t.tv_sec; }
/* G1: Date/Time Properties through syscall 97 (the Control Panel holds the time authority). */
static int time_zone(void *ctx, struct ciuki_time_zone *z, uint32_t op) { (void)ctx; return ciuki_time_zone(z, op) ? -errno : 0; }
static int close_request(struct du_window *w, void *user)
{ (void)w; struct cp_main *m = user;
  if (settings_busy(m)) {
      m->app.close_pending=1; m->app.display_accept=0;
      du_set_text(du_get(&m->ui,3),"Waiting for settings to finish saving..."); du_invalidate_widget(&m->ui,3); return 0;
  }
  remember_size(m); return cp_close(&m->app); }
/* The kernel's debug_write path is a plain klog line ("[user N] ...": serial, console and the protected boot
 * log), not a debug-only sink. The trailing newline is dropped: debug_write maps control bytes to '.'. */
static void raw_log(const char *line)
{
#ifdef __i386__
    size_t n=strlen(line); if (n && line[n-1]=='\n') n--;
    (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line,(uint32_t)n,0,0,0,0);
#else
    fputs(line,stderr);
#endif
}
static void display_log(struct cp_main *m, const char *wait)
{
    struct cp_app *a = &m->app;
    const char *state = a->page != CP_DISPLAY ? "closed" : a->display_tab == CP_BACKGROUND ? "background" :
                        a->display_tab == CP_APPEARANCE ? "appearance" : "settings";
    char thumb[64];
    const char *decode = a->decode_cancelled ? "cancelling" : a->decode_wait ?
        (cp_system_thumbnail(a->decode_path, thumb, sizeof(thumb)) ? "thumbnail" : "user") :
        a->preview_pending == 1 ? "deferred" : a->decode_failed[0] && !strcmp(a->decode_failed, a->cfg.wallpaper) ? "failed" :
        a->image.pixels ? "ready" : "idle";
    if (a->page != CP_DISPLAY && !m->disp_state) return;
    if (m->disp_state == state && m->disp_decode == decode && m->disp_wait == wait && m->disp_done == a->decode_done) return;
    char line[192];
    snprintf(line, sizeof(line), "[control] display state=%s decode=%s ui_wait=%s jobs=%u decode_ms=%u render_ms=%u\n",
             state, decode, wait, a->decode_done, a->decode_ms, a->render_ms);
    raw_log(line);
    m->disp_state = state; m->disp_decode = decode; m->disp_wait = wait; m->disp_done = a->decode_done;
}
static void idle(void *arg)
{
    struct cp_main *m=arg;
    if (m->app.page==CP_DISPLAY || m->app.page==CP_SYSTEM || m->app.page==CP_DEVICES || m->app.page==CP_DRIVERS) m->assets_requested=1;
    if (m->window.configured && m->ui.frame_count) programs_poll(m);
    unsigned icons=du_file_icons_generation();   /* fx-exeicons: program icons the worker published */
    if (icons!=m->icon_generation) { m->icon_generation=icons; cp_uninstall_icons(&m->app); }
    if (m->assets.started && cp_job_ready(&m->assets) && m->ui.capture<0 && !m->ui.dialog.kind) {
        cp_job_finish(&m->assets); m->assets_ready=1;
        m->hwid_pci=m->pci_cache; m->hwid_usb=m->usb_cache;
        if (m->portrait_ready) m->app.host.portrait=m->portrait;
        cp_assets_ready(&m->app);
    } else if (m->assets_requested && !m->assets_ready && !m->assets.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        if (cp_job_start(&m->assets,read_assets,m,notify_completion,m)) m->assets_ready=1;
    }
    if (m->window.configured && m->ui.frame_count) { cp_fonts_poll(&m->app);cp_menu_poll(&m->app); }
    if (m->title_page!=m->app.page) {
        m->title_page=m->app.page;
        const char *title=m->app.page==CP_DISPLAY ? "Display Properties" :
            m->app.page==CP_DEVICES || m->app.page==CP_DRIVERS ? "System" : "Control Panel";
        for (unsigned k=0;k<CP_APPLETS;k++) if (cp_applets[k].page==m->app.page && m->app.page!=CP_DISPLAY) title=cp_applets[k].label;
        du_window_set_title(&m->window,title);
    }
    uint64_t mono=du_now();
    if (m->window.configured && m->app.page==CP_DISPLAY && mono>=m->engine_next && !m->engine_serial && !m->app.mode_state) {
        struct ciuki_message query; desk_message(&query,DESK_GFX_QUERY_ENGINE,12); desk_put32(query.data+8,CIUKI_DISPLAY_ENGINE_VERSION);
        if (!++m->serial) ++m->serial;
        desk_put32(query.data+4,m->serial);
        if (!du_window_queue(&m->window,&query)) m->engine_serial=m->serial;
        m->engine_next=mono+2000000000ull;
    }
    if (cp_job_ready(&m->game_job)) cp_tick(&m->app);
    int error;
    if (settings_result(m,&error)) {
        if (m->reload_pending) {unsigned request=(unsigned)m->reload_pending;m->reload_pending=0; if (!error) apply(m,request,0,0); }
        cp_settings_saved(&m->app,error);
    }
    /* Clocks, uptime, external mixer state and the cached power status
     * (call 73 STATUS: no firmware call) have genuine one-second work.
     * Other pages have no heartbeat. Navigation/close also runs on input and
     * completion, even when the wall clock has not changed. */
    int ticking=m->app.page==CP_DATETIME || m->app.page==CP_SYSTEM || m->app.page==CP_SOUNDS || m->app.page==CP_JOYSTICK ||
        m->app.page==CP_POWER || m->app.page==CP_ACCESS;
    if (ticking && (!m->tick_next || mono>=m->tick_next)) {
        cp_tick(&m->app); m->tick_next=mono+1000000000ull;
    } else if (!ticking) m->tick_next=0;
    if (m->app.close_pending || m->app.navigation_pending) cp_tick(&m->app);
    if (settings_busy(m) && m->app.quit) return;
    if (m->app.quit) { remember_size(m); du_window_close(&m->window); m->app.quit = 0; }
}
__attribute__((weak)) int ciuki_win32_uninstall_list(struct desk_uninstall_entry *out, unsigned cap)
{ (void)out; (void)cap; return -ENOSYS; }
__attribute__((weak)) int ciuki_win32_uninstall_registered(const char *key)
{(void)key;return -ENOSYS;}
__attribute__((weak)) int ciuki_win32_uninstall_run(const char *key)
{ (void)key; return -ENOSYS; }
static int registry_list(void *ctx,struct desk_uninstall_entry *out,unsigned cap)
{(void)ctx;return ciuki_win32_uninstall_list(out,cap);}
static int registry_check(void *ctx,const char *key)
{(void)ctx;return ciuki_win32_uninstall_registered(key);}
static int registry_run(void *ctx,const char *key)
{(void)ctx;return ciuki_win32_uninstall_run(key);}
static int game_program(void *ctx,unsigned game,int remove,uint32_t *size)
{
    (void)ctx;struct ciuki_dosvm_command c={.version=CIUKI_DOSVM_VERSION,.size=sizeof(c)};
    c.launch.game=game;
    if(ciuki_dosvm(remove ? CIUKI_DOSVM_GAME_REMOVE : CIUKI_DOSVM_GAME_QUERY,&c))return -errno;
    *size=c.changed;return (int)c.state;
}
static void program_log(void *ctx,const char *line)
{
    (void)ctx;
#ifdef __i386__
    (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line,(uint32_t)strlen(line),0,0,0,0);
#else
    fputs(line,stderr);
#endif
}
static void forget_icon(void *ctx,const char *label)
{struct cp_main *m=ctx;cfg_icon_forget(&m->app.cfg,label);}
static int program_save(void *ctx)
{
    struct cp_main *m=ctx;char *text=malloc(CFG_TEXT_MAX);if(!text)return -ENOMEM;
    size_t n=cfg_format(&m->app.cfg,text,CFG_TEXT_MAX);
    int result=!n ? -EIO : save(ctx,text,n) ? -EIO : 0;free(text);
    if(result)return result;
    /* The removal identity stays durable until the settings worker commits.
     * This applet's removal already waits for disk/uninstaller completion. */
    int error=0;
    while (!settings_result(m,&error)) {
        struct pollfd fd={.fd=m->completion[0],.events=POLLIN};
        int r=poll(&fd,1,-1);
        if (r<0 && errno==EINTR) continue;
        if (r<0 || fd.revents&(POLLERR|POLLHUP|POLLNVAL)) return -EIO;
        struct ciuki_message token;
        r=ciuki_channel_recv(fd.fd,&token,DONTWAIT);
        if (!r || (r<0 && errno!=EINTR && errno!=EAGAIN)) return -EIO;
    }
    return error ? -error : 0;
}
static struct cp_program_store programs={.root="",.registry_list=registry_list,.registry_run=registry_run,.registry_check=registry_check,.game=game_program,.forget_icon=forget_icon,.persist=program_save};
static void scan_programs(void *ctx)
{
    struct cp_main *m=ctx; struct cp_program_store store=programs;
    m->next_program_count=cp_program_list(&store,m->program_next,CP_PROGRAM_MAX);
}
static void programs_poll(struct cp_main *m)
{
    if (cp_job_ready(&m->program_scan) && m->ui.capture<0 && !m->ui.dialog.kind) {
        cp_job_finish(&m->program_scan);
        m->program_count=m->next_program_count; m->programs_ready=1;
        if (m->program_count>0) memcpy(m->program_cache,m->program_next,(unsigned)m->program_count*sizeof(*m->program_next));
        free(m->program_next); m->program_next=NULL;
        if (m->app.page==CP_UNINSTALL) cp_open(&m->app,CP_UNINSTALL);
        m->programs_requested=0;
    } else if (m->programs_requested && !m->program_scan.started && !m->ui.dirty) {
        m->program_next=calloc(CP_PROGRAM_MAX,sizeof(*m->program_next));
        int error=m->program_next ? cp_job_start(&m->program_scan,scan_programs,m,notify_completion,m) : -ENOMEM;
        if (error) { free(m->program_next); m->program_next=NULL; m->programs_ready=1; m->program_count=error; m->programs_requested=0; }
    }
}
static int win32_uninstall_list(void *ctx,struct desk_uninstall_entry *out,unsigned cap)
{
    struct cp_main *m=ctx; m->programs_requested=1;
    if (!m->programs_ready) return 0;
    if (m->program_count<0) return m->program_count;
    unsigned n=(unsigned)m->program_count<cap ? (unsigned)m->program_count : cap;
    memcpy(out,m->program_cache,n*sizeof(*out)); return (int)n;
}
static int program_remove(void *ctx,const struct desk_uninstall_entry *r)
{
    struct cp_main *m=ctx; if (m->program_scan.started) return -EBUSY;
    programs.ctx=ctx;
    int result=cp_program_remove(&programs,r);
    struct cp_program_store recorder=programs;recorder.log=program_log;
    cp_program_record(&recorder,r,result);return result;
}
static void programs_changed(void *ctx)
{ struct cp_main *m=ctx; m->programs_ready=0; m->programs_requested=1; (void)apply(ctx,DESK_SETTINGS_PROGRAMS,0,0); }
static int program_install(void *ctx,const char *path)
{
    struct cp_main *m=ctx;
    if(!cp_program_installer(path))return -EINVAL;
    struct ciuki_message request;
    unsigned n=(unsigned)strlen(path),t=7;
    desk_message(&request,DESK_LAUNCH,16+t+n);
    desk_put32(request.data+4,++m->serial);desk_put32(request.data+8,t);
    memcpy(request.data+12,"install",t);desk_put32(request.data+12+t,n);
    memcpy(request.data+16+t,path,n);
    int result=du_window_queue(&m->window,&request);if(!result)m->install_serial=m->serial;return result;
}
#include "start_menu_io.inc"
static uint64_t control_deadline(struct cp_main *m)
{
    uint64_t next=du_next_deadline(&m->ui);
    if (m->tick_next && m->tick_next<next) next=m->tick_next;
    if (m->window.configured && m->app.page==CP_DISPLAY && !m->engine_serial && !m->app.mode_state && m->engine_next<next)
        next=m->engine_next;
    return next;
}
/* Input, decoder and all other applet jobs are in the same event wait set. */
static int control_run(struct cp_main *m)
{
    for (;;) {
        int r = du_window_turn(&m->window, du_now());
        if (r <= 0) return r;
        idle(m);
        /* idle may finish a deferred Close. Queue its protocol request before
         * sleeping; du_window_close() itself only changes closing state. */
        if (m->window.closing==1 && m->window.count<16) continue;
        uint64_t mono = du_now(), next = control_deadline(m);
        int timeout = cp_display_wait_ms(mono, next);
        if (m->ui.dirty && m->window.configured && !m->window.swap_sent && !m->window.present_pending && m->window.count < 16) continue;
        struct pollfd fds[3] = {
            {.fd = m->window.channel, .events = POLLIN | (m->window.count ? POLLOUT : 0)},
            {.fd = cp_decoder_fd(), .events = POLLIN},
            {.fd = m->completion[0], .events = POLLIN}
        };
        display_log(m, next != UINT64_MAX ? "timer" : "event");
        r = poll(fds, 3, timeout);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if ((fds[0].revents | fds[1].revents | fds[2].revents) & POLLNVAL) { errno = EBADF; return -1; }
        if (fds[1].revents & POLLIN) cp_preview_ready(&m->app);
        if ((fds[1].revents | fds[2].revents) & (POLLERR | POLLHUP)) { errno = EIO; return -1; }
        if (fds[2].revents & POLLIN) {
            struct ciuki_message token;
            /* One token is enough; further queued publications stay readable.
             * No producer can race a reset or lose the transition to sleep. */
            r=ciuki_channel_recv(fds[2].fd,&token,DONTWAIT);
            if (!r) { errno=EIO;return -1; }
            if (r<0 && errno!=EAGAIN && errno!=EINTR) return -1;
        }
    }
}

#include "internet_history.h"
static int internet_clear(void *ctx)
{ (void)ctx;return internet_history_clear("/Documents/NetSurf"); }
static int internet_read(void *ctx,struct cp_internet_options *o)
{ (void)ctx;return internet_load(INTERNET_OPTIONS_PATH,o); }
static int internet_write(void *ctx,const struct cp_internet_options *o)
{ (void)ctx;return internet_save(INTERNET_OPTIONS_PATH,o); }
static int internet_default(void *ctx)
{ (void)ctx;fm_registry_exchange=desk_w32_exchange;return fm_browser_default(); }
static int internet_programs(void *ctx)
{
    struct cp_main *m=ctx;struct ciuki_message request;
    const char *name="files";char arg[40];snprintf(arg,sizeof(arg),"--file-action=%d:",FILES_TYPES);
    unsigned t=strlen(name),n=strlen(arg);
    desk_message(&request,DESK_LAUNCH,16+t+n);desk_put32(request.data+4,++m->serial);
    desk_put32(request.data+8,t);memcpy(request.data+12,name,t);
    desk_put32(request.data+12+t,n);memcpy(request.data+16+t,arg,n);
    return du_window_queue(&m->window,&request);
}
int main(int argc, char **argv)
{
    int channel = 3, page = CP_HOME;
    const char *set_wallpaper=NULL;
    unsigned game=0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            channel = (int)n;
        } else if (!strncmp(argv[i], "--panel=", 8)) page = cp_page_index(argv[i] + 8);
        else if (!strncmp(argv[i],"--set-wallpaper=",16)) { set_wallpaper=argv[i]+16; page=CP_DISPLAY; }
        else if (!strncmp(argv[i],"--game=",7)) {
            char *end;unsigned long n=strtoul(argv[i]+7,&end,10);
            if (!argv[i][7] || *end || !n || n>DESK_GAME_COUNT) return 2;
            game=(unsigned)n;page=CP_JOYSTICK;
        } else return 2;
    }
    struct cp_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (completion_start(m)) { free(m); return 1; }
    snprintf(m->settings_temp,sizeof(m->settings_temp),"%s.cpl",profile_settings());
    m->writer.notify=notify_completion; m->writer.notify_ctx=m;
    m->writer_started=!cfg_writer_start(&m->writer,profile_settings(),m->settings_temp);
    struct cp_host *h = &m->app.host;
    h->user_begin=user_begin; h->user_poll=user_poll;
    h->menu_begin=menu_begin_io;h->menu_poll=menu_poll_io;h->menu_explore=menu_explore;
    h->pack_scan=pack_scan; h->pack_preview=pack_preview;
    h->pack_install=pack_install; h->pack_remove=pack_remove;
    h->startup_list=startup_list; h->startup_enable=startup_enable;
    h->uninstall_list=win32_uninstall_list; h->program_remove=program_remove;
    h->program_install=program_install;h->programs_changed=programs_changed;
    h->sound_validate=sound_validate; h->sound_schemes=sound_schemes; h->sound_scheme=sound_scheme;
    h->load_sounds = load_sounds; h->save_sounds = save_sounds; h->load = load; h->save = save; h->wallpapers = wallpapers; h->decode_begin = decode_begin; h->decode_poll = decode_poll; h->decode_cancel = decode_cancel; h->mixer = mixer; h->apply = apply; h->now = now; h->time_zone = time_zone; h->ctx = m;
    h->display = display_query;
    h->internet_clear_history=internet_clear;h->internet_load=internet_read;h->internet_save=internet_write;
    h->internet_default=internet_default;h->internet_programs=internet_programs;
    h->display = display_query; h->monitor=monitor_query; h->refresh_rates=refresh_query;
    h->network_begin=network_begin; h->network_poll=network_poll;
    h->engine=engine_query; h->engine_info=generic_engine_query; h->accel_retry=accel_retry;
    h->network=network_query; h->power=open_power; h->power_status=read_power_status; h->power_events=read_power_events; h->log_read=log_read;
    h->open_applet=open_applet; h->network=network_query; h->power=open_power; h->power_status=read_power_status; h->log_read=log_read;
    h->autoplay_list=autoplay_list; h->autoplay_forget=autoplay_forget;
    h->save_async=1; h->fonts_async=1; h->job_notify=notify_completion;
    h->audio_diagnostics = audio_query; h->capture_caps=capture_caps; h->audio_line=audio_line; h->multimedia_open=multimedia_open;
    h->game_begin=game_begin;h->game_poll=game_poll_result;
    h->joystick = joystick_request; h->joystick_save_game = joystick_save_game; h->devices = devices; h->firmware = firmware; h->hwid_name = hwid_name; h->usb_hwid_name = usb_hwid_name; h->monotonic = monotonic; h->boot_load = boot_load; h->boot_save = boot_save;
    h->hw_load = hw_load; h->hw_save = hw_save; h->boot_serial = boot_serial; h->perf_load = perf_load; h->perf_save = perf_save; h->memory = memory_query; h->driver_catalog = driver_catalog;

    const char *display = getenv("CIUKI_DISPLAY");
    h->screen_w = 1024; h->screen_h = 768;
    if (display) { char *end; long w = strtol(display, &end, 10); if (*end == 'x' && w >= 320 && w <= 2048) { h->screen_w = (uint32_t)w; h->screen_h = (uint32_t)strtol(end + 1, NULL, 10); } }
    struct utsname u;
    if (!uname(&u)) { snprintf(h->version, sizeof(h->version), "%s", u.version); snprintf(h->build, sizeof(h->build), "%s", u.release); snprintf(h->machine, sizeof(h->machine), "%s", u.machine); }
    static uint8_t buffer[CIUKI_PROC_LIST_MIN + CIUKI_PROCESS_MAX * sizeof(struct ciuki_proc_record)];
    if (ciuki_proc_list(buffer, sizeof(buffer)) > 0) {
        struct ciuki_proc_system sys; memcpy(&sys, buffer, sizeof(sys));
        if (sys.version == CIUKI_PROC_VERSION) { h->mem_total_k = (uint64_t)sys.usable_pages * 4u; h->mem_free_k = (uint64_t)sys.free_pages * 4u; h->processes = sys.processes; }
    }
    int width,height; panel_size(&width,&height);
    if (width>(int)h->screen_w-16) width=CP_WIDTH;
    if (height>(int)h->screen_h-100) height=CP_HEIGHT;
    m->saved_w=width; m->saved_h=height; m->title_page=-99;
    if ((page!=CP_DISPLAY && page!=CP_ADAPTER && page!=CP_FONTS ? du_window_open_resizable(&m->window, &m->ui, channel, width, height, "Control Panel", CP_WIDTH, CP_HEIGHT) :
         du_window_open(&m->window, &m->ui, channel, CP_WIDTH, CP_HEIGHT, "Control Panel"))) {
        fprintf(stderr, "Control Panel: desktop connection failed: %s\n", strerror(errno));
        if (m->writer_started) cfg_writer_stop(&m->writer);
        completion_stop(m); free(m->hwid_pci); free(m->hwid_usb); free(m); return 1;
    }
    du_help_topic(&m->ui, "control");   /* G15: F1 opens this program's Help topic */
    m->window.message = on_message; m->window.close_request = close_request; m->window.user = m;
    if (cp_attach(&m->app, &m->ui, game ? CP_HOME : page)) {
        du_window_destroy(&m->window); if (m->writer_started) cfg_writer_stop(&m->writer); completion_stop(m); free(m->hwid_pci); free(m->hwid_usb); free(m); return 1;
    }
    if (game) cp_game_open(&m->app,game);
    if (set_wallpaper && cp_wallpaper_set(&m->app,set_wallpaper)) fprintf(stderr,"control: wallpaper path is not a supported image\n");
    if (du_file_icons_start(m->ui.wake, m->ui.wake_ctx)) fprintf(stderr,"control: icon worker unavailable\n");   /* fx-exeicons */
    int r = control_run(m);
    du_file_icons_stop();
    cp_decoder_stop();
    cp_job_finish(&m->users_worker.job);
    cp_job_finish(&m->menu_job);
    cp_job_finish(&m->game_job);
    cp_job_finish(&m->program_scan); free(m->program_next);
    cp_job_finish(&m->assets);
    if (!m->assets_ready) { free(m->pci_cache); free(m->usb_cache); }
    if (m->writer_started) cfg_writer_stop(&m->writer);
    network_worker_stop(&m->network_worker);
    cp_detach(&m->app); completion_stop(m); du_window_destroy(&m->window); free(m->hwid_pci); free(m->hwid_usb); free(m);
    return r < 0 ? 1 : 0;
}
