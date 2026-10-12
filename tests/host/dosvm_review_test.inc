/* f3-08 regressions through production trap/burst paths. No guest CPU claimed. */
static void review_boundary(struct dosvm *v, uint32_t ip)
{
    cur.tf.eip = ip;
    step(v, 0x20, 0);                 /* actual trap address is a known boundary */
    CHECK(!left && in_pm());
}
static void test_review_flags_step(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true);
        v86_start(); enter(v, bits != 0);
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        static const uint8_t code[] = {0x9C, 0x58, 0xFA, 0x50, 0x9D, 0x9D, 0x90};
        CHECK(!dosvm_write(v, base + 0x200, code, sizeof(code)));
        cur.tf.eflags = V86_IF | 2;
        cur.tf.eax = V86_IF | 2;
        review_boundary(v, 0x200); cur.tf.eip = 0x202;
        step(v, 13, 0);
        CHECK(v->dpmi.if_armed && !v->monitor.vif && !v->dpmi.dr7);
        /* First POPF loads the armed image; second POPF/IRET clears TF.
         * The CPU still reports BS after that instruction (also after the
         * instruction following MOV/POP SS). Supply those saved frames. */
        for (unsigned form = 0; form < 4; form++) {
            if (form) {
                v->monitor.vif = true; cur.tf.eax = V86_IF | 2;
                review_boundary(v, 0x200); cur.tf.eip = 0x202; step(v, 13, 0);
            }
            cur.tf.eip = form == 2 ? 0x207 : 0x206;
            cur.tf.eflags &= ~V86_TF;
            if (form == 3) {
                v->dpmi.watch[0] = (struct dpmi_watch){.used=true, .owner=1, .type=1, .size=1};
                v->dpmi.dr7 = 1;
            }
            debug_dr6 = (1u << 14) | (form == 3 ? 1u : 0u);
            step(v, 1, 0);
            CHECK(!left && v->monitor.vif && !v->dpmi.if_armed && !debug_dr6 && !(cur.tf.eflags & V86_TF));
            if (form == 3) CHECK(v->dpmi.watch[0].hit);
        }
        unsigned restored = v->dpmi.stats.if_restored;
        /* An unrelated subsequent debug trap must reach the client. */
        v->dpmi.exc[1] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x400};
        cur.tf.eflags |= V86_TF;
        debug_dr6 = 1u << 14; step(v, 1, 0);
        CHECK(!left && in_pm() && cur.tf.eip == 0x400 && v->dpmi.stats.if_restored == restored && v->dpmi.stats.exceptions > 0);
        v->frame_pm = true; destroy(v, true); empty();
    }
    puts("f3-08 flags step: BS with TF clear, DR7=0, delayed SS step and disarm PASS");
}
static void test_review_flags_boundaries(void)
{
    struct dosvm *v = dpmi_vm(true);
    v86_start(); enter(v, false);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    static const uint8_t immediate[][8] = {
        {0xBB, 0x9C, 0x58, 0xFA},             /* MOV BX,589Ch; CLI */
        {0xBB, 0x02, 0x9C, 0xFA},             /* MOV BX,9C02h; CLI */
        {0xC7, 0x06, 0x00, 0x01, 0x9C, 0x58, 0xFA}, /* memory immediate */
        {0x66, 0xBB, 0x02, 0x00, 0x9C, 0x58, 0xFA}, /* 32-bit immediate */
        {0x67, 0xC7, 0x44, 0x8D, 0x04, 0x9C, 0x58, 0xFA} /* SIB/displacement/immediate */
    };
    static const unsigned sizes[] = {4, 4, 7, 7, 8};
    for (unsigned i = 0; i < 5; i++) {
        CHECK(!dosvm_write(v, base + 0x300, immediate[i], sizes[i]));
        cur.tf.eflags = V86_IF | 2; cur.tf.eax = 0xCAFE0202;
        pm_push(v, V86_IF | 2); uint32_t sp = cur.tf.user_esp;
        v->monitor.vif = true; review_boundary(v, 0x300); cur.tf.eip = 0x300 + sizes[i] - 1;
        step(v, 13, 0);
        CHECK(!left && !v->monitor.vif && cur.tf.eax == 0xCAFE0202 && cur.tf.user_esp == sp && !v->dpmi.if_armed);
        CHECK(pm_pop(v) == (V86_IF | 2));
    }
    /* Positive prefixed, 32-bit flags image in a 16-bit code segment. */
    static const uint8_t good[] = {0x66, 0x9C, 0x66, 0x58, 0xFA};
    CHECK(!dosvm_write(v, base + 0x300, good, sizeof(good)));
    v->monitor.vif = true; cur.tf.eax = V86_IF | 2; review_boundary(v, 0x300);
    cur.tf.eip = 0x304; step(v, 13, 0);
    CHECK(!left && v->dpmi.if_armed && cur.tf.eax == (V86_IF | V86_TF | 2));
    v->frame_pm = true; destroy(v, true); empty();
    puts("f3-08 flags boundaries: register/memory/prefixed immediates untouched, decoded save armed PASS");
}
static void test_review_burst_boundary(void)
{
    struct dosvm *v = create();
    struct v86_frame mode = vf(0x13, 0, 0, 0);
    CHECK(!dosvm_vga_bios(v, &mode));
    /* Planar read/write, rotate 1 and XOR: repeating a MOVS element changes
     * the byte again. A failed probe must also leave the latches alone. */
    vga_out(v, 0x3C4, 4); vga_out(v, 0x3C5, 6);
    vga_out(v, 0x3C4, 2); vga_out(v, 0x3C5, 1);
    vga_out(v, 0x3CE, 5); vga_out(v, 0x3CF, 0);
    vga_out(v, 0x3CE, 6); vga_out(v, 0x3CF, 1); /* A0000-BFFFF */
    vga_out(v, 0x3CE, 3); vga_out(v, 0x3CF, 0x19);
    static const uint8_t code[] = {0xF3, 0xA4, 0xCB};
    CHECK(!dosvm_write(v, 0x30000, code, sizeof(code)));
    for (int df = 1; df >= 0; df--) {
        for (unsigned width = 0; width < 2; width++) {
            uint8_t op = width ? 0xAB : 0xA4; /* crossing word or repeated byte */
            CHECK(!dosvm_write(v, 0x30001, &op, 1));
            struct v86_frame f = vf(1, 0, 2, 0);
            f.tf.cs = 0x3000; f.es = f.ds = df ? 0x9FFF : 0xBFFF;
            f.tf.esi = f.tf.edi = df ? 0x10 : 0xF;
            f.tf.eflags |= df ? 0x400 : 0;
            cvx_cpu c = {0}; c.seg_base[CVX_CS] = 0x30000;
            c.seg_base[CVX_ES] = c.seg_base[CVX_DS] = f.es << 4;
            v->vga.plane[0][0] = 1;
            v->vga.plane[0][0xFFFF] = 1;
            memset(v->vga.latch, 0x5A, sizeof(v->vga.latch));
            cvga_state *before = malloc(sizeof(*before)); CHECK(before); *before = v->vga;
            struct v86_frame saved = f;
            video_burst(v, &f, &c);
            CHECK(!memcmp(&saved, &f, sizeof(f)) && !memcmp(before, &v->vga, sizeof(*before)));
            /* The first instruction now runs once through the fault path,
             * including RAM below 9FFFFh or above BFFFFh. */
            if (!width) {
                if (!df) f.tf.ecx = 1; /* next destination is ROM, native fault */
                CHECK(!dosvm_vga_fault(v, &f) && !(f.tf.ecx & 0xFFFF));
                if (df) CHECK(v->vga.plane[0][0] == 0x81);
            }
            free(before);
        }
    }
    /* B0000h is outside the selected model aperture but still within the
     * trapped VGA window: preflight must not confuse those boundaries. */
    uint8_t stosb = 0xAA; CHECK(!dosvm_write(v, 0x30001, &stosb, 1));
    vga_out(v, 0x3CE, 6); vga_out(v, 0x3CF, 5); /* A0000-AFFFF */
    struct v86_frame f = vf(0x33, 0, 2, 0); f.tf.cs = 0x3000; f.es = 0xAFFF; f.tf.edi = 0xF;
    cvx_cpu c = {0}; c.seg_base[CVX_CS] = 0x30000; c.seg_base[CVX_ES] = 0xAFFF0;
    video_burst(v, &f, &c); CHECK(!(f.tf.ecx & 0xFFFF) && (f.tf.edi & 0xFFFF) == 0x11);
    destroy(v, true); empty();
    puts("f3-08 burst boundaries: DF both ways, REP and word refusal leave planes/latches/frame unchanged PASS");
}
static void test_review_burst_forms(void)
{
    struct dosvm *v = create();
    static const uint8_t invalid[][5] = {
        {0xF0, 0x89, 0xD8}, {0xF0, 0xF3, 0xAA},
        {0xF3, 0xF0, 0xA4}, {0xC7, 0xC8, 0x01, 0x00},
        {0xF0, 0x01, 0xD8}
    };
    static const unsigned sizes[] = {3, 3, 3, 4, 3};
    for (unsigned i = 0; i < 5; i++) {
        CHECK(!dosvm_write(v, 0x30000, invalid[i], sizes[i]));
        struct v86_frame f = vf(0, 0x1234, 1, 0); f.tf.cs = 0x3000; f.es = 0xA000;
        cvx_cpu c = {0}; c.seg_base[CVX_CS] = 0x30000; c.seg_base[CVX_ES] = 0xA0000;
        c.gpr[CVX_EBX] = 0x1234;
        cvx_cpu saved = c; cvx_result result;
        CHECK(cvx_execute(&c, &v->video_bus, 1, &result) == CVX_UNSUPPORTED && !memcmp(&saved, &c, sizeof(c)));
        struct v86_frame before = f; video_burst(v, &f, &c); CHECK(!memcmp(&before, &f, sizeof(f)));
    }
    destroy(v, true); empty();
    puts("f3-08 burst forms: LOCK MOV/string/ALU-register and invalid group rejected before execution PASS");
}
static void test_review_vm(void)
{
    const char *only = getenv("F3_REVIEW");
    if (!only || !strcmp(only, "1")) test_review_flags_step();
    if (!only || !strcmp(only, "2")) test_review_flags_boundaries();
    if (!only || !strcmp(only, "3")) test_review_burst_boundary();
    if (!only || !strcmp(only, "4")) test_review_burst_forms();
}
