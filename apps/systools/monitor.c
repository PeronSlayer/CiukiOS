/* SY-2 System Monitor counters. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const struct { const char *name, *category, *unit, *explain; uint64_t scale; } info[SM_COUNTERS] = {
    [SM_CPU_USAGE] = { "Processor usage", "Kernel", "%", "Percentage of time the processor ran work other than the idle task during the interval.", 100 },
    [SM_MEM_FREE] = { "Free memory", "Memory Manager", " KB", "Physical memory not allocated to any process or to the kernel.", 0 },
    [SM_MEM_USED] = { "Allocated memory", "Memory Manager", " KB", "Physical memory in use by processes and the kernel.", 0 },
    [SM_KHEAP] = { "Kernel heap", "Memory Manager", " KB", "Bytes currently allocated from the kernel heap.", 0 },
    [SM_PROCESSES] = { "Processes", "Kernel", "", "Number of processes, including the kernel owner.", 0 },
    [SM_THREADS] = { "Threads", "Kernel", "", "Number of threads in all processes.", 0 },
    [SM_DISK_READ] = { "Bytes read/second", "File System", " B/s", "Rate of bytes read from disks by the file system.", 0 },
    [SM_DISK_WRITE] = { "Bytes written/second", "File System", " B/s", "Rate of bytes written to disks by the file system.", 0 },
    [SM_NET_RX] = { "Packets received/second", "Network", " pkt/s", "Rate of packets received by the network adapter.", 0 },
    [SM_NET_TX] = { "Packets sent/second", "Network", " pkt/s", "Rate of packets sent by the network adapter.", 0 },
    [SM_NET_ERRORS] = { "Network errors/second", "Network", " err/s", "Rate of receive and transmit errors reported by all available network adapters.", 0 },
};
const char *sm_name(int c) { return c >= 0 && c < SM_COUNTERS ? info[c].name : ""; }
const char *sm_category(int c) { return c >= 0 && c < SM_COUNTERS ? info[c].category : ""; }
const char *sm_explain(int c) { return c >= 0 && c < SM_COUNTERS ? info[c].explain : ""; }

void sm_init(struct sm_monitor *m)
{
    memset(m, 0, sizeof(*m));
    m->interval_ms = 5000;     /* Windows 98 default update interval */
    m->chart = SM_LINE;
    m->items[0] = (struct sm_item){ .counter = SM_CPU_USAGE, .color = 0x0000cc, .scale = 100 };
    m->count = 1;
}
static struct sm_item *item(struct sm_monitor *m, int c)
{
    for (unsigned i = 0; i < m->count; i++) if (m->items[i].counter == c) return &m->items[i];
    return NULL;
}
int sm_add(struct sm_monitor *m, int c, const struct sm_raw *src)
{
    if (c < 0 || c >= SM_COUNTERS) return -EINVAL;
    if (item(m, c)) return -EEXIST;
    if (src && !(src->have & (1u << c))) return -ENODEV;
    if (m->count >= SM_CHART_ITEMS) return -ENOSPC;
    static const uint32_t palette[SM_CHART_ITEMS] = { 0x0000cc, 0xcc0000, 0x00aa00, 0xaa00aa, 0x00aaaa, 0xaaaa00, 0x808080, 0x000000 };
    unsigned at = m->count++;
    m->items[at] = (struct sm_item){ .counter = c, .color = palette[at], .scale = info[c].scale };
    return 0;
}
int sm_remove(struct sm_monitor *m, int c)
{
    for (unsigned i = 0; i < m->count; i++)
        if (m->items[i].counter == c) { memmove(&m->items[i], &m->items[i + 1], (m->count - i - 1) * sizeof(m->items[0])); m->count--; return 0; }
    return -ENOENT;
}
int sm_edit(struct sm_monitor *m, int c, uint32_t color, uint64_t scale)
{
    struct sm_item *it = item(m, c);
    if (!it) return -ENOENT;
    it->color = color & 0xffffff;
    it->scale = scale; it->peak = 0; it->count = 0;     /* history is scaled by the old maximum */
    return 0;
}
int sm_set_chart(struct sm_monitor *m, int chart)
{ if (chart < SM_LINE || chart > SM_NUMERIC) return -EINVAL; m->chart = chart; return 0; }
int sm_set_interval(struct sm_monitor *m, unsigned s)
{ if (s < 1 || s > 300) return -EINVAL; m->interval_ms = s * 1000; return 0; }

static uint64_t delta(uint64_t now, uint64_t before) { return now >= before ? now - before : 0; }
static uint64_t per_second(uint64_t d, uint64_t ticks, uint32_t hz)
{ return ticks && hz ? d * hz / ticks : 0; }
static uint64_t value_of(const struct sm_monitor *m, int c, const struct sm_raw *r)
{
    const struct sm_raw *p = &m->last;
    uint64_t ticks = delta(r->total_ticks, p->total_ticks);
    switch (c) {
    case SM_CPU_USAGE: {
        if (!ticks) return 0;
        uint64_t busy = ticks - (delta(r->idle_ticks, p->idle_ticks) > ticks ? ticks : delta(r->idle_ticks, p->idle_ticks));
        return (busy * 100 + ticks / 2) / ticks;
    }
    case SM_MEM_FREE: return (uint64_t)r->free_pages * 4;
    case SM_MEM_USED: return r->usable_pages >= r->free_pages ? (uint64_t)(r->usable_pages - r->free_pages) * 4 : 0;
    case SM_KHEAP: return r->kheap_bytes / 1024;
    case SM_PROCESSES: return r->processes;
    case SM_THREADS: return r->threads;
    case SM_DISK_READ: return per_second(delta(r->disk_read_bytes, p->disk_read_bytes), ticks, r->tick_hz);
    case SM_DISK_WRITE: return per_second(delta(r->disk_write_bytes, p->disk_write_bytes), ticks, r->tick_hz);
    case SM_NET_RX: return per_second(delta(r->net_rx_packets, p->net_rx_packets), ticks, r->tick_hz);
    case SM_NET_TX: return per_second(delta(r->net_tx_packets, p->net_tx_packets), ticks, r->tick_hz);
    case SM_NET_ERRORS: return per_second(delta(r->net_errors, p->net_errors), ticks, r->tick_hz);
    }
    return 0;
}
static int append(char *log, size_t cap, size_t *used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int append(char *log, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(log + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - *used) return -ENOSPC;
    *used += (size_t)w;
    return 0;
}
int sm_log_header(struct sm_monitor *m, char *log, size_t cap, size_t *used)
{
    size_t at = *used;
    int e = append(log, cap, &at, "seconds");
    for (unsigned i = 0; !e && i < m->count; i++) e = append(log, cap, &at, ",\"%s\"", info[m->items[i].counter].name);
    if (!e) e = append(log, cap, &at, "\r\n");
    if (e) return e;
    *used = at; m->logging = 1; m->log_rows = 0;
    return 0;
}
int sm_sample(struct sm_monitor *m, const struct sm_raw *raw, char *log, size_t cap, size_t *used)
{
    if (!m->have_last) { m->last = *raw; m->have_last = 1; return 0; }   /* rates need two snapshots */
    size_t at = used ? *used : 0;
    int e = 0;
    if (m->logging && log && used)
        e = append(log, cap, &at, "%llu", (unsigned long long)(raw->tick_hz ? raw->total_ticks / raw->tick_hz : 0));
    for (unsigned i = 0; i < m->count; i++) {
        struct sm_item *it = &m->items[i];
        it->value = value_of(m, it->counter, raw);
        if (!it->scale && it->value > it->peak) it->peak = it->value;   /* auto scale: the running peak */
        uint64_t scale = it->scale ? it->scale : (it->peak ? it->peak : 1);
        unsigned pct = (unsigned)(it->value >= scale ? 100 : it->value * 100 / scale);
        if (it->count == SM_HISTORY) { memmove(it->history, it->history + 1, SM_HISTORY - 1); it->count--; }
        it->history[it->count++] = (uint8_t)pct;
        if (m->logging && log && used && !e) e = append(log, cap, &at, ",%llu", (unsigned long long)it->value);
    }
    if (m->logging && log && used && !e) e = append(log, cap, &at, "\r\n");
    m->last = *raw;
    if (m->logging && log && used) {
        if (e) { m->logging = 0; return e; }       /* log full: stop logging, charts continue */
        *used = at; m->log_rows++;
    }
    return 0;
}
void sm_format(const struct sm_monitor *m, unsigned i, char *out, size_t cap)
{
    if (i >= m->count) { if (cap) out[0] = 0; return; }
    snprintf(out, cap, "%s: %llu%s", info[m->items[i].counter].name, (unsigned long long)m->items[i].value, info[m->items[i].counter].unit);
}

void sm_raw_from_proc(struct sm_raw *out, struct sm_source *src, const struct ciuki_proc_system *sys,
                      unsigned threads, int have_net, uint64_t rx_frames, uint64_t tx_frames)
{
    memset(out, 0, sizeof(*out));
    out->total_ticks = sys->total_ticks; out->idle_ticks = sys->idle_ticks;
    out->usable_pages = sys->usable_pages; out->free_pages = sys->free_pages; out->kheap_bytes = sys->kheap_bytes;
    out->processes = sys->processes; out->threads = threads; out->tick_hz = sys->tick_hz;
    out->have = (1u << SM_CPU_USAGE) | (1u << SM_MEM_FREE) | (1u << SM_MEM_USED) | (1u << SM_KHEAP) | (1u << SM_PROCESSES) | (1u << SM_THREADS);
    if (src) {
        if (src->primed) {
            src->read_total += (uint32_t)(sys->disk_read_kib - src->read_kib) * 1024ull;
            src->write_total += (uint32_t)(sys->disk_write_kib - src->write_kib) * 1024ull;
        }
        src->primed = 1; src->read_kib = sys->disk_read_kib; src->write_kib = sys->disk_write_kib;
        out->disk_read_bytes = src->read_total; out->disk_write_bytes = src->write_total;
        out->have |= (1u << SM_DISK_READ) | (1u << SM_DISK_WRITE);
    }
    if (have_net) { out->net_rx_packets = rx_frames; out->net_tx_packets = tx_frames; out->have |= (1u << SM_NET_RX) | (1u << SM_NET_TX); }
}

void sm_network_sample(struct sm_raw *out, struct sm_source *src, const struct ciuki_net_control *net, unsigned count)
{
    if (!src) return;
    out->have &= ~((1u << SM_NET_RX) | (1u << SM_NET_TX) | (1u << SM_NET_ERRORS));
    for (unsigned i = 0; i < 2; i++) {
        const struct ciuki_net_control *n = net && i < count ? &net[i] : NULL;
        if (!n || n->version != 1 || n->size != sizeof(*n)) { src->net[i].primed = 0; continue; }
        uint64_t errors = n->rx_errors + n->tx_errors;
        if (src->net[i].primed && src->net[i].generation == n->generation && !memcmp(src->net[i].mac, n->mac, 6)) {
            src->rx_total += delta(n->rx_packets, src->net[i].rx);
            src->tx_total += delta(n->tx_packets, src->net[i].tx);
            src->error_total += delta(errors, src->net[i].errors);
        }
        src->net[i].rx = n->rx_packets; src->net[i].tx = n->tx_packets; src->net[i].errors = errors;
        src->net[i].generation = n->generation; memcpy(src->net[i].mac, n->mac, 6); src->net[i].primed = 1;
        out->have |= (1u << SM_NET_RX) | (1u << SM_NET_TX) | (1u << SM_NET_ERRORS);
    }
    out->net_rx_packets = src->rx_total; out->net_tx_packets = src->tx_total; out->net_errors = src->error_total;
}
