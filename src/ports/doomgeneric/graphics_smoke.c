/* Actual protected-mode bridge exercise. Requires the host installed first. */
#include "graphics_bridge.h"
#include <stdio.h>
int main(void) {
    unsigned x,y,i,scan,frames=0;
    int pressed,palette=1;
    uint32_t start;
    if(!cg_open()) { puts("[CGFXSMOKE] discovery failed");return 1; }
    puts("[CGFXSMOKE] real DPMI bridge ready");
    for(i=0;i<256;i++) {
        cg_palette()[i*3]=(unsigned char)i;
        cg_palette()[i*3+1]=(unsigned char)(255-i);
        cg_palette()[i*3+2]=(unsigned char)(i^0x55);
    }
    start=cg_ticks();
    do {
        for(y=0;y<200;y++) for(x=0;x<320;x++)
            cg_pixels()[y*320+x]=(unsigned char)(((x+frames)>>2)^(y>>2));
        if(!cg_present(palette)) {
            printf("[CGFXSMOKE] present failed dpmi=%04X service=%04X status=%04X ms=%lu host_frames=%lu\n",
                cg_error_code(),cg_error_service(),cg_info()->status,
                (unsigned long)cg_info()->milliseconds,(unsigned long)cg_info()->presented_frames);
            return 2;
        }
        palette=0;frames++;
        while(cg_key(&scan,&pressed)>0)
            if(pressed && scan==1) goto done;
    } while(!cg_close_requested() && (uint32_t)(cg_ticks()-start)<6000 && !cg_failed() && frames<100000UL);
done:
    if(cg_failed()) {
        printf("[CGFXSMOKE] clock poll failed dpmi=%04X service=%04X status=%04X\n",
               cg_error_code(),cg_error_service(),cg_info()->status);
        return 3;
    }
    printf("[CGFXSMOKE] frames=%u elapsed_ms=%lu host_frames=%lu status=%u\n",
      frames,(unsigned long)(cg_ticks()-start),
      (unsigned long)cg_info()->presented_frames,cg_info()->status);
    return 0;
}
