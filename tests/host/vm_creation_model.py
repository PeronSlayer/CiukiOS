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


if __name__ == '__main__':
    write_model(sys.argv[1])
