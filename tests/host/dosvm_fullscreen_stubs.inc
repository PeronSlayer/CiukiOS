/* The guest instruction runner has no physical VGA or desktop. */
void dosvm_fullscreen_switch(const struct task *t) { (void)t; }
void dosvm_fullscreen_stopping(struct dosvm *v) { (void)v; }
void dosvm_fullscreen_checkpoint(struct dosvm *v) { (void)v; }
void dosvm_fullscreen_progress(struct dosvm *v,uint32_t c,uint32_t i) { (void)v;(void)c;(void)i; }
int dosvm_fullscreen_leave(struct dosvm_key k,bool b) { (void)k;(void)b;return 0; }
void dosvm_fullscreen_model_begin(struct dosvm *v) { (void)v; }
void dosvm_fullscreen_model_end(struct dosvm *v) { (void)v; }
void dosvm_fullscreen_mode_changed(struct dosvm *v) { (void)v; }
bool dosvm_fullscreen_aperture(struct dosvm *v,uint32_t l,unsigned n,bool w)
{ (void)v;(void)l;(void)n;(void)w;return false; }
bool dosvm_fullscreen_pinned(const struct dosvm *v) { return v->fullscreen_hold; }
bool dosvm_fullscreen_service(void) { return false; }
bool display_fullscreen_supported(uint16_t mode) { (void)mode;return false; }
int display_fullscreen_write(uint32_t off,const void *p,uint32_t n)
{ (void)off;(void)p;(void)n;return -EOPNOTSUPP; }
int display_fullscreen_lfb(struct display_lfb *out)
{ (void)out;return -EOPNOTSUPP; }
bool vmm_cache_alias_ok(uint32_t physical,uint32_t bytes,uint32_t flags)
{ (void)physical;(void)bytes;(void)flags;return false; }
void framebuffer_write_drain(void) {}
int dosvm_fullscreen_port(uint16_t p,unsigned n,bool w,uint32_t *v)
{ (void)p;(void)n;(void)w;(void)v; abort(); }
