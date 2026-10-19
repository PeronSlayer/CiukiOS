"""Generate a deterministic replay of the production eager-backing loop.

Only allocate/zero/map and scheduler boundaries are modeled. The complete
lifecycle, mappings, admission and rollback run in dosvm_test.c as well.
Costs are calibrated to the directive's 110 ms / 6144 pages, not host time.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def write_model(destination):
    source = (ROOT / 'src/kernel/vm/dosvm_xms.c').read_text()
    start = source.index('    for (uint32_t off=0;', source.index('int dosvm_memory_init('))
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    loop = source[start:end]
    memory = (ROOT / 'src/kernel/vm/dosvm_memory.h').read_text()
    cap = re.search(r'^#define VM_POOL_CAP (.+)$', memory, re.M)[1]
    Path(destination).write_text(r'''
/* Generated from dosvm_memory_init: do not replace with a parallel loop. */
struct creation_result { uint64_t ns, span, maximum; unsigned pages, points; };
static struct creation_result creation;
static bool creation_points;
static void (*creation_service)(uint64_t);
static uint8_t creation_page[PAGE_SIZE];
static void creation_boundary(void)
{
    if (creation.span > creation.maximum) creation.maximum = creation.span;
    if (creation_service && creation.span) creation_service(creation.span);
    creation.span = 0;
}
static void creation_charge(uint64_t ns)
{ creation.ns += ns; creation.span += ns; }
static uint32_t creation_alloc(int zone)
{ (void)zone; creation_charge(4000); return ++creation.pages * PAGE_SIZE; }
static void *creation_clear(void *p, int value, size_t n)
{ creation_charge(8000); return memset(p, value, n); }
static int creation_map(void *as, uint32_t va, uint32_t phys, unsigned flags)
{ (void)as; (void)va; (void)phys; (void)flags; creation_charge(6000); return 0; }
static void creation_point(void)
{
    if (creation_points) {
        creation_charge(200); creation.points++; creation_boundary();
    }
}
static int creation_loop(void)
{
    struct {
        struct { uint32_t quota; } memory;
        struct { unsigned tables; } as;
        struct { uint32_t resident; } info;
    } vm = {.memory.quota = ''' + cap + r'''}, *v = &vm;
    unsigned tables = 0;
#define VM_POOL_BASE 0x110000u
#pragma push_macro("P2V")
#undef P2V
#define P2V(p) ((void)(p), creation_page)
#define pmm_alloc_zone creation_alloc
#define pmm_free(p) ((void)(p))
#define memset creation_clear
#define as_map creation_map
#define kpreempt_point creation_point
''' + loop + r'''
#undef kpreempt_point
#undef as_map
#undef memset
#undef pmm_free
#undef pmm_alloc_zone
#undef P2V
#pragma pop_macro("P2V")
#undef VM_POOL_BASE
    return 0;
}
static struct creation_result creation_replay(bool points, void (*service)(uint64_t))
{
    memset(&creation, 0, sizeof(creation));
    creation_points = points; creation_service = service;
    CHECK(!creation_loop()); creation_boundary();
    return creation;
}
''')

    # F0-44 audio/scheduler replay uses the actual destruction loop as well.
    source = (ROOT / 'src/kernel/core/vmm.c').read_text()
    start = source.index('void as_destroy(')
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    body = source[start:end].replace('void as_destroy(', 'static void creation_destroy_loop(')
    body = body.replace('uint32_t old = pt[j];', 'creation_charge(1000); uint32_t old = pt[j];')
    body = body.replace('if (pd[i] & PTE_P) {', 'creation_charge(1000); if (pd[i] & PTE_P) {')
    body = body.replace('table_va(', 'creation_table_va(').replace('mm_free(', 'creation_release(')
    body = body.replace('g_user_mappings', 'creation_mappings').replace('kpd_phys', '0')
    body = body.replace('read_cr3()', '0u').replace('write_cr3(0)', '((void)0)')
    body = body.replace('kpreempt_point()', 'creation_point()')
    with Path(destination).open('a') as stream:
        stream.write(r'''
static uint32_t creation_tables[8][1024], creation_mappings;
static uint32_t *creation_table_va(uint32_t p)
{ CHECK(p>=PAGE_SIZE && p<=sizeof(creation_tables)); return creation_tables[p/PAGE_SIZE-1]; }
static void creation_release(uint32_t p, enum mm_owner owner)
{ (void)p; (void)owner; creation_charge(6000); }
''' + body + r'''
static inline struct creation_result creation_cleanup_replay(bool points, void (*service)(uint64_t))
{
    memset(&creation,0,sizeof(creation)); memset(creation_tables,0,sizeof(creation_tables));
    creation_points=points; creation_service=service;
    struct aspace as={.pd_phys=PAGE_SIZE,.pages=6416,.tables=7,.owner=MM_VM_PRIVATE,.pool_base=0x110000};
    creation_mappings=as.pages;
    for (unsigned i=0;i<7;i++) creation_tables[0][i]=(i+2)*PAGE_SIZE|PTE_P;
    for (unsigned i=0;i<6416;i++) creation_tables[1+i/1024][i%1024]=(10000+i)*PAGE_SIZE|PTE_P;
    creation_destroy_loop(&as); creation_boundary();
    CHECK(!as.pd_phys && !creation_mappings);
    return creation;
}
''')


if __name__ == '__main__':
    write_model(sys.argv[1])
