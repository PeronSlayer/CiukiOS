/* Private desktop/PE/kernel protocol, never an SDK interface. SPDX-License-Identifier: MIT */
#ifndef CIUKI_W32_CORE_CONTROLLER_H
#define CIUKI_W32_CORE_CONTROLLER_H
#include <stdint.h>
#define W32_CORE_MAGIC 0x62353257u
#define W32_FIXTURE_COUNT 10u
#define W32_CORE_ROWS 6u
#define W32_CORE_VALUES 6u
enum { W32_FIXTURE_KERNEL32, W32_FIXTURE_FULLSCREEN, W32_FIXTURE_REGISTRY,
       W32_FIXTURE_AUDIO, W32_FIXTURE_GDI32, W32_FIXTURE_CRASH,
       W32_FIXTURE_USER32, W32_FIXTURE_CODECS, W32_FIXTURE_DDRAW, W32_FIXTURE_INPUT };
struct w32_fixture_spec { const char *name, *path; uint32_t address; };
static const struct w32_fixture_spec w32_fixtures[W32_FIXTURE_COUNT]={
    {"kernel32-exercise", "/System/Win32/Fixtures/kernel32-exercise.exe", 0x10030000u},
    {"fullscreen-exercise", "/System/Win32/Fixtures/fullscreen-exercise.exe", 0x10040000u},
    {"registry-exercise", "/System/Win32/Fixtures/registry-exercise.exe", 0x10050000u},
    {"audio-exercise", "/System/Win32/Fixtures/audio-exercise.exe", 0x10060000u},
    {"gdi32-exercise", "/System/Win32/Fixtures/gdi32-exercise.exe", 0x10070000u},
    {"crash-exercise", "/System/Win32/Fixtures/crash-exercise.exe", 0x10080000u},
    {"user32-exercise", "/System/Win32/Fixtures/user32-exercise.exe", 0x10090000u},
    {"codecs-exercise", "/System/Win32/Fixtures/codecs-exercise.exe", 0x100a0000u},
    {"ddraw-exercise", "/System/Win32/Fixtures/ddraw-exercise.exe", 0x100b0000u},
    {"input-exercise", "/System/Win32/Fixtures/input-exercise.exe", 0x100c0000u},
};
struct w32_core_row { uint32_t checked, errors, value[W32_CORE_VALUES]; };
struct w32_core_result {
    uint32_t magic, version, done, release, fixture, unsupported, first_error;
    /* Registry only: request 1/2 asks the observer for a real w32srv crash.
     * The kernel acknowledges after its old process has left the live set. */
    uint32_t request, acknowledged, control_error;
    struct w32_core_row row[W32_CORE_ROWS];
};
_Static_assert(sizeof(struct w32_core_result)==232,"core result wire layout");
struct w32_core_field { const char *name; uint32_t expected; int compare; };
struct w32_core_case { const char *name; unsigned fields; struct w32_core_field field[W32_CORE_VALUES]; };
struct w32_core_schema { const char *probe; unsigned rows; struct w32_core_case row[W32_CORE_ROWS]; };
/* compare: 0 exact, -1 upper bound, +1 lower bound. Values are measurements,
 * not initializers for the guest result. The suite remains authoritative. */
static const struct w32_core_schema w32_core_schemas[W32_FIXTURE_COUNT]={
    {"w32-kernel32",0,{{0}}},
    {"w32-fullscreen",3,{
        {"indexed_640480",4,{{"width",640u,0},{"height",480u,0},{"source_bpp",8u,0},{"scale",1u,0}}},
        {"indexed_320200",4,{{"width",320u,0},{"height",200u,0},{"source_bpp",8u,0},{"scale",2u,0}}},
        {"lease",6,{{"former_writes",0u,0},{"stale_refused",1u,0},{"rollback",1u,0},{"restored",1u,0},{"input",1u,0},{"return_ms",2000u,-1}}},
    }},
    {"w32-registry",3,{
        {"registry_recovery",1,{{"checked",1u,0}}},
        {"registry_restart",1,{{"checked",1u,0}}},
        {"registry_writers",1,{{"checked",1u,0}}},
    }},
    {"w32-audio",1,{
        {"audio",4,{{"lead",512u,-1},{"streams",1u,0},{"pcm",1u,0},{"callbacks",1u,0}}},
    }},
    {"w32-gdi32",3,{
        {"gdi_dib",3,{{"formats",6u,0},{"rop3",256u,0},{"stretch_modes",4u,0}}},
        {"gdi_regions_palettes_text",3,{{"region_modes",5u,0},{"static_colors",20u,0},{"text_alignments",10u,1}}},
        {"gdi_teardown",3,{{"live_objects",0u,0},{"surface_bytes",0u,0},{"clean",1u,0}}},
    }},
    {"w32-crash",1,{
        {"crash",3,{{"classes",7u,1},{"files",8u,0},{"stack_words",8u,0}}},
    }},
    {"w32-user32",4,{
        {"user32_sends",1,{{"done",1u,0}}},
        {"user32_input",1,{{"done",1u,0}}},
        {"user32_paint",1,{{"done",1u,0}}},
        {"user32_teardown",1,{{"done",1u,0}}},
    }},
    {"w32-codecs",1,{
        {"codecs",3,{{"adpcm",1u,0},{"video",1u,0},{"dlls",8u,0}}},
    }},
    {"w32-ddraw",2,{
        {"ddraw_surfaces",1,{{"depths",3u,0}}},
        {"ddraw_loss_restore",3,{{"cycles",10u,0},{"retained_system",1u,0},{"leaks",0u,0}}},
    }},
    {"w32-input",6,{
        {"input_layouts",1,{{"done",1u,0}}},
        {"input_formats",1,{{"done",1u,0}}},
        {"input_buffering",1,{{"done",1u,0}}},
        {"input_cooperative",1,{{"done",1u,0}}},
        {"input_relative_warp",1,{{"done",1u,0}}},
        {"input_teardown",1,{{"done",1u,0}}},
    }},
};
static inline int w32_core_valid(const struct w32_core_result *r,unsigned id)
{
    return id>0 && id<W32_FIXTURE_COUNT && r->magic==W32_CORE_MAGIC &&
        r->version==1 && r->fixture==id && r->done==1 && !r->release &&
        (id==W32_FIXTURE_REGISTRY ? r->request<=2 && r->acknowledged<=r->request :
            !r->request && !r->acknowledged && !r->control_error) &&
        !(r->unsupported & ~((1u<<w32_core_schemas[id].rows)-1u));
}
static inline int w32_core_pass(const struct w32_core_result *r,unsigned id,uint32_t exit_code)
{
    if (!w32_core_valid(r,id) || r->unsupported || exit_code || r->control_error || r->request!=r->acknowledged) return 0;
    const struct w32_core_schema *s=&w32_core_schemas[id];
    for (unsigned i=0;i<s->rows;i++) {
        if (r->row[i].checked!=1 || r->row[i].errors) return 0;
        for (unsigned j=0;j<s->row[i].fields;j++) {
            const struct w32_core_field *f=&s->row[i].field[j];
            uint32_t v=r->row[i].value[j];
            if (f->compare<0 ? v>f->expected : f->compare>0 ? v<f->expected : v!=f->expected) return 0;
        }
    }
    return 1;
}
static inline char *w32_core_text(char *p,const char *s)
{ while (*s) *p++=*s++; return p; }
static inline char *w32_core_number(char *p,uint32_t n)
{
    char digits[10];unsigned used=0;
    do {digits[used++]=(char)('0'+n%10);n/=10;} while(n);
    while(used)*p++=digits[--used];
    return p;
}
static inline void w32_core_publish(const struct w32_core_result *r,unsigned id,
    void (*emit)(const char *,const char *,const char *,...))
{
    if (!w32_core_valid(r,id)) return;
    const struct w32_core_schema *s=&w32_core_schemas[id];
    for(unsigned i=0;i<s->rows;i++) {
        char line[256],*p=w32_core_text(line,"case=");
        p=w32_core_text(p,s->row[i].name);
        p=w32_core_text(p," errors=");p=w32_core_number(p,r->row[i].errors);
        p=w32_core_text(p," observed=");p=w32_core_number(p,r->row[i].checked);
        p=w32_core_text(p," unsupported=");p=w32_core_number(p,(r->unsupported>>i)&1u);
        for(unsigned j=0;j<s->row[i].fields;j++) {
            *p++=' ';p=w32_core_text(p,s->row[i].field[j].name);*p++='=';
            p=w32_core_number(p,r->row[i].value[j]);
        }
        *p=0;emit(s->probe,"DATA","%s",line);
    }
}
#endif
