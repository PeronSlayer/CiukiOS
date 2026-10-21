/* Three depths, ten real mode-loss/restore cycles, retained system pixels. LGPL-2.1-or-later */
#include "scene.h"
static void surfaces(void)
{
    unsigned depths[]={8,16,32};
    for(unsigned i=0;i<3;i++) {
        unsigned errors=result->row[0].errors;
        if(!scene_mode(0,640,480,depths[i]))continue;
        DD *surface=scene_surface(0,DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY,depths[i]);
        if(surface) {
            DDDESC locked={.size=sizeof(locked)};
            DDRESULT error=METHOD(surface,25,DDRESULT(W32API *)(DD *,RECT *,DDDESC *,DWORD,void *))(
                surface,0,&locked,DDLOCK_WAIT,0);
            CHECK(0,!error && locked.pixels && locked.pixel.bits==depths[i]);
            if(!error) {
                for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16*(depths[i]/8);x++)
                    ((uint8_t *)locked.pixels)[y*locked.pitch+x]=(uint8_t)(x+y);
                CHECK(0,!METHOD(surface,32,DDRESULT(W32API *)(DD *,void *))(surface,0));
            }
            scene_release(surface);
        }
        if(errors==result->row[0].errors)result->row[0].value[0]++;
    }
}
void W32API entry(void)
{
    if(!scene_begin(W32_FIXTURE_DDRAW)){fixture_finish();return;}
    surfaces();
    if(scene_mode(1,640,480,32)) {
        DD *video=scene_surface(1,DDSCAPS_OFFSCREENPLAIN|DDSCAPS_VIDEOMEMORY,32);
        DD *system=scene_surface(1,DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY,32);
        if(video && system) {
            DDDESC locked={.size=sizeof(locked)};
            DDRESULT error=METHOD(system,25,DDRESULT(W32API *)(DD *,RECT *,DDDESC *,DWORD,void *))(system,0,&locked,DDLOCK_WAIT,0);
            CHECK(1,!error && locked.pixels);
            if(!error){((uint32_t *)locked.pixels)[0]=0x375564;CHECK(1,!METHOD(system,32,DDRESULT(W32API *)(DD *,void *))(system,0));}
            unsigned retained=1;
            for(unsigned cycle=0;cycle<10;cycle++) {
                unsigned errors=result->row[1].errors;
                CHECK(1,!METHOD(scene_device,19,DDRESULT(W32API *)(DD *))(scene_device));
                CHECK(1,METHOD(video,24,DDRESULT(W32API *)(DD *))(video)==DDERR_SURFACELOST);
                if(!scene_mode(1,640,480,32))break;
                CHECK(1,!METHOD(video,27,DDRESULT(W32API *)(DD *))(video));
                CHECK(1,!METHOD(video,24,DDRESULT(W32API *)(DD *))(video));
                CHECK(1,!METHOD(system,24,DDRESULT(W32API *)(DD *))(system));
                locked=(DDDESC){.size=sizeof(locked)};
                error=METHOD(system,25,DDRESULT(W32API *)(DD *,RECT *,DDDESC *,DWORD,void *))(system,0,&locked,DDLOCK_WAIT,0);
                CHECK(1,!error && locked.pixels);
                if(!error) {
                    retained&=((uint32_t *)locked.pixels)[0]==0x375564;
                    CHECK(1,!METHOD(system,32,DDRESULT(W32API *)(DD *,void *))(system,0));
                }else retained=0;
                if(errors==result->row[1].errors)result->row[1].value[0]++;
            }
            result->row[1].value[1]=retained;
        }
        scene_release(video);scene_release(system);
    }
    scene_end();
    result->row[1].value[2]=w32_ddraw_surface_bytes();
    CHECK(1,!result->row[1].value[2]);
    fixture_finish();
}
