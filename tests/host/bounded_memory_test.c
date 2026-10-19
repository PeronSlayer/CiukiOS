/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual VMM/uaddr loops; synthetic physical frame IDs avoid a 3 GiB host
 * allocation when walking the architectural maximum. No policy is copied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#define CIUKI_CPU_H
#define KERNEL_VBASE 0xc0000000u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"bounded-memory:%u %s\n",__LINE__,#x); abort(); } } while (0)
static uint32_t tables[769][1024];
static uint8_t freed[786432+1024];
static uint64_t total,work_span,maximum;
static bool points;
static unsigned releases;
static void (*on_point)(void);
static void charge(uint64_t ns) { total+=ns; work_span+=ns; }
static void boundary(void) { if (work_span>maximum) maximum=work_span; work_span=0; }
static void *host_physical(uint32_t pa) { CHECK(pa>=4096 && pa<770*4096); return (uint8_t *)tables+pa-4096; }
#define P2V(p) host_physical(p)
static uint32_t read_cr3(void) { return 0; }
static void write_cr3(uint32_t p) { (void)p; }
static void invlpg(uint32_t p) { (void)p; }
void kpreempt_point(void) { if (points) { charge(200); boundary(); if (on_point) on_point(); } }
void pmm_free(uint32_t pa) { unsigned p=pa/4096; CHECK(p<sizeof(freed) && !freed[p]); freed[p]=1; releases++; charge(6000); }
uint32_t pmm_alloc(void) { abort(); }
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1,n); }
void kfree(void *p) { free(p); }
void ww_cancel(void *p,uint64_t g,uint32_t b,uint32_t e) { (void)p;(void)g;(void)b;(void)e; }
void panic(const char *fmt,...) { fprintf(stderr,"panic %s\n",fmt); abort(); }
struct task *g_current;
static uint32_t kpd[1024], kpd_phys;
static uint32_t *table_va(uint32_t p) { return P2V(p); }
uint32_t g_user_mappings;
#include "bounded_aspace.inc"
#include "bounded_uaddr.inc"
static struct uaddr *walking_owner;
static void mapping_during_walk(void)
{
    on_point=0;
    CHECK(ua_map_at(walking_owner,CIUKI_MMAP_BASE,PAGE_SIZE,PROT_NONE,
                   PROT_READ,UA_ANON,0)==-EBUSY);
}
static struct uaddr setup(unsigned pages,bool own)
{
    memset(tables,0,sizeof(tables)); memset(freed,0,sizeof(freed));
    releases=0; total=work_span=maximum=0;
    struct uaddr u={.as={.pd_phys=4096,.pages=pages,.tables=768,.owner=MM_USER_PRIVATE},.backing=pages};
    for (unsigned i=0;i<768;i++) {
        tables[0][i]=(i+2)*4096|PTE_P|PTE_W|PTE_U;
        for (unsigned j=0;j<1024 && i*1024+j<pages;j++)
            tables[i+1][j]=(770+i*1024+j)*4096|PTE_P|PTE_W|PTE_U|(own ? PTE_OWN : 0);
    }
    g_user_mappings=pages; return u;
}
int main(void)
{
    /* Every PDE and PTE, including empty tables at the high end; the frame
     * maximum is synthetic, with real 512 MiB/24 MiB cases below. */
    const unsigned counts[]={6144,131072,786432};
    for (unsigned count=0;count<3;count++) for (unsigned site=0;site<4;site++) {
        uint64_t before=0,oldtotal=0;
        for (unsigned enabled=0;enabled<2;enabled++) {
            struct uaddr u=setup(counts[count],site!=0); points=enabled;
            if (site==0) as_destroy(&u.as);
            if (site==1) free_pages(&u,0,CIUKI_MAIN_STACK_LIMIT,false);
            if (site==2) protect_pages(&u,0,CIUKI_MAIN_STACK_LIMIT,PROT_READ);
            if (site==3) {
                /* Existing backing used to skip all reserve checkpoints. */
                CHECK(!reserve_pages(&u,0,counts[count]*4096));
            }
            boundary();
            if (!enabled) { before=maximum; oldtotal=total; }
            else {
                CHECK(maximum<=2000000 && total<=oldtotal*110/100);
                printf("F0-44 memory site=%u pages=%u work_span=%llu->%lluns work=%llu->%lluns PASS\n",site,counts[count],
                    (unsigned long long)before,(unsigned long long)maximum,(unsigned long long)oldtotal,(unsigned long long)total);
            }
            if (site==0) CHECK(!u.as.pd_phys && !g_user_mappings && releases==counts[count]+769);
            if (site==1) CHECK(!u.backing && !u.as.tables && !g_user_mappings && releases==counts[count]+768);
            if (site==2) CHECK(releases==0 && u.backing==counts[count]);
        }
    }
    /* Win32 can reserve an extent per VA page; there is no 256-extent
     * cap outside the native mmap arena. Exercise that architectural limit. */
    struct ua_extent *nodes=calloc(786432,sizeof(*nodes)); CHECK(nodes);
    struct uaddr u={.head=nodes};
    for (unsigned i=0;i<786432;i++) {
        nodes[i]=(struct ua_extent){.base=i*PAGE_SIZE,.end=(i+1)*PAGE_SIZE,
            .next=i+1<786432 ? &nodes[i+1] : 0};
    }
    for (unsigned site=0;site<2;site++) {
        uint64_t before=0,oldtotal=0;
        for (unsigned enabled=0;enabled<2;enabled++) {
            total=work_span=maximum=0; points=enabled;
            if (!site) CHECK(ua_find(&u,CIUKI_MAIN_STACK_LIMIT-1)==&nodes[786431]);
            else CHECK(vacant(&u,CIUKI_MAIN_STACK_LIMIT,CIUKI_MAIN_STACK_LIMIT+PAGE_SIZE));
            boundary(); CHECK(!u.pins);
            if (!enabled) { before=maximum;oldtotal=total; }
            else {
                CHECK(maximum<=2000000 && total<=oldtotal*110/100);
                printf("F0-44 extent walk site=%u records=786432 span=%llu->%lluns work=%llu->%lluns PASS\n",site,
                    (unsigned long long)before,(unsigned long long)maximum,(unsigned long long)oldtotal,(unsigned long long)total);
            }
        }
    }
    nodes[255].next=0; walking_owner=&u; points=true;
    on_point=mapping_during_walk;
    CHECK(ua_find(&u,255*PAGE_SIZE)==&nodes[255] && !on_point && !u.pins);
    puts("F0-44 metadata service: concurrent insertion refused, no leaked walk pin PASS");
    free(nodes);
    return 0;
}
