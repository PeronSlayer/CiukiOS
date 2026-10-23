/* Production Device Manager Properties and Control Panel 2D identity.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include "../../apps/control/app.c"
static char message[DU_DIALOG_TEXT];
static struct cp_app app;
static struct du_ui ui;
void du_message(struct du_ui *u,int id,const char *title,const char *text,int icon,
                const char *const *buttons,unsigned count,int def,int cancel)
{
    (void)title; (void)icon; (void)buttons; (void)def; (void)cancel;
    assert(u==&ui && id==D_PROPERTIES && count==3);
    snprintf(message,sizeof(message),"%s",text);
}
void du_dialog_icon(struct du_ui *u,const uint32_t *argb) { assert(u==&ui); (void)argb; }
const uint32_t *du_icon_argb(const char *name,unsigned size) { (void)name; assert(size==32); return NULL; }
const char *du_icon_name(const char *object) { return object; }
struct du_widget *du_get(struct du_ui *u,int id) { assert(u==&ui && id==974); return NULL; }
void du_set_text(struct du_widget *w,const char *s) { (void)w; (void)s; assert(!"unexpected widget"); }
void du_invalidate(struct du_ui *u,struct du_rect r) { (void)u; (void)r; assert(!"unexpected invalidate"); }
/* The rest of the Control Panel links against libdeskui; this test drives only
 * Properties and the engine line, so any other widget, dialog, timer or text
 * area call is a failure (the settings and sound tables are linked for real). */
#define UNEXPECTED(what) do { assert(!"unexpected " what); abort(); } while (0)
struct du_widget *du_add(struct du_ui *u,enum du_kind k,int id,struct du_rect r,const char *t)
{ (void)u; (void)k; (void)id; (void)r; (void)t; UNEXPECTED("du_add"); }
void du_invalidate_widget(struct du_ui *u,int id) { (void)u; (void)id; UNEXPECTED("du_invalidate_widget"); }
void du_release(struct du_ui *u) { (void)u; UNEXPECTED("du_release"); }
void du_list_ensure(struct du_widget *w,unsigned index) { (void)w; (void)index; UNEXPECTED("du_list_ensure"); }
void du_dialog_close(struct du_ui *u) { (void)u; UNEXPECTED("du_dialog_close"); }
int du_dialog_id(const struct du_ui *u) { (void)u; UNEXPECTED("du_dialog_id"); }
int du_timer_start(struct du_ui *u,int id,uint64_t ns,int repeat) { (void)u; (void)id; (void)ns; (void)repeat; UNEXPECTED("du_timer_start"); }
void du_timer_stop(struct du_ui *u,int id) { (void)u; (void)id; UNEXPECTED("du_timer_stop"); }
int du_theme_use(struct du_ui *u,const char *name) { (void)u; (void)name; UNEXPECTED("du_theme_use"); }
int du_ta_init(struct du_textarea *t,size_t cap) { (void)t; (void)cap; UNEXPECTED("du_ta_init"); }
int du_ta_set_text(struct du_textarea *t,const char *s,size_t n) { (void)t; (void)s; (void)n; UNEXPECTED("du_ta_set_text"); }
const char *desk_theme_preference(const char *name) { (void)name; UNEXPECTED("desk_theme_preference"); }
static int inventory_query(void *ctx,struct ciuki_devices_info *out)
{ (void)ctx; *out=app.devices; return 0; }
int main(void)
{
    app.ui=&ui; app.devices_ok=1; app.devices.count=1; app.properties_device=0;
    struct ciuki_device_record *d=&app.devices.devices[0];
    d->id=CIUKI_DEVICE_ACCELERATOR_ID; d->class_id=CIUKI_DEVICE_DISPLAY; d->bus_type=CIUKI_BUS_LOADER;
    strcpy(d->name,"2D accelerator"); strcpy(d->driver,"trident");
    strcpy(d->driver_display_name,"Trident Blade / CyberBlade");
    properties(&app,0);
    assert(strstr(message,"Driver: Trident Blade / CyberBlade\n"));
    app.host.devices=inventory_query;
    d->accel_ready=1; strcpy(d->reason,"active");
    cp_engine_refresh(&app);
    assert(strstr(app.engine_line,"Trident Blade / CyberBlade: active"));
    d->state=CIUKI_DEVICE_QUARANTINED; d->accel_ready=0; strcpy(d->reason,"engine_timeout");
    cp_engine_refresh(&app);
    assert(strstr(app.engine_line,"Trident Blade / CyberBlade: quarantined (engine_timeout)"));
    d->driver_display_name[0]=0; properties(&app,0); assert(strstr(message,"Driver: trident\n"));
    d->driver[0]=0; properties(&app,0); assert(strstr(message,"Driver: None bound\n"));
    puts("Device Manager Properties and Control Panel 2D identity: display name, active/quarantined state, short-id fallback and unbound identity PASS");
}
