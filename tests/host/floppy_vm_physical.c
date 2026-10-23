/* Physical register backend for the production DOS VM assignment fixture.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "floppy_fixture.h"
static struct floppy_fixture *vm_floppy;
static struct blkdev *native;
void host_floppy_open(void)
{
    vm_floppy=calloc(1,sizeof(*vm_floppy));assert(vm_floppy);assert(!fixture_open(vm_floppy,4));
    for(unsigned i=0;i<512;i++)vm_floppy->disk[i]=(uint8_t)(i*7+11);
}
struct fdc *floppy_controller(void) { return vm_floppy ? &vm_floppy->fdc : 0; }
void host_floppy_native(bool open)
{ assert(!(open ? fdc_native_open(&vm_floppy->fdc,0,&native) : fdc_native_close(&vm_floppy->fdc,0))); }
void host_floppy_change(void) { vm_floppy->changed=true; }
void host_floppy_blank(void) { vm_floppy->blank=true;vm_floppy->changed=true; }
void host_floppy_error(uint8_t st1) { vm_floppy->st1=st1; }
unsigned host_floppy_reads(void) { return vm_floppy->reads; }
void host_floppy_close(void)
{ fixture_close(vm_floppy);free(vm_floppy);vm_floppy=0; }
