/* Software-only adapter boundary for the independent mode-service fixture. */
bool display_accel_disabled(const char **reason)
{
    if (reason) *reason="software";
    return !!(g_boot.flags & CBI_F_SAFE_MODE);
}
const struct display_driver *display_driver_bind(const struct fb_device *fb) { (void)fb; return NULL; }
const struct pci_func *display_driver_owner(void) { return NULL; }
int display_attempt_prepare(struct vfs *vfs,const struct display_driver *d,const struct pci_func *p,bool *retained)
{ (void)vfs; (void)d; (void)p; *retained=false; return -ENODEV; /* never bound */ }
void display_driver_software(const char *reason) { (void)reason; }
void display_driver_record(void) { }
bool display_driver_refresh(void) { return false; }
void display_driver_count(unsigned operation,bool native) { (void)operation; (void)native; }
void display_driver_command_count(unsigned opcode,bool native) { (void)opcode; (void)native; }
void display_driver_command_counters(struct ciuki_command_info *out) { (void)out; }
void display_driver_info(struct ciuki_display_engine_info *out)
{
    memset(out,0,sizeof(*out)); out->version=1; out->size=sizeof(*out);
    strcpy(out->driver_id,"swdib"); strcpy(out->display_name,"Software renderer");
    out->kind=CIUKI_ENGINE_SOFTWARE; out->software_operations=15; out->cpu_ready=1;
}
