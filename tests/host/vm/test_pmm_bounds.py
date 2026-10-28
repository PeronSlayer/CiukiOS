"""Compare the actual DOS host substitute with PMM's run boundaries."""
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[3]

class PmmBounds(unittest.TestCase):
    def test_contiguous_substitute(self):
        lines=[]
        with (ROOT/'tests/host/dosvm_test.c').open() as source:
            for line in source:
                if line.startswith('uint32_t pmm_alloc_contig(') or lines:
                    lines.append(line)
                    if line.rstrip()=='}': break
        self.assertTrue(lines)
        prelude=r'''
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HOST_PAGES (PMM_NORMAL_START_PFN+8)
static bool allocated[HOST_PAGES], refuse;
static enum mm_owner page_owner[HOST_PAGES];
static struct mm_page_descriptor host_descriptors[HOST_PAGES];
static unsigned used_pages, contiguous_calls, fail_contiguous_at=UINT32_MAX;
static void creation_cost_charge(uint64_t n) { (void)n; }
static bool allocation_fails(void) { return refuse; }
'''
        checks=r'''
int main(void) {
    assert(!pmm_alloc_contig(0) && !pmm_alloc_contig(UINT32_MAX) && !used_pages);
    uint32_t p=pmm_alloc_contig(3); assert(p==PMM_NORMAL_START_PFN*PAGE_SIZE && used_pages==3);
    for (unsigned i=0;i<3;i++) assert(allocated[p/PAGE_SIZE+i] && host_descriptors[p/PAGE_SIZE+i].refs==1);
    memset(allocated,1,sizeof(allocated)); used_pages=0;
    allocated[PMM_NORMAL_START_PFN-1]=allocated[PMM_NORMAL_START_PFN]=false;
    assert(!pmm_alloc_contig(2));
    for (unsigned i=PMM_NORMAL_START_PFN;i<HOST_PAGES;i++) allocated[i]=!!(i&1);
    assert(!pmm_alloc_contig(2) && pmm_alloc_contig(1)==PMM_NORMAL_START_PFN*PAGE_SIZE);
    refuse=true; assert(!pmm_alloc_contig(1));
    puts("PASS DOS host contiguous substitute: zero, overflow, shared zone boundary, fragmentation, failure");
    return 0;
}
'''
        out=ROOT/'build/host/classaudit-dos-pmm'; out.mkdir(parents=True,exist_ok=True)
        unit=out/'bounds.c'; unit.write_text(prelude+''.join(lines)+checks)
        binary=out/'bounds'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src/kernel/include'),str(unit),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS DOS host contiguous substitute',result.stdout)

if __name__=='__main__': unittest.main()
