/* Byte streams, distinct from diagnostic call 2. Prefix each physical serial
 * line so untrusted text can never become a CIUKI_TEST controller record.
 * No newline/tab/UTF-8 byte is altered; the console receives original bytes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/bootlog.h>
#include <ciuki/supervisor.h>
#include <ciuki/cpustat.h>

int device_open(enum px_kind kind, uint32_t flags)
{
    if (flags & (O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY)) return -EINVAL;
    if (kind == PX_CONSOLE && (flags & O_ACCMODE) != O_WRONLY) return -EACCES;
    if (kind == PX_AUDIO || kind == PX_MIXER || kind == PX_NET || kind == PX_MEDIA) {
        if (flags & O_APPEND) return -EINVAL;
        if ((flags & O_ACCMODE) != (kind == PX_AUDIO ? O_WRONLY : O_RDONLY)) return -EACCES;
    }
    /* f9-02: /dev/ataN opens read-only or read-write for anyone; authority
     * is checked on every mutating request, never at open. */
    if (kind == PX_DISK) {
        if (flags & O_APPEND) return -EINVAL;
        if ((flags & O_ACCMODE) == O_WRONLY) return -EACCES;
    }
    return 0;
}
int device_write(const void *buffer, uint32_t bytes)
{
    const char *text = buffer;
    /* The producer only queues/captures bytes. Bound its copy and envelope
     * work; a UART readiness wait belongs only to the writer/panic path. */
    if (bytes > 1024) bytes = 1024;
    if (!bytes) return 0;
    static const char prefix[] = "[console] ";
    /* Include every prefix and the terminator in ONE queue publication.
     * A timer or another task may run between calls to serial_write. */
    char record[SERIAL_RECORD_MAX];
    size_t at = sizeof(prefix) - 1;
    memcpy(record, prefix, at);
    unsigned consumed = 0;
    while (consumed < bytes && at + 2 <= sizeof(record)) {
        char c = text[consumed++];
        record[at++] = c;
        if ((c == '\n' || c == '\r') && consumed < bytes) {
            /* A short write bounds stack and IRQ-off queue work even for
             * 1024 consecutive line endings. Reserve the final newline. */
            if (at + sizeof(prefix) + 1 > sizeof(record)) break;
            memcpy(record + at, prefix, sizeof(prefix) - 1);
            at += sizeof(prefix) - 1;
        }
    }
    /* Terminate the envelope as well, preventing a partial user line from
     * swallowing the next trusted record. Payload bytes are captured raw. */
    if (text[consumed - 1] != '\n') record[at++] = '\n';
    if (cpu_stall_record) cpu_stall_record(text, consumed);
    serial_write(record, at);
    bootlog_capture(record, at);
    console_write(text, consumed);
    return (int)consumed;
}
