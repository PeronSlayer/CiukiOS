/* fx-11: storage requests made by ciuki_font_open for the two faces of the
 * T23 trial 423b803a (NetSurf's first layout: font_opens=2
 * font_open_us=2117000 of elapsed_us=2175000).
 *
 * Ciuki's newlib is configured with HAVE_BLKSIZE (sdk/newlib/patches/
 * 0001-ciukios-configuration.patch), so __swhatbuf_r sizes a FILE buffer from
 * st_blksize, and _fread_r refills that buffer with one read(2) per block
 * (newlib/libc/stdio/fread.c, makebuf.c). The kernel reports st_blksize as
 * the FAT cluster (src/kernel/proc/posixpath.c), 8 x 512 bytes on the system
 * volume (scripts/build_image.py). glibc's fread reads large requests
 * directly, so this harness supplies the guest's refill loop through
 * --wrap=fread; read(2) itself is counted through --wrap=read.
 * SPDX-License-Identifier: MIT */
#include <ciuki/font.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define GUEST_BLKSIZE 4096u
static unsigned reads, freads;
static size_t largest, bytes_read;
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t n)
{
    ssize_t r = __real_read(fd, buffer, n);
    reads++;
    if (n > largest) largest = n;
    if (r > 0) bytes_read += (size_t)r;
    return r;
}
/* newlib _fread_r on a buffered stream: refill GUEST_BLKSIZE bytes per
 * read(2) until the request is satisfied (no direct large read). */
size_t __wrap_fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    static unsigned char block[GUEST_BLKSIZE];
    size_t want = size * count, done = 0;
    freads++;
    if (!size || !count) return 0;
    while (done < want) {
        ssize_t r = __wrap_read(fileno(stream), block, sizeof(block));
        if (r <= 0) break;
        size_t take = (size_t)r < want - done ? (size_t)r : want - done;
        memcpy((unsigned char *)buffer + done, block, take);
        done += take;
    }
    return done / size;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    printf("{\"guest_blksize\":%u,\"faces\":[", GUEST_BLKSIZE);
    for (int i = 1; i < argc; i++) {
        struct ciuki_font_face *face = NULL;
        reads = freads = 0; largest = bytes_read = 0;
        if (ciuki_font_open(argv[i], &face)) { fprintf(stderr, "open %s: %s\n", argv[i], strerror(errno)); return 1; }
        struct ciuki_font_metrics metrics;
        if (ciuki_font_get_metrics(face, &metrics) || !metrics.glyph_count) return 1;
        ciuki_font_close(face);
        printf("%s{\"path\":\"%s\",\"reads\":%u,\"stdio_freads\":%u,\"largest_read\":%zu,\"bytes_read\":%zu}",
               i > 1 ? "," : "", argv[i], reads, freads, largest, bytes_read);
    }
    puts("]}");
    return 0;
}
