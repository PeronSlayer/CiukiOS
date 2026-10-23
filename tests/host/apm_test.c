/* APM BIOS register model driving the production protocol, no guest image.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ciuki/kernel.h>
#undef CLOCKS_PER_SEC
#include <ciuki/abi.h>
#include "../../src/kernel/core/apm.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
struct model {
    uint64_t now, flush_until, stop_until, ack_at;
    uint16_t version, flags, signature, negotiated, fault_function;
    uint8_t bios_error;
    int transport_error, reserve_error, prepare_error, flush_error, stop_error, resume_error;
    unsigned probes, connects, disconnects, calls, reserves, prepared, releases, quarantines;
    unsigned polls, rejects, suspends, offs, flushes, stops, resumes, publishes;
    unsigned event_head, event_tail, event_info;
    uint16_t events[64];
    uint16_t status_bx, status_cx, status_dx;
    /* fx-18: battery sockets answered by BX=80nnh (AH=09h beyond them). */
    unsigned sockets, unit_calls, busy_calls, lows;
    uint16_t busy_function; /* one EAGAIN for this function */
    uint16_t unit_cx[4], unit_dx[4];
    int low_error;
    uint64_t us;
    bool handoff;
    uint16_t ack_bx, ack_cx;
    bool acpi, acpi_on_reserve, bad_entry, leased;
    struct apm_status published;
    char order[1024];
    unsigned order_size;
};
static void trace(struct model *m, char c)
{
    CHECK(m->order_size + 1 < sizeof(m->order));
    if (m->order_size + 1 < sizeof(m->order)) {
        m->order[m->order_size++] = c; m->order[m->order_size] = 0;
    }
}
static bool acpi(void *p) { return ((struct model *)p)->acpi; }
static bool fault(struct model *m, struct biosvm_regs *r)
{
    if ((uint16_t)r->eax != m->fault_function) return false;
    if (m->bios_error) { r->flags = 1; r->eax = (uint32_t)m->bios_error << 8; }
    return true;
}
static int pm(void *, struct biosvm_regs *, uint32_t);
static int real(void *p, struct biosvm_regs *r, uint32_t ms)
{
    struct model *m = p;
    CHECK(r->interrupt == 0x15);
    m->us += 7; /* modelled firmware time per call */
    if (m->busy_calls) { m->busy_calls--; return -EAGAIN; } /* EC hold busy: nothing ran */
    if (m->busy_function && (uint16_t)r->eax == m->busy_function) { m->busy_function = 0; return -EAGAIN; }
    uint16_t fn = (uint16_t)r->eax;
    if (fn == 0x5300) m->probes++;
    else if (fn == 0x5301) { CHECK(m->leased && !m->acpi); m->connects++; trace(m, 'C'); }
    else if (fn == 0x5304) { m->disconnects++; trace(m, 'D'); }
    else return pm(p, r, ms);
    CHECK(!r->ebx && !r->ecx);
    CHECK(ms == (fn == 0x5301 ? 2000u : 100u));
    if (fn == 0x5304 && m->transport_error) return -EIO;
    if (fault(m, r)) return m->transport_error;
    r->flags = 0;
    if (fn == 0x5300) { r->eax = m->version; r->ebx = m->signature; r->ecx = m->flags; }
    return 0;
}
static int reserve(void *p)
{
    struct model *m = p; m->reserves++;
    if (m->reserve_error) return m->reserve_error;
    CHECK(!m->leased); m->leased = true;
    if (m->acpi_on_reserve) m->acpi = true;
    return 0;
}
static int pm(void *p, struct biosvm_regs *r, uint32_t ms)
{
    struct model *m = p; m->calls++;
    CHECK(m->leased && r->interrupt == 0x15 && !m->acpi);
    uint16_t fn = (uint16_t)r->eax;
    CHECK(ms == (fn == 0x5307 && r->ecx < 4 ? 2000u : 100u));
    if (fault(m, r)) return m->transport_error;
    r->flags = 0;
    switch (fn) {
    case 0x530e:
        CHECK(!r->ebx && r->ecx == 0x0102); r->eax = m->negotiated; trace(m, 'V'); break;
    case 0x5308: case 0x530f:
        CHECK(r->ebx == 1 && r->ecx == 1); trace(m, fn == 0x5308 ? 'E' : 'N'); break;
    case 0x530a:
        if (r->ebx == 1) { r->ebx = m->status_bx; r->ecx = m->status_cx; r->edx = m->status_dx; break; }
        CHECK((r->ebx & 0xff00) == 0x8000 && (r->ebx & 0xff) >= 1);
        m->unit_calls++;
        if ((r->ebx & 0xff) > m->sockets) { r->flags = 1; r->eax = 0x0900; break; }
        r->ecx = m->unit_cx[(r->ebx & 0xff) - 1]; r->edx = m->unit_dx[(r->ebx & 0xff) - 1];
        r->ebx = (m->status_bx & 0xff00) | 0xff; break;
    case 0x530b:
        m->polls++;
        if (m->event_head < m->event_tail) {
            r->ebx = m->events[m->event_head++]; r->ecx = m->event_info;
        } else { r->flags = 1; r->eax = 0x8000; }
        break;
    case 0x5307:
        CHECK(r->ebx == 1);
        if (r->ecx == 5) m->rejects++;
        else {
            CHECK(m->flushes && m->stops);
            m->ack_at = m->now; m->ack_bx = (uint16_t)r->ebx; m->ack_cx = (uint16_t)r->ecx;
            if (r->ecx == 2) { m->suspends++; trace(m, 'S'); }
            else if (r->ecx == 3) { m->offs++; trace(m, 'O'); }
            else CHECK(false);
        }
        break;
    default: CHECK(false); break;
    }
    return 0;
}
static void release(void *p, bool uncertain)
{
    struct model *m = p; CHECK(m->leased);
    if (uncertain) { m->quarantines++; trace(m, 'Q'); }
    else { m->releases++; m->leased = false; trace(m, 'R'); }
}
static uint64_t now(void *p) { return ((struct model *)p)->now; }
static int step(void *p, enum apm_stage stage, uint64_t end)
{
    struct model *m = p;
    if (stage == APM_FLUSH) {
        m->flushes++; trace(m, 'F');
        CHECK(end >= 2000);
        if (m->flush_error) return m->flush_error;
        return m->now < m->flush_until ? -EAGAIN : 0;
    }
    CHECK(stage == APM_STOP_DMA); m->stops++; trace(m, 'X');
    if (m->stop_error) return m->stop_error;
    return m->now < m->stop_until ? -EAGAIN : 0;
}
static int resume(void *p, uint16_t event, uint16_t info)
{
    struct model *m = p; CHECK(event == 3 || event == 4 || event == 7 || event == 11);
    CHECK(info == 0 || info == m->event_info); m->resumes++; trace(m, 'U');
    return m->resume_error;
}
static void publish(void *p, const struct apm_status *s)
{
    struct model *m = p; m->published = *s; m->publishes++;
}
static uint64_t now_us(void *p) { return ((struct model *)p)->us; }
static void low(void *p, int error) { struct model *m = p; m->lows++; m->low_error = error; trace(m, 'L'); }
static bool handoff(void *p) { return ((struct model *)p)->handoff; }
static int orderly(void *p, uint64_t end)
{
    int e = step(p, APM_FLUSH, end);
    return e ? e : step(p, APM_STOP_DMA, end);
}
static void fixture(struct model *m, struct apm_backend *b)
{
    memset(m, 0, sizeof(*m));
    m->version = m->negotiated = 0x0102; m->signature = 0x504d; m->flags = 3;
    m->status_bx = 0x0100; m->status_cx = 0x014b; m->status_dx = 0x805a;
    struct apm_ops ops = {.ctx=m, .real_call=real, .acpi_present=acpi, .reserve=reserve,
        .release=release, .now_ms=now, .step=step, .resume=resume,
        .publish=publish, .orderly_shutdown=orderly, .now_us=now_us, .low_battery=low, .handoff=handoff};
    apm_backend_init(b, &ops);
}
static void connected(struct model *m, struct apm_backend *b)
{
    fixture(m, b); CHECK(!apm_backend_probe(b)); CHECK(!apm_backend_connect(b));
    CHECK(b->state == APM_CONNECTED && !strcmp(m->order, "CV"));
    m->order_size = 0; m->order[0] = 0;
}
static void test_probe_connection(void)
{
    struct model m; struct apm_backend b;
    fixture(&m, &b); m.acpi = true;
    CHECK(apm_backend_probe(&b) == -EBUSY && b.state == APM_EXCLUDED);
    CHECK(apm_backend_connect(&b) == -EBUSY && m.probes == 1 && !m.connects && !m.reserves);
    for (unsigned i = 0; i < 3; i++) {
        fixture(&m, &b);
        if (i == 0) m.version = 0x0101;
        if (i == 1) m.signature = 0x4d50;
        if (i == 2) m.flags = 32;
        CHECK(apm_backend_probe(&b) < 0 && !m.connects && b.state == APM_ABSENT);
    }
    fixture(&m, &b); CHECK(!apm_backend_probe(&b)); b.ops.step = NULL;
    CHECK(apm_backend_connect(&b) == -ENOTSUP && !m.connects && !m.reserves);
    fixture(&m, &b); CHECK(!apm_backend_probe(&b)); m.acpi_on_reserve = true;
    CHECK(apm_backend_connect(&b) == -EBUSY && !m.connects && m.releases == 1);
    fixture(&m, &b); CHECK(!apm_backend_probe(&b)); m.reserve_error = -ENOSPC;
    CHECK(apm_backend_connect(&b) == -ENOSPC && !m.connects);
    fixture(&m, &b); m.flags = 27; CHECK(!apm_backend_probe(&b));
    CHECK(!apm_backend_connect(&b) && !strcmp(m.order, "CVEN"));
    CHECK(apm_backend_connect(&b) == -EBUSY && m.connects == 1);
    fixture(&m, &b); CHECK(!apm_backend_probe(&b)); m.negotiated = 0x0101;
    CHECK(apm_backend_connect(&b) == -ENOTSUP && m.disconnects == 1 && m.releases == 1);
    for (unsigned i = 0; i < 3; i++) {
        fixture(&m, &b); CHECK(!apm_backend_probe(&b));
        m.fault_function = i == 0 ? 0x5301 : 0x530e;
        m.bios_error = 7; m.transport_error = i == 1 ? -EFAULT : i == 2 ? -110 : 0;
        CHECK(apm_backend_connect(&b) < 0 && b.state == APM_FAILED);
        CHECK(m.disconnects == (i ? 1u : 0u) && (i ? m.quarantines == 1 : m.releases == 1));
    }
    puts("APM: installation, ACPI exclusion, admission, negotiation, no foreign disconnect, fault quarantine PASS");
}
static void test_descriptors_status(void)
{
    struct biosvm_regs r;
    struct apm_status ps;
    r = (struct biosvm_regs){.ebx=0x0203, .ecx=0x0864, .edx=0x8002};
    apm_status_decode(&r, &ps); CHECK(ps.ac == 2 && ps.percent == 100 && ps.seconds == 120 && ps.battery_flags == 8);
    r.ecx = 0xffff; r.edx = 0xffff; r.ebx = 0xff00;
    apm_status_decode(&r, &ps); CHECK(ps.ac == APM_UNKNOWN && ps.percent == APM_UNKNOWN && ps.seconds == APM_UNKNOWN);
    r.ecx = 0x8064; r.ebx = 0x0100; r.edx = 12;
    apm_status_decode(&r, &ps); CHECK(ps.ac == 1 && ps.percent == APM_UNKNOWN && ps.seconds == APM_UNKNOWN);
    r.ecx = 0x1064; apm_status_decode(&r, &ps); CHECK(ps.percent == APM_UNKNOWN);
    r.ecx = 101; r.edx = 0x7fff; apm_status_decode(&r, &ps); CHECK(ps.percent == APM_UNKNOWN && ps.seconds == 32767);
    r.flags = 1; apm_status_decode(&r, &ps); CHECK(ps.ac == APM_UNKNOWN && ps.seconds == APM_UNKNOWN);
    puts("APM: percentage/time/unknown/no-battery decoding PASS");
}
static void test_events(void)
{
    struct model m; struct apm_backend b; connected(&m, &b);
    uint16_t events[] = {1, 2, 9, 10, 6, 12, 0xbeef};
    memcpy(m.events, events, sizeof(events)); m.event_tail = 7;
    CHECK(!apm_backend_service(&b) && m.rejects == 4 && !m.suspends && m.polls == 8);
    CHECK(m.published.ac == 1 && m.published.percent == 75 && m.published.seconds == 5400);
    unsigned polls = m.polls; m.now = 999; CHECK(!apm_backend_service(&b) && m.polls == polls);
    m.now = 1000; CHECK(!apm_backend_service(&b) && m.polls == polls + 1);
    m.events[7] = 3; m.events[8] = 4; m.events[9] = 7; m.events[10] = 11;
    m.event_tail = 11; m.event_info = 1; m.now = 2000;
    CHECK(!apm_backend_service(&b) && m.resumes == 4);
    /* Power Status Change (APM 1.2 4.3.7) refreshes at once; a BIOS
     * refusal publishes unknown and keeps the connection. */
    m.events[11] = 6; m.event_tail = 12;
    m.fault_function = 0x530a; m.bios_error = 0x86; m.now = 3000;
    CHECK(!apm_backend_service(&b) && m.published.ac == APM_UNKNOWN && b.state == APM_CONNECTED);
    m.fault_function = 0x530b; m.bios_error = 0; m.transport_error = -EFAULT; m.now = 4000;
    CHECK(apm_backend_service(&b) == -EFAULT && m.disconnects == 1 && m.quarantines == 1);
    CHECK(apm_backend_service(&b) == -ENODEV);
    connected(&m, &b); for (unsigned i = 0; i < 20; i++) m.events[i] = 6; m.event_tail = 20;
    CHECK(!apm_backend_service(&b) && b.poll_overflows == 1 && m.event_head == 8);
    CHECK(!apm_backend_service(&b) && m.event_head == 16);
    CHECK(!apm_backend_service(&b) && m.event_head == 20);
    connected(&m, &b); m.acpi = true;
    CHECK(apm_backend_service(&b) == -EBUSY && !m.polls && m.disconnects == 1 && m.releases == 1);
    puts("APM: periodic event drain, ordinary suspend rejection, status publication, resume/time, flood bound PASS");
}
static void test_emergency_off(void)
{
    struct model m; struct apm_backend b;
    {
        connected(&m, &b); m.events[0] = 8; m.event_tail = 1;
        m.flush_until = 1800; m.stop_until = 2400;
        CHECK(!apm_backend_service(&b) && b.emergency && !m.suspends);
        CHECK(apm_backend_off(&b) == -EBUSY && !m.offs);
        for (unsigned t = 10; t <= 2410; t += 10) { m.now = t; CHECK(!apm_backend_service(&b)); }
        CHECK(!b.emergency && m.suspends == 1 && m.ack_at <= 2500 && !m.rejects && m.resumes == 1);
        CHECK(m.polls >= 4 && m.ack_bx == 1 && m.ack_cx == 2);
    }
    /* fx-18: Battery Low (APM 1.2 4.3.6) is a warning: storage is flushed
     * within the budget, the status refreshed and the event published;
     * never a DMA stop or Set Power State (Linux apm_32.c queues it too). */
    {
        connected(&m, &b); m.events[0] = 5; m.event_tail = 1; m.flush_until = 1800;
        unsigned published = m.publishes;
        CHECK(!apm_backend_service(&b) && b.emergency && b.emergency_kind == APM_EMERGENCY_LOW);
        CHECK(m.publishes == published + 1);
        for (unsigned t = 10; t <= 2410; t += 10) { m.now = t; CHECK(!apm_backend_service(&b)); }
        CHECK(!b.emergency && !m.suspends && !m.stops && !m.rejects && m.flushes && m.lows == 1 && !m.low_error);
        CHECK(strchr(m.order, 'L') && !strchr(m.order, 'S') && !strchr(m.order, 'X'));
        /* An expired flush is recorded, still no power operation. */
        connected(&m, &b); m.events[0] = 5; m.event_tail = 1; m.flush_until = 99999;
        CHECK(!apm_backend_service(&b)); m.now = APM_FLUSH_MS; CHECK(!apm_backend_service(&b));
        CHECK(!b.emergency && m.lows == 1 && m.low_error == -V86_ETIMEDOUT && !m.suspends && !m.stops);
        /* A critical suspend during the battery-low flush is upgraded and
         * its budget starts at the critical event (4.3.9: 5 s from then). */
        connected(&m, &b); m.events[0] = 5; m.event_tail = 1; m.flush_until = 99999;
        CHECK(!apm_backend_service(&b)); m.now = 1000; m.events[1] = 8; m.event_tail = 2;
        CHECK(!apm_backend_service(&b) && b.emergency_kind == APM_EMERGENCY_CRITICAL && b.emergency_start == 1000);
        m.flush_until = 0; m.now = 1010; CHECK(!apm_backend_service(&b)); m.now = 1020; CHECK(!apm_backend_service(&b));
        CHECK(m.suspends == 1 && m.stops && !m.lows && m.ack_at <= 1000 + APM_QUIESCE_MS);
    }
    connected(&m, &b); m.events[0] = 8; m.event_tail = 1;
    m.flush_until = m.stop_until = 99999;
    CHECK(!apm_backend_service(&b)); m.now = 2000; CHECK(!apm_backend_service(&b));
    m.events[1] = 8; m.event_tail = 2; m.now = 2010; CHECK(!apm_backend_service(&b));
    m.now = 2500; CHECK(apm_backend_service(&b) == -110);
    CHECK(m.suspends == 1 && !m.rejects && m.stops && b.emergency_start == 0);
    connected(&m, &b); m.events[0] = 8; m.event_tail = 1; m.flush_error = -EIO;
    CHECK(!apm_backend_service(&b)); m.now = 10;
    CHECK(apm_backend_service(&b) == -EIO && m.suspends == 1 && m.stops == 1 && !m.rejects);
    connected(&m, &b); m.events[0] = 8; m.event_tail = 1; m.resume_error = -EIO;
    CHECK(!apm_backend_service(&b)); m.now = 10;
    CHECK(apm_backend_service(&b) == -EIO && b.state == APM_FAILED && m.disconnects == 1 && m.releases == 1);
    connected(&m, &b); m.flush_until = 99999;
    CHECK(apm_backend_off(&b) == -EAGAIN && b.off_deadline == 2500);
    m.now = 2500;
    CHECK(apm_backend_off(&b) == -V86_ETIMEDOUT && !m.offs && !b.off_started);
    connected(&m, &b); m.flush_error = -EIO;
    CHECK(apm_backend_off(&b) == -EIO && !m.stops && !m.offs);
    m.flush_error = 0; m.stop_error = -EBUSY;
    CHECK(apm_backend_off(&b) == -EBUSY && !m.offs);
    m.stop_error = 0; m.order_size = 0; m.order[0] = 0;
    CHECK(apm_backend_off(&b) == -EIO && !strcmp(m.order, "FXODR"));
    CHECK(m.offs == 1 && m.ack_bx == 1 && m.ack_cx == 3);
    puts("APM: low/critical flush-stop-ack budget, polls during quiescence, retained errors, orderly off ordering PASS");
}
static void benchmark_poll(void)
{
    struct model m; struct apm_backend b; connected(&m, &b);
    CHECK(!apm_backend_service(&b)); /* first status and unit probe */
    struct apm_cost c; apm_backend_cost(&b, &c);
    const unsigned n = 10000;
    struct timespec start, end;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &start));
    for (unsigned i = 0; i < n; i++) {
        m.now += 1000;
        CHECK(!apm_backend_service(&b));
    }
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &end));
    double us = (double)(end.tv_sec - start.tv_sec) * 1000000.0 +
                (double)(end.tv_nsec - start.tv_nsec) / 1000.0;
    /* fx-18: one event call per second, Get Power Status every 10 s. */
    CHECK(m.polls == n + 1);
    apm_backend_cost(&b, &c);
    CHECK(c.event_calls == n && c.status_calls == n / 10 && c.calls == n + n / 10 && c.max_us == 7);
    printf("APM host model: %u polls, %u firmware calls, %.3f us/poll, %.3f us/call\n",
           n, c.calls, us/n, us/c.calls);
}
/* fx-18: the T23 shape (APM 1.2 BIOS 1AET52WW; values as a ThinkPad
 * reports them): AC off-line, main battery 75 % with 90 min, UltraBay
 * socket empty (CH=10h), no third socket (AH=09h). */
static void test_t23_units(void)
{
    struct model m; struct apm_backend b;
    fixture(&m, &b); m.sockets = 2;
    m.status_bx = 0x0000; m.status_cx = 0x014b; m.status_dx = 0x805a;
    m.unit_cx[0] = 0x014b; m.unit_dx[0] = 0x805a; m.unit_cx[1] = 0x10ff; m.unit_dx[1] = 0xffff;
    CHECK(!apm_backend_probe(&b) && !apm_backend_connect(&b));
    CHECK(!apm_backend_service(&b));
    CHECK(b.units == 2 && m.unit_calls == 3 && m.published.units == 2);
    CHECK(m.published.ac == 0 && m.published.percent == 75 && m.published.seconds == 5400 && m.published.battery_flags == 1);
    CHECK(m.published.unit[0].present == 1 && m.published.unit[0].percent == 75 && m.published.unit[0].seconds == 5400);
    CHECK(m.published.unit[1].present == 0 && m.published.unit[1].percent == APM_UNKNOWN);
    /* Refresh: aggregate + two units every 10 s, no re-probe. */
    unsigned calls = m.unit_calls; m.now = 10000; m.unit_cx[1] = 0x0832; m.unit_dx[1] = 0x0e10;
    CHECK(!apm_backend_service(&b) && m.unit_calls == calls + 2);
    CHECK(m.published.unit[1].present == 1 && m.published.unit[1].percent == 50 && m.published.unit[1].flags == 8 &&
          m.published.unit[1].seconds == 3600);
    /* No status call between refreshes. */
    unsigned publishes = m.publishes; m.now = 11000; CHECK(!apm_backend_service(&b) && m.publishes == publishes);
    /* Capabilities Change (4.3.13) repeats the unit probe. */
    m.sockets = 1; m.events[0] = 12; m.event_tail = 1; m.now = 12000;
    CHECK(!apm_backend_service(&b) && b.units == 1 && m.published.units == 1);
    /* APM 1.0-style CH=FFh: BL supplies the flag (03h charging). */
    struct biosvm_regs r = {.ebx = 0x0103, .ecx = 0xff5a, .edx = 0x0258};
    struct apm_status ps; apm_status_decode(&r, &ps);
    CHECK(ps.ac == 1 && ps.battery_flags == 8 && ps.percent == 90 && ps.seconds == 600);
    r.ebx = 0x00ff; apm_status_decode(&r, &ps); CHECK(ps.battery_flags == APM_UNKNOWN && ps.percent == 90);
    puts("APM: T23 battery units (80nnh until AH=09h), empty bay, 10 s refresh, capabilities change, BL fallback PASS");
}
/* fx-18: a busy EC hold (EAGAIN: nothing ran) skips a turn, never fails. */
static void test_busy_and_handoff(void)
{
    struct model m; struct apm_backend b;
    fixture(&m, &b); m.busy_calls = 1;
    CHECK(apm_backend_probe(&b) == -EAGAIN && b.state == APM_UNPROBED && !m.probes);
    CHECK(!apm_backend_probe(&b)); m.busy_calls = 1;
    CHECK(apm_backend_connect(&b) == -EAGAIN && b.state == APM_DISCOVERED && !m.leased && m.releases == 1);
    CHECK(!apm_backend_connect(&b) && b.state == APM_CONNECTED);
    m.busy_calls = 1; unsigned polls = m.polls, skipped = b.cost.skipped;
    CHECK(!apm_backend_service(&b) && m.polls == polls && b.state == APM_CONNECTED && b.cost.skipped == skipped + 1);
    CHECK(!apm_backend_service(&b) && m.polls == polls + 1);
    /* A standby request whose rejection meets a busy hold is answered next turn. */
    m.events[m.event_tail++] = 9; m.now = 1000; m.busy_function = 0x5307;
    CHECK(!apm_backend_service(&b) && b.rejects_pending == 1 && !m.rejects);
    m.now = 1010; CHECK(!apm_backend_service(&b) && m.rejects == 1 && !b.rejects_pending);
    /* ACPI ownership replaces APM: disconnect, clean release, EXCLUDED. */
    m.handoff = true; m.order_size = 0; m.order[0] = 0;
    CHECK(!apm_backend_service(&b) && b.state == APM_EXCLUDED && !strcmp(m.order, "DR") && !m.leased);
    CHECK(m.published.ac == APM_UNKNOWN && apm_backend_service(&b) == -ENODEV);
    connected(&m, &b); m.handoff = true; m.transport_error = -EIO;
    CHECK(apm_backend_service(&b) == -EIO && b.state == APM_EXCLUDED && m.quarantines == 1 && m.leased);
    puts("APM: busy EC hold skips without failing, rejection retried, ACPI handoff (clean and uncertain) PASS");
}
int main(void)
{
    test_probe_connection(); test_descriptors_status(); test_events(); test_emergency_off();
    test_t23_units(); test_busy_and_handoff(); benchmark_poll();
    printf("APM model: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
