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
        CHECK(r->ebx == 1); r->ebx = m->status_bx; r->ecx = m->status_cx; r->edx = m->status_dx; break;
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
        .publish=publish, .orderly_shutdown=orderly};
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
    for (unsigned event = 5; event <= 8; event += 3) {
        connected(&m, &b); m.events[0] = event; m.event_tail = 1;
        m.flush_until = 1800; m.stop_until = 2400;
        CHECK(!apm_backend_service(&b) && b.emergency && !m.suspends);
        CHECK(apm_backend_off(&b) == -EBUSY && !m.offs);
        for (unsigned t = 10; t <= 2410; t += 10) { m.now = t; CHECK(!apm_backend_service(&b)); }
        CHECK(!b.emergency && m.suspends == 1 && m.ack_at <= 2500 && !m.rejects && m.resumes == 1);
        CHECK(m.polls >= 4 && m.ack_bx == 1 && m.ack_cx == 2);
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
    /* Each empty one-second poll makes one event + one status call. */
    CHECK(m.polls == n);
    printf("APM host model: %u polls, %u firmware calls, %.3f us/poll, %.3f us/call\n",
           n, 2*n, us/n, us/(2*n));
}
int main(void)
{
    test_probe_connection(); test_descriptors_status(); test_events(); test_emergency_off(); benchmark_poll();
    printf("APM model: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
