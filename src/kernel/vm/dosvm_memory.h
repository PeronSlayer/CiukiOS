/* Per-VM XMS/EMS/UMB state. Guest addresses, never host physical addresses.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_MEMORY_H
#define CIUKI_DOSVM_MEMORY_H
#define VM_POOL_BASE 0x110000u
#define VM_POOL_CAP (24u * 1024u * 1024u)
#define VM_POOL_PAGES (VM_POOL_CAP / PAGE_SIZE)
#define VM_MEM_HANDLES 64u
#define VM_EMS_PAGE 16384u
#define VM_EMS_CAP (8u * 1024u * 1024u)
#define VM_XMS_SEG 0xC800u
#define VM_UMB_FIRST 0xC9000u
#define VM_UMB_END 0xE0000u
#define VM_EMS_CALLS 8u
struct vm_block { uint32_t base, bytes, pages; bool used; uint8_t locks; };
struct vm_map { uint16_t handle, page; uint32_t serial; };
struct vm_ems_handle {
    struct vm_block block;
    uint32_t serial;
    char name[8];
    bool saved;
    struct vm_map map[4];
};
struct vm_ems_call { struct vm_map restore[4]; uint16_t ss, sp; };
struct vm_memory {
    uint32_t quota, used_pages, ems_bytes, serial;
    uint8_t occupied[VM_POOL_PAGES];
    struct vm_block xms[VM_MEM_HANDLES];
    struct vm_ems_handle ems[VM_MEM_HANDLES];
    struct vm_map map[4];
    struct vm_ems_call calls[VM_EMS_CALLS];
    unsigned depth;
    uint16_t umb[40]; /* allocation length at first page, continuation=FFFFh */
    uint16_t frame_segment, local_a20, global_a20;
    bool hma, umb_enabled;
};
int dosvm_memory_init(struct dosvm *, const struct dosvm_config *);
void dosvm_memory_close(struct dosvm *);
void dosvm_page_remove(struct dosvm *, uint32_t);
bool dosvm_pool_alloc(struct dosvm *, struct vm_block *, uint32_t);
bool dosvm_pool_resize(struct dosvm *, struct vm_block *, uint32_t);
void dosvm_pool_free(struct dosvm *, struct vm_block *);
void dosvm_pool_query(struct dosvm *, uint32_t *, uint32_t *);
int dosvm_copy_region(struct dosvm *, uint32_t, uint32_t, uint32_t, bool);
int dosvm_xms(struct dosvm *, struct v86_frame *);
uint8_t dosvm_umb(struct dosvm *, struct v86_frame *, unsigned);
int dosvm_ems(struct dosvm *, struct v86_frame *);
int dosvm_ems_return(struct dosvm *, struct v86_frame *);
static inline uint16_t vm_u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t vm_u32(const uint8_t *p) { return vm_u16(p) | ((uint32_t)vm_u16(p+2) << 16); }
static inline void vm_w16(uint8_t *p, uint16_t x) { p[0]=x; p[1]=x>>8; }
static inline void vm_w32(uint8_t *p, uint32_t x) { vm_w16(p,x); vm_w16(p+2,x>>16); }
static inline void vm_low(uint32_t *p, uint16_t x) { *p=(*p&0xFFFF0000u)|x; }
static inline uint32_t vm_far(uint32_t x) { return (x>>16)*16+(uint16_t)x; }
static inline uint32_t vm_ptr(uint32_t seg, uint32_t off) { return (seg&0xFFFFu)*16+(off&0xFFFFu); }
/* Structures must not cross the caller's 64 KiB segment limit. */
static inline bool vm_buffer(struct dosvm *v, uint32_t seg, uint32_t off, unsigned n, bool wr)
{ return seg<=0xFFFF && n<=0x10000u-(uint16_t)off && dosvm_range(v,vm_ptr(seg,off),n,wr); }
#endif
