/* Production transport/decoders with independent byte fixtures. GPL-2.0-only */
struct touchpad_events {
    unsigned absolute, down, up, count;
    int sample[5], x, y, wheel;
};

static struct touchpad_events touchpad_events(void)
{
    struct touchpad_events result = {0};
    struct input_event e;
    unsigned next_axis = 0;
    while (input_read(&e)) {
        result.count++;
        CHECK(e.source == INPUT_NATIVE && e.generation == native.stats.generation);
        if (e.type == INPUT_ABS) {
            CHECK(e.code == next_axis);
            result.sample[next_axis] = e.value;
            next_axis = (next_axis + 1) % 5;
            result.absolute++;
        } else if (e.type == INPUT_REL) {
            if (e.code == INPUT_X) result.x += e.value;
            if (e.code == INPUT_Y) result.y += e.value;
            if (e.code == INPUT_WHEEL) result.wheel += e.value;
        } else if (e.type == INPUT_BTN) {
            if (e.value) result.down |= 1u << e.code;
            else result.up |= 1u << e.code;
        } else CHECK(e.type == INPUT_RESYNC);
    }
    CHECK(!next_axis);
    return result;
}

static void touchpad_boot(unsigned kind, unsigned version)
{
    reset_native();
    hw.touchpad_kind = kind;
    if (kind == 1) {
        hw.syn_model = version ? 0x80 : 0;
        hw.syn_caps = 0x804700 | (version ? 0x83 : 0);
    } else hw.alps_model = version;
    CHECK(!i8042_init() && native.stats.active && !native.stats.quarantined);
    CHECK(!hw.resets && !hw.count && !native.queue.stats.pending);
}

/* Synthetic coordinates encode the documented bit fields independently. */
static void touchpad_position(unsigned protocol, int x, int y, int pressure, unsigned width, unsigned buttons)
{
    uint8_t p[6];
    if (protocol == TOUCHPAD_SYNAPTICS_NEW) {
        p[0] = 0x80 | ((width & 12) << 2) | ((width & 2) << 1) | (buttons & 3);
        p[1] = ((x >> 8) & 15) | ((y >> 4) & 0xf0);
        p[2] = pressure;
        p[3] = 0xc0 | ((x >> 8) & 0x10) | ((y >> 7) & 0x20) | ((width & 1) << 2);
        p[4] = x; p[5] = y;
    } else if (protocol == TOUCHPAD_SYNAPTICS_OLD) {
        p[0] = 0xc0 | ((pressure >> 2) & 0x30) | ((width & 2) << 1) | (buttons & 3);
        p[1] = ((x >> 8) & 31) | ((width & 8) << 4); p[2] = x;
        p[3] = 0x80 | (pressure & 63); p[4] = (y >> 8) & 31; p[5] = y;
    } else if (protocol == TOUCHPAD_ALPS_V1) {
        p[0] = 0x88 | ((x >> 7) & 7); p[1] = x & 127;
        p[2] = ((buttons & 1) << 4) | ((buttons & 2) << 2);
        p[3] = (y >> 7) & 7; p[4] = y & 127; p[5] = pressure;
    } else {
        p[0] = native.touchpad.byte0; p[1] = x & 127;
        p[2] = (x >> 4) & 0x78; p[3] = ((y >> 3) & 0x70) | 8 | (buttons & 7);
        p[4] = y & 127; p[5] = pressure;
    }
    native_packet(p, 6);
}

static void test_touchpad_protocols(void)
{
    _Static_assert(INPUT_KEY == 1 && INPUT_RESYNC == 5 && INPUT_ABS == 6, "internal event compatibility");
    _Static_assert(sizeof(struct i8042_stats) == 80, "host stats unchanged");
    /* The exact old/new and v1/v2 wire fixtures must agree with the corpus. */
    const uint8_t packets[][6] = {
        {0xc0,0x07,0xd0,0xa8,0x07,0xd0},
        {0x90,0x77,0x28,0xc0,0xd0,0xd0},
        {0x8a,0x2c,0,0x02,0x2c,0x28},
        {0xf8,0x2c,0x10,0x28,0x2c,0x28},
    };
    for (unsigned version = 0; version < 4; version++) {
        touchpad_boot(version < 2 ? 1 : 2, version < 2 ? version : version - 2);
        unsigned protocol = version + 1;
        CHECK(native.touchpad.info.protocol == protocol);
        struct touchpad_info info;
        i8042_touchpad_snapshot(&info);
        CHECK(info.protocol == protocol && info.x_max > info.x_min && info.y_max > info.y_min);
        native_packet(packets[version], 6);
        struct touchpad_events e = touchpad_events();
        CHECK(e.absolute == 5 && !e.x && !e.y && !e.down);
        CHECK(e.sample[0] == (version < 2 ? 2000 : 300));
        CHECK(e.sample[1] == (version < 2 ? 3856 : 300) && e.sample[2] == 40 && e.sample[3] == 1);
        g_ticks += 10;
        touchpad_position(protocol, version < 2 ? 2020 : 310, version < 2 ? 1980 : 310, 40, 4, 0);
        e = touchpad_events();
        CHECK(e.x > 0 && e.y > 0 && !e.wheel);
        g_ticks += 10;
        touchpad_position(protocol, 0, 0, 0, 4, 0);
        e = touchpad_events();
        CHECK(e.down == 1 && e.up == 1 && !e.x && !e.y); /* one short tap */
        /* Edge scroll must have no pointer motion or tap on lift. */
        touchpad_position(protocol, info.x_max - 5, version < 2 ? 2000 : 300, 40, 4, 0);
        touchpad_events(); g_ticks += 10;
        touchpad_position(protocol, info.x_max - 5, version < 2 ? 2200 : 200, 40, 4, 0);
        e = touchpad_events();
        CHECK(e.wheel > 0 && !e.x && !e.y);
        touchpad_position(protocol, 0, 0, 0, 4, 0);
        e = touchpad_events(); CHECK(!e.down && !e.up);
        /* A physical press cancels tapping; a palm never moves/clicks. */
        touchpad_position(protocol, version < 2 ? 2000 : 300, version < 2 ? 2000 : 300, 40, 4, 1);
        e = touchpad_events(); CHECK(e.down == 1);
        touchpad_position(protocol, 0, 0, 0, 4, 0);
        e = touchpad_events(); CHECK(!e.down && e.up == 1);
        touchpad_position(protocol, info.x_max, info.y_max, version < 2 ? 220 : 125, 4, 0);
        e = touchpad_events(); CHECK(!e.x && !e.y && !e.down && !e.sample[3]);
        /* Incomplete packet gap/parity loss cancels tap and coordinate history. */
        native_packet(packets[version], 2); g_ticks += 501;
        native_packet(packets[version], 6);
        e = touchpad_events(); CHECK(e.absolute == 5 && !e.x && !e.y && native.queue.stats.resync);
        capture_at(&native, 0xa1, 0, g_ticks);
        native_packet(packets[version], 6);
        e = touchpad_events(); CHECK(e.absolute == 5 && !e.x && !e.y && native.queue.stats.errors);
        /* Corrupt framing and retain suffix headers; subsequent packet reanchors. */
        const uint8_t bad[] = {0,0x40,0x40}; native_packet(bad, sizeof(bad));
        native_packet(packets[version], 6); native_packet(packets[version], 6);
        touchpad_events(); CHECK(!native.queue.mouse_pos);
        touchpad_position(protocol, 0, 0, 0, 4, 0);
        e = touchpad_events(); CHECK(!e.down); /* resync cannot synthesize a tap */
        uint32_t random = 0x8042u + version;
        for (unsigned n = 0; n < 2000; n++) {
            random = random * 1664525u + 1013904223u;
            uint8_t byte = random >> 24;
            g_ticks++;
            native_packet(&byte, 1);
            struct input_event event;
            while (input_read(&event)) {
                CHECK(event.type >= INPUT_KEY && event.type <= INPUT_ABS);
                if (event.type == INPUT_ABS) CHECK(event.code <= TOUCHPAD_WIDTH);
            }
        }
        /* Detectable loss always permits a clean bounded reanchor. */
        capture_at(&native, 0xa1, 0, g_ticks);
        native_packet(packets[version], 6);
        e = touchpad_events(); CHECK(e.absolute == 5 && !e.x && !e.y);
    }
    /* Capability-proven two-finger scroll, pressure hysteresis and long hold. */
    touchpad_boot(1, 1);
    touchpad_position(2, 2000, 2000, 40, 0, 0); touchpad_events();
    touchpad_position(2, 2000, 2200, 40, 0, 0);
    struct touchpad_events e = touchpad_events(); CHECK(e.wheel > 0 && !e.x && !e.y && e.sample[3] == 2);
    touchpad_position(2, 2000, 2200, 0, 4, 0); touchpad_events();
    touchpad_position(2, 2000, 2000, 31, 4, 0); touchpad_events();
    touchpad_position(2, 2000, 2000, 26, 4, 0);
    e = touchpad_events(); CHECK(e.sample[3] == 1);
    g_ticks += 181;
    touchpad_position(2, 2000, 2000, 0, 4, 0);
    e = touchpad_events(); CHECK(!e.down);
    /* Tunnel separates stick state and never replaces the pad baseline. */
    touchpad_position(2, 2000, 2000, 40, 4, 1); touchpad_events();
    const uint8_t stick[] = {0x84,0x09,0,0xc4,2,1};
    native_packet(stick, 6); e = touchpad_events();
    CHECK(!e.absolute && e.x == 2 && e.y == -1 && !e.up && !e.down);
    touchpad_position(2, 2020, 2000, 40, 4, 0); e = touchpad_events();
    CHECK(e.x == 4 && !e.up && native.queue.stats.buttons == 1);
    const uint8_t release[] = {0x84,0x08,0,0xc4,0,0};
    native_packet(release, 6); e = touchpad_events(); CHECK(e.up == 1 && !e.absolute);
    /* Every audited ALPS v1/v2 signature variant selects by query, not DMI. */
    for (unsigned i = 0; i < 21; i++) {
        touchpad_boot(2, i);
        CHECK(native.touchpad.info.protocol == (i ? TOUCHPAD_ALPS_V2 : TOUCHPAD_ALPS_V1));
    }
    touchpad_boot(2, 1);
    touchpad_position(4, 300, 300, 40, 4, 0); touchpad_events();
    const uint8_t bare[] = {0x09,2,1}; native_packet(bare, 3);
    e = touchpad_events(); CHECK(!e.absolute && e.x == 2 && e.y == -1 && e.down == 1);
    touchpad_position(4, 310, 300, 40, 4, 0);
    e = touchpad_events(); CHECK(e.x == 10 && e.up == 1); /* combined ALPS snapshot releases shared button */
    /* v2 pressure=127 is stick motion with signed extended coordinates. */
    touchpad_position(4, 766, 511, 127, 4, 0);
    e = touchpad_events(); CHECK(e.x == -2 && e.y == 1 && !e.absolute && !e.up);
    touchpad_boot(2, 1);
    native_packet(bare, 3); e = touchpad_events();
    CHECK(e.x == 2 && e.y == -1 && e.down == 1 && !e.absolute); /* stick works before first pad contact */
    /* Simultaneous nine-byte interleaving with arbitrary PS/2 data high bits. */
    touchpad_boot(2, 4);
    const uint8_t interleaved[] = {0xcf,0x2c,0x10,0x3f,0xfe,0xff,0x28,0x2c,40};
    native_packet(interleaved, 9); e = touchpad_events();
    CHECK(e.absolute == 5 && e.x == -2 && e.y == 1 && e.sample[0] == 300 && e.sample[1] == 300);
    /* A legitimate v2 packet with all buttons held is flushed after 20 ms. */
    touchpad_position(4, 300, 300, 40, 4, 7);
    CHECK(native.queue.mouse_pos == 6); g_ticks += 20;
    e = touchpad_events(); CHECK(e.absolute == 5 && e.down == 7 && !native.queue.mouse_pos);
    /* Unknown ALPS, unsupported Synaptics, mode refusal and missing bytes
     * all retain a proved compatibility format. */
    touchpad_boot(2, 21); CHECK(!native.touchpad.info.protocol && native.stats.aux_id == 0);
    native_packet(bare, 3); e = touchpad_events();
    CHECK(!e.absolute && e.x == 2 && e.y == -1 && e.down == 1);
    reset_native(); hw.touchpad_kind = 1; hw.syn_major = 7;
    CHECK(!i8042_init() && !native.touchpad.info.protocol && !hw.syn_mode);
    reset_native(); hw.touchpad_kind = 1; hw.syn_model = 0x80; hw.syn_caps = 0x804783; hw.mode_mismatch = true;
    CHECK(!i8042_init() && !native.touchpad.info.protocol && !hw.syn_mode);
    CHECK(native.touchpad.info.fallback_error == -I8042_EPROTO);
    /* All ACKs and query data may be captured before write() returns. */
    reset_native(); hw.touchpad_kind = 1; hw.syn_model = 0x80; hw.syn_caps = 0x804783; hw.burst_reply = true;
    CHECK(!i8042_init() && native.touchpad.info.protocol == TOUCHPAD_SYNAPTICS_NEW && !hw.count);
    /* A transient refusal anywhere in the optional Synaptics negotiation
     * must take the verified compatibility path without a controller reset. */
    unsigned optional_end = hw.aux_sends - 2;
    for (unsigned command = 3; command <= optional_end; command++) {
        reset_native(); hw.touchpad_kind = 1; hw.syn_model = 0x80; hw.syn_caps = 0x804783;
        hw.fail_aux_send = command; hw.fault_reply = 0xFC;
        CHECK(!i8042_init() && !native.touchpad.info.protocol && !native.stats.quarantined && !hw.resets);
        CHECK(native.touchpad.info.fallback_error == -I8042_EPROTO);
    }
    touchpad_boot(2, 1);
    optional_end = hw.aux_sends - 2;
    for (unsigned command = 3; command <= optional_end; command++) {
        reset_native(); hw.touchpad_kind = 2; hw.alps_model = 1;
        hw.fail_aux_send = command; hw.fault_reply = 0xFC;
        CHECK(!i8042_init() && !native.touchpad.info.protocol && !native.stats.quarantined && !hw.resets);
        CHECK(native.stats.last_elapsed_ms <= I8042_SETUP_MS);
    }
    for (unsigned i = 1; i <= 3; i++) {
        reset_native(); hw.missing_query_byte = i; hw.query_missing_once = true;
        CHECK(!i8042_init() && !native.touchpad.info.protocol && !native.stats.quarantined);
        CHECK(native.touchpad.info.fallback_error == -I8042_ETIMEDOUT);
    }
    reset_native(); hw.touchpad_kind = 1; hw.syn_major = 7; hw.rollback_bad = true;
    CHECK(i8042_init() == -I8042_EPROTO && native.stats.quarantined && !native.stats.active && !hw.resets);
    /* Firmware policy is an early return: no query and no port/PIC changes. */
    reset_native(); hw.touchpad_kind = 1; g_boot.input_policy = CBI_INPUT_FIRMWARE;
    CHECK(i8042_init() == -ENOSYS && !hw.writes && !hw.reads && !hw.aux_sends && !pic_changes);
    puts("touchpad: Synaptics old/new, ALPS v1/v2, gestures, stick/interleaving, fallback, resync and firmware ownership PASS");
}
