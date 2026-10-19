/* Production fixture state, no DOS/BIOS simulation or guest evidence. MIT. */
#include <assert.h>
#include "../../../src/kernel/vm/dos_conformance_fixture.h"
int main(void)
{
    struct d48_media m;uint8_t sector[512];
    struct dos_file_frame enter={.ax=0x3d00,.cs=0x1000,.ip=12,.ss=0x2000,.sp=0x300};
    struct dos_file_frame returned=enter;returned.ax=5;
    d48_reset(&m,1,99);
    assert(!d48_sector(&m,D48_E_START+D48_DATA_SECTOR,sector,true) && sector[0]=='F' && !m.failures);
    d48_open_enter(&m,&enter,false);d48_open_return(&m,&returned,true);assert(!m.armed);
    d48_open_enter(&m,&enter,true);
    struct dos_file_frame other=returned;other.sp++;d48_open_return(&m,&other,true);assert(!m.armed && m.pending);
    d48_open_return(&m,&returned,true);assert(m.armed && m.opens==1 && m.remaining==5);
    assert(!d48_sector(&m,D48_E_START+41,sector,true) && !memcmp(sector,"D48FAULTDAT",11));
    for (unsigned i=0;i<5;i++) assert(d48_sector(&m,D48_E_START+D48_DATA_SECTOR,sector,true)==-FS_EIO);
    assert(m.failures==5 && !m.armed);
    for (unsigned i=0;i<8;i++) { assert(!d48_sector(&m,D48_E_START+D48_DATA_SECTOR+i,sector,true));for (unsigned j=0;j<512;j++) assert(sector[j]=='F'); }
    d48_open_enter(&m,&enter,true);d48_open_return(&m,&returned,true);assert(!m.armed && m.opens==1);
    assert(!d48_sector(&m,0,sector,true) && fs_rd32(sector+454)==63 && fs_rd32(sector+470)==D48_E_START);
    assert(sector[510]==0x55 && sector[511]==0xaa);
    assert(d48_sector(&m,D48_DISK_SECTORS,sector,true)==-FS_EINVAL);
    d48_reset(&m,2,100);d48_open_enter(&m,&enter,true);d48_open_return(&m,&returned,true);
    for (unsigned i=0;i<12;i++) assert(d48_sector(&m,D48_E_START+D48_DATA_SECTOR,sector,true)==-FS_EIO);
    assert(m.armed && m.failures==12);
    d48_reset(&m,1,101);d48_open_enter(&m,&enter,true);d48_open_return(&m,&returned,false);
    assert(d48_sector(&m,D48_E_START+D48_DATA_SECTOR,sector,false)==-FS_EIO);
    assert(!d48_sector(&m,D48_E_START+D48_DATA_SECTOR,sector,false) && m.failures==1);
    d48_reset(&m,1,102);d48_open_enter(&m,&enter,true);returned.flags=1;d48_open_return(&m,&returned,false);
    assert(m.bad && !m.armed && !m.opens && !m.failures);
    d48_reset(&m,0,103);assert(d48_sector(&m,0,sector,false)==-FS_EINVAL);
}
