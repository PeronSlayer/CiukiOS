/* SPDX-License-Identifier: MIT. Real viewer model and CLP file operations. */
#define main clipboard_viewer_program_main
#include "../../../apps/accessories/clipbrd_main.c"
#undef main
#include <assert.h>
static struct clip_store store;
static uint32_t client=2;
static int transport(void *ctx,struct desk_clip_wire *w,const void *data,struct desk_clip_wire *r,void **out)
{ return clip_request(ctx,client,w,data,r,out); }
int main(void)
{
    struct viewer *v=calloc(1,sizeof(*v)); assert(v);
    clip_init(&store); du_clip_bind(transport,&store);
    assert(!du_system_clipboard_text_set("Clipboard\nCiuki",15)); client=1;
    refresh(v); assert(v->text && !strcmp(v->text,"Clipboard\nCiuki") && v->menus[2].count==4);
    int sizes=0; for (unsigned i=0;i<v->formats.count;i++) if (strstr(v->labels[i],"bytes")) sizes++;
    assert(sizes==4);
    const char *path="build/host/g5/viewer-roundtrip.clp";
    assert(!save_clip(path)); client=2; assert(!du_system_clipboard_text_set("other",5)); client=1;
    assert(!read_clip(path)); refresh(v); assert(v->text && !strcmp(v->text,"Clipboard\nCiuki")); assert(!unlink(path));
    uint32_t rgb[]={0xff0000,0x00ff00,0x0000ff,0xffffff}; void *dib=NULL; uint32_t bytes;
    assert(!clip_dib_encode(rgb,2,2,2,&dib,&bytes)); client=2;
    assert(!du_clip_open() && !du_clip_empty() && !du_clip_put(8,dib,bytes) && !du_clip_close()); free(dib); client=1;
    refresh(v); assert(v->image && v->width==2 && v->height==2 && !memcmp(v->image,rgb,sizeof(rgb)));
    assert(!du_clip_open() && !du_clip_empty() && !du_clip_close()); refresh(v); assert(!v->image && !v->text && !v->formats.count);
    free(v); du_clip_bind(NULL,NULL); clip_destroy(&store);
    puts("PASS G5 Clipboard Viewer text/image preview, format sizes, CLP Save As/Open round-trip, external replacement and empty clipboard"); fflush(stdout);
}
