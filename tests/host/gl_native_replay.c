/* f7-14: Graphics Demo scenes through the production native GL (libgl: the
 * shared opengl32 engine, journal, admission and native provider), the
 * copied render3d wire, the kernel validator and the fake Savage of
 * gl_native_kernel.c. Software (unqualified, QEMU) and hardware frames are
 * compared and counted. SPDX-License-Identifier: MIT */
#include "../../apps/gfxdemo/gfxdemo.h"
#include <GL/gl.h>
#include <ciuki/gl.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gl_native_setup(int qualified);
void gl_native_teardown(void);
void gl_native_mode(unsigned w,unsigned h);
void gl_native_transport(int present);
void gl_native_texture_full(int full);
void gl_native_lease(unsigned token,unsigned generation);
void gl_native_revoke(void);
void gl_native_process_exit(void);
unsigned gl_native_draws(void), gl_native_copies(void), gl_native_writes(void);
unsigned gl_native_gpu_presents(void), gl_native_cpu_presents(void), gl_native_exchanges(void);
unsigned gl_native_uploads(void), gl_native_reads(void);
const uint32_t *gl_native_frame(void);

static const char *const names[]={"cube-flat","cube-gouraud","tiles"};
static const uint64_t phases[]={1234567890ull,2234567890ull};
static uint32_t *portrait;

static struct ciuki_gl_status status(void)
{
    struct ciuki_gl_status s={.size=sizeof(s)};
    assert(!ciuki_gl_status(&s));
    return s;
}
static void render(struct gfx_scene *s,unsigned scene,uint64_t phase)
{
    if (scene<2) gfx_cube(s,phase,portrait,(int)scene);
    else gfx_sprites(s,phase,portrait);
    assert(!s->error);
}
/* Hardware sampling, coverage and depth follow the declared f7-11 envelope,
 * not bit equality with the scalar path. Coverage must be identical: the
 * background (clear colour) set is compared exactly; colour channels inside
 * geometry are reported and bounded. */
struct difference { unsigned pixels, over2, coverage, max; };
static struct difference compare(const uint32_t *a,const uint32_t *b,unsigned n,uint32_t background)
{
    struct difference d={0};
    for (unsigned i=0;i<n;i++) {
        if ((a[i]==background)!=(b[i]==background)) d.coverage++;
        unsigned worst=0;
        for (unsigned c=0;c<3;c++) {
            int delta=(int)(a[i]>>(c*8)&255)-(int)(b[i]>>(c*8)&255);
            unsigned m=(unsigned)(delta<0 ? -delta : delta);
            if (m>worst) worst=m;
        }
        d.pixels+=worst!=0; d.over2+=worst>2;
        if (worst>d.max) d.max=worst;
    }
    return d;
}
static uint32_t background;
static uint32_t clear_colour(const uint32_t *pixels)
{
    (void)pixels;
    return background; /* the scenes' common clear colour */
}
static void software_reference(uint32_t *golden,unsigned w,unsigned h,struct gfx_scene *s)
{
    gl_native_setup(0);
    s->width=w; s->height=h;
    for (unsigned scene=0;scene<3;scene++) for (unsigned p=0;p<2;p++) {
        render(s,scene,phases[p]);
        memcpy(golden+(size_t)(scene*2+p)*w*h,s->target ? s->target : s->pixels,(size_t)w*h*4);
    }
    background=golden[0]; /* the cube leaves the top-left corner uncovered */
    struct ciuki_gl_status st=status();
    assert(st.backend==CIUKI_GL_BACKEND_SOFTWARE && !st.generation && !st.hardware_failed);
    assert(st.frames==6 && st.software_frames==6 && !st.hardware_frames && !st.hardware_draws && st.software_draws);
    assert(!gl_native_draws() && !gl_native_writes());
    assert(ciuki_gl_set_backend(CIUKI_GL_BACKEND_HARDWARE)==-ENODEV);
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_AUTO));
    ciuki_gl_close(); gl_native_teardown();
    printf("PASS native GL software path on an unqualified device (QEMU model): %ux%u, 6 frames, 0 GPU draws, 0 device writes, hardware policy refused\n",w,h);
}
static void hardware_replay(const uint32_t *golden,unsigned w,unsigned h,struct gfx_scene *s)
{
    gl_native_setup(2);
    s->width=w; s->height=h;
    for (unsigned scene=0;scene<3;scene++) {
        unsigned draws=gl_native_draws();
        struct ciuki_gl_status before=status();
        struct difference total={0};
        unsigned uploads=0,reads=0;
        for (unsigned p=0;p<2;p++) {
            unsigned u0=gl_native_uploads(),r0=gl_native_reads();
            render(s,scene,phases[p]);
            /* Steady state: the target stays on the provider between frames
             * (no upload); each completed frame is read back once (the
             * journal checkpoint, as in opengl32). */
            if (p) { uploads=gl_native_uploads()-u0; reads=gl_native_reads()-r0; }
            const uint32_t *ref=golden+(size_t)(scene*2+p)*w*h;
            struct difference d=compare(s->target ? s->target : s->pixels,ref,w*h,clear_colour(ref));
            total.pixels+=d.pixels; total.over2+=d.over2; total.coverage+=d.coverage;
            if (d.max>total.max) total.max=d.max;
        }
        struct ciuki_gl_status after=status();
        uint64_t gpu=after.hardware_frames-before.hardware_frames, cpu=after.software_frames-before.software_frames;
        printf("GL_NATIVE_REPLAY scene=%s width=%u height=%u frames=2 gpu_frames=%llu cpu_frames=%llu gpu_draws=%llu cpu_draws=%llu "
               "refusals=%llu fallbacks=%llu kernel_draw_packets=%u differing_pixels=%u over_2_levels=%u max_level=%u coverage_mismatch=%u "
               "steady_uploads=%u steady_reads=%u\n",
               names[scene],w,h,(unsigned long long)gpu,(unsigned long long)cpu,
               (unsigned long long)(after.hardware_draws-before.hardware_draws),
               (unsigned long long)(after.software_draws-before.software_draws),
               (unsigned long long)(after.refusals-before.refusals),(unsigned long long)(after.fallbacks-before.fallbacks),
               gl_native_draws()-draws,total.pixels,total.over2,total.max,total.coverage,uploads,reads);
        assert(gpu==2 && !cpu && after.software_draws==before.software_draws);
        assert(after.refusals==before.refusals && !after.fallbacks && gl_native_draws()>draws);
        assert(!total.coverage && total.over2*1000<=2u*w*h*2); /* at most 0.1 % beyond two levels */
        assert(!uploads && reads==2); /* colour and depth */
    }
    struct ciuki_gl_status st=status();
    assert(st.backend==CIUKI_GL_BACKEND_HARDWARE && st.generation && !st.hardware_failed);
}
static void policy(const uint32_t *golden,unsigned w,unsigned h,struct gfx_scene *s)
{
    /* Software policy: the qualified provider stays closed; exact scalar pixels. */
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_SOFTWARE));
    unsigned draws=gl_native_draws();
    struct ciuki_gl_status before=status();
    render(s,0,phases[0]);
    assert(!memcmp(s->target ? s->target : s->pixels,golden,(size_t)w*h*4));
    struct ciuki_gl_status after=status();
    assert(gl_native_draws()==draws && after.software_frames==before.software_frames+1 &&
        after.hardware_frames==before.hardware_frames && after.backend==CIUKI_GL_BACKEND_SOFTWARE && after.policy==CIUKI_GL_BACKEND_SOFTWARE);
    /* Required hardware on a qualified device; automatic again afterwards. */
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_HARDWARE));
    render(s,0,phases[0]);
    after=status();
    assert(gl_native_draws()>draws && after.hardware_frames==before.hardware_frames+1 && after.backend==CIUKI_GL_BACKEND_HARDWARE);
    assert(ciuki_gl_set_backend(7)==-EINVAL && !ciuki_gl_set_backend(CIUKI_GL_BACKEND_AUTO));
    puts("PASS backend policy: software keeps the qualified provider closed (exact scalar frame), hardware and automatic reopen it");
}
static void untextured_frame(struct gfx_scene *s)
{
    render(s,2,phases[0]); /* binds and clears; its tiles are refused */
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glDisable(GL_TEXTURE_2D); glColor3f(1,.5f,.25f);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBegin(GL_TRIANGLES); glVertex3f(-1,-1,0); glVertex3f(1,-1,0); glVertex3f(0,1,0); glEnd();
    ciuki_gl_finish();
    assert(glGetError()==GL_NO_ERROR);
}
static void refusal(unsigned w,unsigned h,struct gfx_scene *s)
{
    /* The kernel's texture heap is exhausted: every textured draw is
     * refused (temporary), those frames are CPU frames, the context stays on
     * the provider and untextured work runs there. */
    gl_native_setup(2); gl_native_texture_full(1); s->width=w; s->height=h;
    render(s,0,phases[0]);
    struct ciuki_gl_status a=status();
    assert(a.refusals>=12 && a.software_frames==1 && !a.hardware_frames && !a.fallbacks && !a.hardware_failed &&
        a.backend==CIUKI_GL_BACKEND_HARDWARE && a.hardware_draws);
    unsigned draws=gl_native_draws();
    untextured_frame(s);
    struct ciuki_gl_status b=status();
    assert(b.hardware_frames==1 && b.software_frames==2 && gl_native_draws()>draws && !b.fallbacks);
    ciuki_gl_close(); gl_native_teardown();
    printf("PASS temporary refusals: portrait upload refused by a full texture heap %llu times, CPU frame, context kept; untextured frame on the GPU\n",
        (unsigned long long)a.refusals);
}
static void fogged_frame(struct gfx_scene *s)
{
    render(s,2,phases[0]);
    glEnable(GL_FOG); glFogi(GL_FOG_MODE,GL_LINEAR); glFogf(GL_FOG_START,0); glFogf(GL_FOG_END,2);
    glColor3f(1,1,1); glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBegin(GL_TRIANGLES); glVertex3f(-1,-1,-1); glVertex3f(1,-1,-1); glVertex3f(0,1,-1); glEnd();
    glDisable(GL_FOG);
    ciuki_gl_finish();
    assert(glGetError()==GL_NO_ERROR);
}
static void state_fallback(unsigned w,unsigned h,struct gfx_scene *s)
{
    /* Unsupported state (fog) moves the whole context to the scalar path
     * before any side effect of that draw (contract 7.2): the frame equals
     * the software frame and the context never reopens hardware. */
    uint32_t *reference=malloc((size_t)w*h*4); assert(reference);
    gl_native_setup(2); s->width=w; s->height=h;
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_SOFTWARE));
    fogged_frame(s);
    memcpy(reference,s->target ? s->target : s->pixels,(size_t)w*h*4);
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_AUTO));
    render(s,2,phases[1]);
    struct ciuki_gl_status before=status();
    assert(before.backend==CIUKI_GL_BACKEND_HARDWARE && !before.fallbacks);
    fogged_frame(s);
    struct ciuki_gl_status after=status();
    assert(!memcmp(reference,s->target ? s->target : s->pixels,(size_t)w*h*4));
    assert(after.fallbacks==1 && after.hardware_failed && after.backend==CIUKI_GL_BACKEND_SOFTWARE &&
        after.hardware_frames==before.hardware_frames+1 && /* the tiles frame */
        after.software_frames==before.software_frames+1);  /* the fogged frame */
    unsigned draws=gl_native_draws();
    render(s,2,phases[1]);
    assert(gl_native_draws()==draws && ciuki_gl_set_backend(CIUKI_GL_BACKEND_HARDWARE)==-ENODEV);
    ciuki_gl_close(); gl_native_teardown(); free(reference);
    puts("PASS unsupported state (fog): whole-context software fallback with the exact software frame, latched");
}
static void fullscreen(const uint32_t *golden,unsigned w,unsigned h,struct gfx_scene *s)
{
    struct ciuki_gl_lease lease={.token=1234,.generation=42,.width=w,.height=h,.bpp=32,.pitch=w*4};
    gl_native_lease(1234,42);
    s->lease=&lease;
    unsigned copies=gl_native_copies(),reads=gl_native_reads(),uploads=gl_native_uploads();
    render(s,0,phases[0]);
    assert(s->presented==1 && gl_native_gpu_presents()==1 && gl_native_copies()==copies+1 && !gl_native_cpu_presents());
    /* f7-15 resident targets: a GPU present reads nothing back. */
    assert(gl_native_reads()==reads && gl_native_uploads()==uploads);
    struct difference d=compare(gl_native_frame(),golden,w*h,clear_colour(golden));
    assert(!d.coverage && d.over2*1000<=w*h);
    struct ciuki_gl_status st=status();
    assert(st.presents==1 && st.hardware_presents==1);
    /* Software policy presents the finished pixels through W32_DISPLAY_FRAME. */
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_SOFTWARE));
    render(s,0,phases[0]);
    assert(s->presented==0 && gl_native_cpu_presents()==1 && !memcmp(gl_native_frame(),golden,(size_t)w*h*4));
    assert(!ciuki_gl_set_backend(CIUKI_GL_BACKEND_AUTO));
    /* A revoked lease is refused by the kernel; nothing reaches the scanout. */
    gl_native_revoke();
    render(s,0,phases[0]);
    assert(s->presented==-EACCES && gl_native_gpu_presents()==1);
    st=status();
    assert(st.presents==2 && st.hardware_presents==1 && !st.fallbacks);
    lease.width=w+1;
    render(s,0,phases[0]);
    assert(s->presented==-EINVAL);
    s->lease=0;
    printf("PASS full-screen lease: completed GPU copy (%u kernel copies), CPU frame under the software policy, revoked and mismatched leases refused\n",
        gl_native_copies()-copies);
}
static void context_fallback(const uint32_t *golden,unsigned w,unsigned h,struct gfx_scene *s)
{
    /* The kernel drops the process's render3d contexts (death model). The
     * next submission fails, the journal replays in software and the whole
     * context stays on the scalar path (contract 7.2). */
    render(s,1,phases[0]);
    gl_native_process_exit();
    struct ciuki_gl_status before=status();
    render(s,1,phases[1]);
    assert(!memcmp(s->target ? s->target : s->pixels,golden+(size_t)3*w*h,(size_t)w*h*4));
    struct ciuki_gl_status after=status();
    assert(after.fallbacks==before.fallbacks+1 && after.hardware_failed && after.backend==CIUKI_GL_BACKEND_SOFTWARE);
    unsigned draws=gl_native_draws();
    render(s,2,phases[0]);
    assert(gl_native_draws()==draws && !memcmp(s->target ? s->target : s->pixels,golden+(size_t)4*w*h,(size_t)w*h*4));
    assert(ciuki_gl_set_backend(CIUKI_GL_BACKEND_HARDWARE)==-ENODEV);
    puts("PASS whole-context fallback: failed submission replayed in software with exact pixels, latched, hardware policy refused");
}
int main(int argc,char **argv)
{
    assert(argc==2);
    setvbuf(stdout,NULL,_IONBF,0); /* rows survive a failed assertion */
    portrait=malloc(256*256*4); assert(portrait);
    FILE *f=fopen(argv[1],"rb"); assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    struct gfx_scene *s=calloc(1,sizeof(*s)); assert(s);
    const unsigned w=640,h=480;
    uint32_t *golden=malloc((size_t)6*w*h*4); assert(golden);
    /* No kernel transport at all (an older kernel): software, no exchanges. */
    gl_native_setup(2); gl_native_transport(0); s->width=w; s->height=h;
    unsigned writes=gl_native_writes(); /* after the boot qualification */
    render(s,0,phases[0]);
    struct ciuki_gl_status st=status();
    assert(st.backend==CIUKI_GL_BACKEND_SOFTWARE && st.software_frames==1 && !gl_native_exchanges() && gl_native_writes()==writes);
    ciuki_gl_close(); gl_native_teardown();
    software_reference(golden,w,h,s);
    hardware_replay(golden,w,h,s);
    policy(golden,w,h,s);
    fullscreen(golden,w,h,s);
    context_fallback(golden,w,h,s);
    ciuki_gl_close(); gl_native_teardown();
    state_fallback(w,h,s);
    refusal(w,h,s);
    /* The opt-in census of opengl32 also runs natively (environment). */
    assert(!setenv("CIUKI_GL_CENSUS","123456789012345678",1));
    gl_native_setup(2); s->width=w; s->height=h; render(s,0,phases[0]); ciuki_gl_close(); gl_native_teardown();
    fprintf(stderr,"CENSUS_MARK\n");
    assert(!setenv("CIUKI_GL_CENSUS","1",1));
    gl_native_setup(2); render(s,0,phases[0]); render(s,2,phases[0]); ciuki_gl_close(); gl_native_teardown();
    assert(!unsetenv("CIUKI_GL_CENSUS"));
    printf("PASS 640x480 Graphics Demo scenes: production native GL, copied wire, validator and fake Savage; GPU frames within the sampling envelope\n");
    /* T2 render size: the cube and tiles at 1024x768 on the GPU, presented
     * by a GPU copy under a lease of the same size. */
    gl_native_mode(1024,768);
    uint32_t *target=malloc((size_t)1024*768*4); assert(target);
    s->target=target;
    uint32_t *full=malloc((size_t)6*1024*768*4); assert(full);
    software_reference(full,1024,768,s);
    hardware_replay(full,1024,768,s);
    struct ciuki_gl_lease lease={.token=77,.generation=42,.width=1024,.height=768,.bpp=32,.pitch=4096};
    gl_native_lease(77,42); s->lease=&lease;
    unsigned reads=gl_native_reads(),uploads=gl_native_uploads();
    for (unsigned scene=0;scene<3;scene++) {
        render(s,scene,phases[1]); assert(s->presented==1);
        const uint32_t *ref=full+(size_t)(scene*2+1)*1024*768;
        struct difference d=compare(gl_native_frame(),ref,1024*768,clear_colour(ref));
        assert(!d.coverage && d.over2*1000<=1024*768);
    }
    st=status();
    assert(st.hardware_presents==3 && gl_native_gpu_presents()==3 && !st.fallbacks);
    printf("GL_NATIVE_LEASE width=1024 height=768 frames=3 gpu_presents=%u reads=%u uploads=%u\n",
        gl_native_gpu_presents(),gl_native_reads()-reads,gl_native_uploads()-uploads);
    assert(gl_native_reads()==reads && gl_native_uploads()==uploads); /* no per-frame readback */
    /* gfxdemo frees its full-screen buffer when the lease ends and binds its
     * window target next: the GPU-presented frame is released, never read
     * back into the freed pixels (ASan); the new frame reads back once. */
    uint32_t *next=malloc((size_t)1024*768*4); assert(next);
    free(target); target=next; s->target=next; s->lease=0;
    render(s,0,phases[1]);
    struct difference d=compare(next,full+(size_t)1*1024*768,1024*768,clear_colour(full));
    assert(!d.coverage && gl_native_reads()==reads+2 && gl_native_uploads()==uploads);
    /* Close right after a GPU present releases the frame without a readback. */
    s->lease=&lease; render(s,1,phases[1]); assert(s->presented==1);
    reads=gl_native_reads();
    ciuki_gl_close(); assert(gl_native_reads()==reads);
    gl_native_teardown();
    printf("PASS 1024x768 (contract T2 size): cube flat/Gouraud and tiles on the GPU, %u completed GPU-copy presents without readback; rebind and close after a GPU present release the frame\n",gl_native_gpu_presents());
    free(full); free(target); free(golden); free(s); free(portrait);
    return 0;
}
