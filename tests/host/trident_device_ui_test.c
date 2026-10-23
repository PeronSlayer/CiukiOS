/* Production Device Manager Properties and Control Panel 2D identity.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
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
