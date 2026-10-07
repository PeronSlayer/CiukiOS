#include "../../src/media/decoder.h"
#include <stdio.h>
#include <stdlib.h>

static int16_t pcm[MEDIA_OUTPUT_FRAMES * 2];

int main(int argc, char **argv)
{
    uint32_t rate = 0, total = 0, n, actual = 0;
    unsigned nonzero = 0, reads = 0, i;
    if (argc != 2) return 2;
    if (!media_decoder_open(argv[1], &rate, &total)) return 3;
    do {
        if (!media_decoder_read(pcm, MEDIA_OUTPUT_FRAMES, &n)) return 4;
        actual += n;
        for (i = 0; i < n * 2; ++i) nonzero += pcm[i] != 0;
        if (++reads > 1000) return 5;
    } while (n);
    if (!media_decoder_seek(total / 2) ||
        !media_decoder_read(pcm, MEDIA_OUTPUT_FRAMES, &n) || !n) return 6;
    printf("{\"rate\":%lu,\"total\":%lu,\"actual\":%lu,\"nonzero\":%u,\"reads\":%u,\"seek_frames\":%lu}\n",
           (unsigned long)rate, (unsigned long)total, (unsigned long)actual,
           nonzero, reads, (unsigned long)n);
    media_decoder_close();
    return 0;
}
