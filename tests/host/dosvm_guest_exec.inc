/* F3-28: observe the real Quake EXEC and CRT in the host CPU replay.
 * No guest patching: stop only after the data-alias request has returned.
 * SPDX-License-Identifier: GPL-2.0-only */
static bool quake_exec_mode, quake_exec_checked, quake_reopened, quake_alias_returned;
static uint16_t quake_psp;
static uint16_t quake_alias_cs;
static uint32_t quake_alias_ip;
static unsigned quake_alias_calls;

static uint16_t exec_word(uint32_t at)
{
    uint8_t b[2];
    CHECK(!dosvm_read(gv, at, b, sizeof(b)));
    return b[0] | ((uint16_t)b[1] << 8);
}

static void quake_exec_observe(void)
{
    if (!quake_exec_mode || frame_pm() || (uint16_t)cpu.tf.eax != 0x3000) return;
    uint8_t magic[16];
    CHECK(!dosvm_read(gv, cpu.tf.cs << 4, magic, sizeof(magic)));
    if (memcmp(magic, "go32stub, v 2.00T", sizeof(magic))) return;
    CHECK(!quake_exec_checked);
    quake_exec_checked = true;
    quake_psp = (uint16_t)cpu.es;
    uint32_t p = (uint32_t)quake_psp << 4;
    uint16_t parent = exec_word(p + 0x16), end = exec_word(p + 2);
    uint16_t env = exec_word(p + 0x2C), arena = exec_word(p - 16 + 3);
    uint32_t header = (cpu.tf.cs << 4) - 512;
    CHECK(exec_word(header) == 0x5A4D && exec_word(header + 8) == 32);
    CHECK(exec_word(header + 10) == 0x28 && exec_word(header + 12) == 0xFFFF);
    CHECK(cpu.tf.user_ss == cpu.tf.cs && (uint16_t)cpu.tf.user_esp == 0x770);
    uint16_t top = exec_word(0x413) << 6, ebda = exec_word(0x40E);
    if (top > 0xA000) top = 0xA000;
    if (ebda >= 0x5201 && ebda < top) top = ebda;
    /* This fixture has no high parent allocations: MAXALLOC=FFFF must own
     * the whole remaining interval, including the low transfer buffer. */
    CHECK(exec_word(p) == 0x20CD && cpu.tf.cs == quake_psp + 16);
    CHECK(exec_word(p - 16 + 1) == quake_psp && end == quake_psp + arena && end == top);
    CHECK(arena >= 16 + (2048 - 512) / 16 + 0x28);
    CHECK(parent != quake_psp && exec_word(((uint32_t)parent << 4) - 16 + 1) == parent);
    CHECK(parent + exec_word(((uint32_t)parent << 4) - 16 + 3) < quake_psp);
    CHECK(exec_word(p + 0x32) == 20 && exec_word(p + 0x34) == 0x18 && exec_word(p + 0x36) == quake_psp);
    CHECK(exec_word(((uint32_t)env << 4) - 16 + 1) == quake_psp);
    uint8_t environment[256];
    unsigned env_bytes = exec_word(((uint32_t)env << 4) - 16 + 3) * 16u;
    CHECK(env_bytes <= sizeof(environment) && env_bytes > 4);
    CHECK(!dosvm_read(gv, (uint32_t)env << 4, environment, env_bytes));
    unsigned i;
    for (i = 0; i + 1 < env_bytes; i++) if (!environment[i] && !environment[i + 1]) break;
    i += 2;
    static const char path[] = "C:\\QUAKE\\QUAKE.EXE";
    CHECK(i + 2 + sizeof(path) <= env_bytes && environment[i] == 1 && environment[i + 1] == 0);
    CHECK(!memcmp(environment + i + 2, path, sizeof(path)));
    uint8_t tail[18];
    CHECK(!dosvm_read(gv, p + 0x80, tail, sizeof(tail)));
    static const char args[] = " +playdemo demo1\r";
    CHECK(tail[0] == sizeof(args) - 2 && !memcmp(tail + 1, args, sizeof(args) - 1));
    printf("f3-28 EXEC: PSP=%04x parent=%04x image=%04x end=%04x paras=%04x min=0028 max=ffff env=%04x count=1 path=%s tail=PASS\n",
           quake_psp, parent, cpu.tf.cs, end, arena, env, path);
}

static void quake_exec_trap(const struct gcpu_event *ev)
{
    if (!quake_exec_mode || ev->vector != 13) return;
    bool pm = frame_pm();
    uint32_t lin = pm ? dpmi_fault_linear(gv, &cpu) : (cpu.tf.cs << 4) + (uint16_t)cpu.tf.eip;
    uint8_t op[2];
    CHECK(!dosvm_read(gv, lin, op, sizeof(op)));
    if (op[0] != 0xCD) return;
    if (!pm && op[1] == 0x21) {
        quake_exec_observe();
        if (quake_exec_checked && cpu.tf.cs == quake_psp + 16 && (uint16_t)cpu.tf.eax == 0x3D00) {
            char path[19];
            CHECK(!dosvm_read(gv, (cpu.ds << 4) + (uint16_t)cpu.tf.edx, path, sizeof(path)));
            CHECK(!strcmp(path, "C:\\QUAKE\\QUAKE.EXE"));
            quake_reopened = true;
        }
    }
    if (pm && op[1] == 0x31 && (uint16_t)cpu.tf.eax == 0x000A) {
        CHECK(quake_exec_checked && quake_reopened && !quake_alias_calls);
        struct dpmi_seg source;
        CHECK(cpu.tf.ebx == cpu.tf.ds && dpmi_seg(gv, cpu.tf.ds, &source) && !source.code);
        quake_alias_cs = cpu.tf.cs;
        quake_alias_ip = cpu.tf.eip + 2;
        quake_alias_calls++;
    }
}

static bool quake_exec_done(void)
{
    return quake_alias_returned || gv->info.verdict_done;
}
