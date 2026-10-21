/* SPDX-License-Identifier: MIT
 * Include the decoder to cover its private restore/metadata allocation paths. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allocation_fail.h"
#include "gif.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
static nsgif_bitmap_t *create(int w,int h) { return ciuki_image_calloc((size_t)w*h,4); }
static void destroy(nsgif_bitmap_t *p) { ciuki_image_free(p); }
static uint8_t *buffer(nsgif_bitmap_t *p) { return p; }
static const nsgif_bitmap_cb_vt ops={.create=create,.destroy=destroy,.get_buffer=buffer};
static unsigned exercise(void)
{
    nsgif_t *gif=NULL;
    if(nsgif_create(&ops,NSGIF_BITMAP_FMT_R8G8B8A8,&gif)!=NSGIF_OK)return allocation_count;
    gif->info.width=128;gif->info.height=128;gif->decoded_frame=1;
    nsgif__get_frame(gif,0);nsgif__get_frame(gif,63);
    nsgif__record_frame(gif,NULL);
    nsgif_destroy(gif);
    assert(ciuki_image_used()==0);
    return allocation_count;
}
static unsigned decode(const uint8_t *data,size_t length)
{
    nsgif_t *gif=NULL;
    nsgif_error error=nsgif_create(&ops,NSGIF_BITMAP_FMT_R8G8B8A8,&gif);
    if(error==NSGIF_OK) {
        error=nsgif_data_scan(gif,length,data);
        if(error==NSGIF_OK) {
            nsgif_data_complete(gif);
            for(unsigned i=0;i<gif->info.frame_count;i++) {
                nsgif_bitmap_t *bitmap=NULL;
                error=nsgif_frame_decode(gif,i,&bitmap);
                if(error!=NSGIF_OK)break;
                assert(bitmap);
            }
        }
    }
    if(!fail_allocation)assert(error==NSGIF_OK);
    nsgif_destroy(gif);assert(!ciuki_image_used());
    return allocation_count;
}
int main(int argc,char **argv)
{
    assert(argc==2);uint8_t data[4096];
    FILE *file=fopen(argv[1],"rb");assert(file);
    size_t length=fread(data,1,sizeof(data),file);assert(feof(file) && length);fclose(file);
    allocation_count=0;unsigned decoded_allocs=decode(data,length);assert(decoded_allocs>=4);
    for(unsigned i=1;i<=decoded_allocs;i++) {
        allocation_count=0;fail_allocation=i;decode(data,length);fail_allocation=0;
    }
    allocation_count=0;unsigned count=exercise();assert(count>=4);
    for(unsigned i=1;i<=count;i++) {
        allocation_count=0;fail_allocation=i;exercise();fail_allocation=0;
        assert(ciuki_image_used()==0);
    }
    nsgif_t *gif=NULL;assert(nsgif_create(&ops,NSGIF_BITMAP_FMT_R8G8B8A8,&gif)==NSGIF_OK);
    assert(nsgif__get_frame(gif,63));
    size_t metadata=ciuki_image_used();assert(metadata>sizeof(*gif));
    assert(!nsgif__get_frame(gif,64) && ciuki_image_used()==metadata);
    gif->info.width=512;gif->info.height=512;gif->decoded_frame=1;
    /* Bitmap + restore would exceed the same 2 MiB ledger. */
    assert(nsgif__record_frame(gif,NULL)==NSGIF_ERR_OOM);
    assert(gif->frame_image && !gif->prev_frame && ciuki_image_used()<=CIUKI_IMAGE_BYTES);
    nsgif_destroy(gif);assert(!ciuki_image_used());
    void *p=ciuki_image_malloc(100);assert(p);memset(p,0x7a,100);
    size_t used=ciuki_image_used();
    assert(!ciuki_image_realloc(p,CIUKI_IMAGE_BYTES) && ciuki_image_used()==used && *(unsigned char *)p==0x7a);
    assert(!ciuki_image_calloc(SIZE_MAX,2));ciuki_image_free(p);assert(!ciuki_image_used());
    puts("NetSurf GIF: shared 2 MiB ledger, restore rejection/OOM, frame cap, overflow and allocation sweep passed");
    fflush(stdout);
}
