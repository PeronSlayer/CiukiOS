/* SPDX-License-Identifier: MIT */
#include "replay_base.h"
#ifndef DEMO_SOURCE
#define DEMO_SOURCE "../../../apps/demo/demo.c"
#endif
#define main ciuki_demo_main
#include DEMO_SOURCE
#undef main
#include <assert.h>
int main(int argc, char **argv)
{
    assert(argc == 2); uint32_t pixels[WIDTH * HEIGHT];
    draw(pixels, "AVATAR gjpqy 0123456789", 123, 2);
    FILE *f = fopen(argv[1], "wb"); assert(f);
    assert(fwrite(pixels, sizeof(*pixels), WIDTH * HEIGHT, f) == WIDTH * HEIGHT);
    assert(!fclose(f)); return 0;
}
