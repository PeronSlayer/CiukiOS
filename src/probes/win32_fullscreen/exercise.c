/* Indexed scenes; absent lease instrumentation stays explicitly incomplete. LGPL-2.1-or-later */
#include "../win32_ddraw/scene.h"
static void indexed(unsigned row,unsigned width,unsigned height)
{
    if(!scene_mode(row,width,height,8))return;
    DD *primary=scene_surface(row,DDSCAPS_PRIMARYSURFACE,8);
    if(!primary)return;
    PALETTEENTRY colors[256];
    for(unsigned i=0;i<256;i++)colors[i]=(PALETTEENTRY){(uint8_t)i,(uint8_t)(255-i),(uint8_t)(i^85),0};
    DD *palette=0;
    CHECK(row,!METHOD(scene_device,5,DDRESULT(W32API *)(DD *,DWORD,PALETTEENTRY *,DD **,void *))(
        scene_device,DDPCAPS_8BIT|DDPCAPS_ALLOW256,colors,&palette,0));
    if(palette)CHECK(row,!METHOD(primary,31,DDRESULT(W32API *)(DD *,DD *))(primary,palette));
    DDDESC desc={.size=sizeof(desc)};
    DDRESULT error=METHOD(primary,25,DDRESULT(W32API *)(DD *,RECT *,DDDESC *,DWORD,void *))(
        primary,0,&desc,DDLOCK_WAIT,0);
    CHECK(row,!error && desc.pixels);
    if(!error) {
        result->row[row].value[0]=desc.width;result->row[row].value[1]=desc.height;
        result->row[row].value[2]=desc.pixel.bits;
        for(unsigned y=0;y<desc.height;y++)for(unsigned x=0;x<desc.width;x++)
            ((uint8_t *)desc.pixels)[y*desc.pitch+x]=(uint8_t)(x+y);
        CHECK(row,!METHOD(primary,32,DDRESULT(W32API *)(DD *,void *))(primary,0));
    }
    scene_release(palette);scene_release(primary);
    /* DirectDraw's lease tuple exposes logical dimensions and pitch only.
     * Never infer actual presenter scale from the requested logical mode. */
    fixture_unsupported(row);
}
void W32API entry(void)
{
    if(scene_begin(W32_FIXTURE_FULLSCREEN)) {
        indexed(0,640,480);indexed(1,320,200);scene_end();
    }
    /* Stale-token refusal, former-owner writes, rollback and restored input
     * need desktop/kernel transaction checkpoints, not an API success. */
    fixture_unsupported(2);fixture_finish();
}
