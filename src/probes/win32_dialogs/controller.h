/* f5-10C2 w32-dialogs probe: one result block shared by the PE fixture
 * (writer), the kernel observer (reader, CIUKI_TEST framing) and the host
 * dry-run. The kernel reads the block from the fixture's address space after
 * validating the process identity; application stdout is never evidence.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_W32_DIALOGS_CONTROLLER_H
#define CIUKI_W32_DIALOGS_CONTROLLER_H
#include <stdint.h>
/* Private fixture/controller address agreement: one 4 KiB page at a 64 KiB
 * boundary in the application region. The authored EXE is at 0x00400000,
 * kernel stacks/TEB/PEB use bottom-up placement, and loader metadata/heaps
 * use the top of the arena. 0x6fff0000 belongs to that loader metadata by
 * entry time. The production-loader host regression checks this page is
 * free after startup; keep that check when the fixture's layout changes. */
#define W32_DIALOGS_RESULT_VA 0x10000000u
#define W32_DIALOGS_MAGIC 0x474c4457u /* "WDLG" */
#define W32_DIALOGS_VERSION 1u
#define W32_DIALOGS_PATH "/System/Win32/Fixtures/dialogs-exercise.exe"
struct w32_dialogs_result {
    uint32_t magic, version, done, release;
    uint32_t errors, first_error; /* source line of the first failed check */
    uint32_t system_x, system_y, dialog_x, dialog_y, units_errors;
    uint32_t classes, controls, controls_errors;
    uint32_t steps, keyboard_errors;
    uint32_t menu_items, menu_errors;
    uint32_t msgbox_result, msgbox_errors;
    uint32_t total_peak, glyph_peak, face_peak, hits, misses, evictions;
    uint32_t bridge_glyphs, bridge_refusals, bridge_errors;
    uint32_t service_peak, service_budget, service_glyphs, font_errors;
    uint32_t live_windows, live_menus, teardown_errors;
};
_Static_assert(sizeof(struct w32_dialogs_result) == 144, "w32-dialogs result block");
static inline int w32_dialogs_valid(const struct w32_dialogs_result *r)
{
    return r->magic == W32_DIALOGS_MAGIC && r->version == W32_DIALOGS_VERSION && r->done == 1;
}
/* Every record stays well below the 240-byte CIUKI_TEST line limit. */
static inline void w32_dialogs_publish(const struct w32_dialogs_result *r, uint32_t exit_code,
    void (*emit)(const char *, const char *, const char *, ...))
{
    const char *p = "w32-dialogs";
    emit(p, "DATA", "case=dialog_units system_x=%u system_y=%u dialog_x=%u dialog_y=%u errors=%u",
         r->system_x, r->system_y, r->dialog_x, r->dialog_y, r->units_errors);
    emit(p, "DATA", "case=dialog_controls classes=%u controls=%u errors=%u",
         r->classes, r->controls, r->controls_errors);
    emit(p, "DATA", "case=dialog_keyboard steps=%u errors=%u", r->steps, r->keyboard_errors);
    emit(p, "DATA", "case=dialog_menu items=%u errors=%u", r->menu_items, r->menu_errors);
    emit(p, "DATA", "case=dialog_msgbox result=%u errors=%u", r->msgbox_result, r->msgbox_errors);
    emit(p, "DATA", "case=font_bridge glyphs=%u service_glyphs=%u refusals=%u errors=%u",
         r->bridge_glyphs, r->service_glyphs, r->bridge_refusals, r->bridge_errors);
    emit(p, "DATA", "case=font_budget total_peak=%u glyph_peak=%u face_peak=%u service_peak=%u service_budget=%u hits=%u misses=%u errors=%u",
         r->total_peak, r->glyph_peak, r->face_peak, r->service_peak, r->service_budget, r->hits, r->misses,
         r->font_errors);
    emit(p, "DATA", "case=dialog_teardown live_windows=%u live_menus=%u exit=%u errors=%u first_error=%u",
         r->live_windows, r->live_menus, exit_code, r->teardown_errors + r->errors, r->first_error);
}
#endif
