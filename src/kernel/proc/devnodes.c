/* Byte streams, distinct from diagnostic call 2. Prefix each physical serial
 * line so untrusted text can never become a CIUKI_TEST controller record.
 * No newline/tab/UTF-8 byte is altered; the console receives original bytes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/bootlog.h>
#include <ciuki/supervisor.h>

int device_open(enum px_kind kind, uint32_t flags)
{
    if (flags & (O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY)) return -EINVAL;
    if (kind == PX_CONSOLE && (flags & O_ACCMODE) != O_WRONLY) return -EACCES;
    return 0;
}
static void stream_bytes(const char *text, size_t bytes)
{
    serial_write(text, bytes);
    bootlog_capture(text, bytes);
}
int device_write(const void *buffer, uint32_t bytes)
{
    const char *text = buffer;
    /* The sink never schedules and interrupt handlers never print. Bound
     * each call so UART polling cannot monopolize the UP kernel. */
    if (bytes > 1024) bytes = 1024;
    static const char prefix[] = "[console] ";
    unsigned start = 0;
    stream_bytes(prefix, sizeof(prefix) - 1);
    for (unsigned i = 0; i < bytes; i++) if (text[i] == '\n' || text[i] == '\r') {
        stream_bytes(text + start, i + 1 - start);
        start = i + 1;
        if (start < bytes) stream_bytes(prefix, sizeof(prefix) - 1);
    }
    if (start < bytes) stream_bytes(text + start, bytes - start);
    /* Terminate the envelope as well, preventing a partial user line from
     * swallowing the next trusted record. Payload bytes are captured raw. */
    if (bytes && text[bytes - 1] != '\n') stream_bytes("\n", 1);
    console_write(text, bytes);
    return (int)bytes;
}
