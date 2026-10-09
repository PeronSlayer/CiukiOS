/* Explicit post-boot checks, on the still-bound desktop in SysVM0.
 * See docs/design/display-s3-qualification.md. Never change the video mode. */
#define S3QUAL_LOG_PATH "\\SYSTEM\\VIDEO\\S3QUAL.LOG"
static u8 qualify_log[400];
static int qualify_step, qualify_probe, qualify_wait, qualify_requested;
static u32 qualify_binding[6];
static char qualify_status[96];
static int qualify_mach64;
static const char *qualify_progress[4] = {
    "Checking the graphics adapter (1 of 4)...",
    "Preparing acceleration (2 of 4)...",
    "Checking drawing (3 of 4)...",
    "Checking 3D acceleration (4 of 4)..."
};

static int qualify_adapter(void)
{
    return native_ok && (((nfield(60) & 0x80000000UL) &&
        nfield(76) == 0x8C2E5333UL) || ((nfield(60) & 0x40000000UL) &&
        nfield(76) == 0x4C4D1002UL));
}
static int qualify_same_binding(void)
{
    int i;
    for (i = 0; i < 4; ++i)
        if (qualify_binding[i] != nfield(16 + i * 4)) return 0;
    /* CVGD: 40 is a changing completion counter; binding is at 44/48. */
    return qualify_binding[4] == nfield(44) && qualify_binding[5] == nfield(48);
}
static int qualify_write(void)
{
    int h, ok;
    h = dos_create(qualify_mach64 ? "\\SYSTEM\\VIDEO\\ATIQUAL.LOG" : S3QUAL_LOG_PATH);
    if (h < 0) return 0;
    ok = dos_write(h, qualify_log, sizeof qualify_log) == sizeof qualify_log;
    if (dos_close(h) < 0) ok = 0;
    return ok;
}
static void qualify_stop(const char *message)
{
    qualify_step = qualify_probe = qualify_wait = qualify_requested = 0;
    str_copy(qualify_status, message);
    app_log("[DISPLAY] acceleration", message);
}
static int qualify_begin(int forced)
{
    int i;
    qualify_requested = 0;
    if (qualify_step || preview) return 1;
    native_read();
    if (!native_ok) {
        qualify_stop("Hardware acceleration is unavailable in this display mode.");
        return 1;
    }
    if (nfield(56)) {
        qualify_stop("Close DOS windows before enabling acceleration.");
        msgbox(&notice, "Display settings", qualify_status, "OK");
        return 1;
    }
    if (!session_query_lfb()) {
        qualify_stop("Hardware acceleration is unavailable in this display mode.");
        return 1;
    }
    qualify_mach64 = (nfield(60) & 0x40000000UL) && nfield(76) == 0x4C4D1002UL;
    if (nfield(8) == 3 || nfield(8) == 4) {
        qualify_stop(nfield(64) == 1 && ((nfield(60) & 7) == 5 ||
            (!qualify_mach64 && (nfield(60) & 7) == 7)) ?
            "Hardware acceleration is already enabled." :
            "The graphics adapter is unavailable. Restart before trying again.");
        return 1;
    }
    if (!forced && !qualify_adapter()) {
        qualify_stop("Hardware acceleration is unavailable for this adapter.");
        return 1;
    }
    mem_set(qualify_log, 0, sizeof qualify_log);
    mem_copy(qualify_log, "CG3D", 4);
    *(u16 *)(qualify_log + 4) = 0x0100;
    *(u16 *)(qualify_log + 6) = 16;
    for (i = 0; i < 4; ++i) qualify_binding[i] = nfield(16 + i * 4);
    qualify_binding[4] = nfield(44); qualify_binding[5] = nfield(48);
    page = 3;
    qualify_probe = 0;
    qualify_step = qualify_wait = 1;
    str_copy(qualify_status, qualify_progress[0]);
    app_log("[DISPLAY] acceleration", "preparing");
    return 1;                       /* paint pending status before any call */
}
static int qualify_poll(void)
{
    struct regs r;
    int failed, step = qualify_step;
    u32 detail = (u32)qualify_probe << 16;
    u16 error;
    char code[5];
    if (!step) return 0;
    if (qualify_wait) { qualify_wait = 0; return 2; }
    native_read();
    if (!native_ok) {
        qualify_stop("The graphics adapter stopped reporting its state.");
        return 1;
    }
    if (nfield(56)) {
        qualify_stop("Close DOS windows before enabling acceleration.");
        return 1;
    }
    if (preview || !session_query_lfb() || !qualify_same_binding()) {
        qualify_stop("The display mode changed. Start acceleration setup again.");
        return 1;
    }
    mem_copy(qualify_log + 16, native, sizeof native);
    mem_set(qualify_log + 208, 0, sizeof native);
    *(u32 *)(qualify_log + 8) = detail | 0x0100 | step;
    *(u32 *)(qualify_log + 12) = 0;
    if (!qualify_write()) {
        qualify_stop("Cannot save acceleration diagnostics. Check the system disk.");
        msgbox(&notice, "Display settings", qualify_status, "OK");
        return 1;
    }
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0118; r.bx = qualify_probe ? 0x0100 | qualify_probe : step;
    r.ds = r.es = app_seg();
    failed = far_regs(vm_seg, vm_off, &r) || (r.flags & 1);
    error = failed ? r.ax : 0;
    native_read();
    if (native_ok) mem_copy(qualify_log + 208, native, sizeof native);
    if (!failed && (!native_ok || (step >= 4 &&
        (nfield(8) != (qualify_mach64 ? 4UL : 3UL) || nfield(64) != 1 ||
         (nfield(60) & 7) != (qualify_mach64 || step == 5 ? 5UL : 7UL))))) {
        failed = 1; error = 9;        /* successful call needs a valid readback */
    }
    *(u32 *)(qualify_log + 8) = detail | (failed ? 0x0300 : 0x0200) | step;
    *(u32 *)(qualify_log + 12) = error;
    if (!qualify_write()) {
        qualify_stop("The acceleration result could not be saved. Check the system disk.");
        msgbox(&notice, "Display settings", qualify_status, "OK");
        return 1;
    }
    if (failed) {
        /* Only a safely restored 3D mismatch with completed packed 2D
         * proofs may request the separate 2D bind. Never retry a timeout
         * or retained engine, and never publish a triangle capability. */
        if (!qualify_mach64 && step == 4 && native_ok &&
            nfield(8) == 0 && nfield(72) == 9 && nfield(156) == 0 &&
            nfield(68) == 8 && nfield(140) && nfield(188)) {
            qualify_step = 5; qualify_probe = 0;
            str_copy(qualify_status, "Enabling verified hardware 2D acceleration...");
            ui_repaint_win(WIN_DISPLAY);
            return 2;
        }
        qualify_stop("Acceleration could not be enabled. Code ");
        fmt_hex4(code, error); str_cat(qualify_status, code);
        app_log("[DISPLAY] acceleration failed", code);
        msgbox(&notice, "Display settings", qualify_status, "OK");
        return 1;
    }
    if (step >= 4) {
        qualify_stop(step == 5 ?
            "Hardware 2D acceleration enabled; 3D verification did not pass." :
            "Hardware acceleration enabled for this desktop session.");
        return 1;
    }
    if (step == 1) { qualify_step = 2; qualify_probe = qualify_mach64 ? 0 : 1; }
    else if (qualify_probe) {
        if (++qualify_probe > 21) qualify_probe = 0;
    } else qualify_step++;
    str_copy(qualify_status, qualify_mach64 && qualify_step == 4 ?
        "Checking graphics state (4 of 4)..." : qualify_progress[qualify_step - 1]);
    ui_repaint_win(WIN_DISPLAY);
    return 2;                       /* yield to input; continue without HLT */
}
static void qualify_cancel(void)
{
    if (qualify_step || qualify_requested)
        qualify_stop("Acceleration setup cancelled.");
}
