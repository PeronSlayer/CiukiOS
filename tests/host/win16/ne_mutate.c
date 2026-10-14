/* SPDX-License-Identifier: MIT */
/* Host-only sanitizer driver for the production NE parser, segment loader and
 * relocator. It never executes NE code. Every input file is parsed as is,
 * truncated, and mutated deterministically; each accepted plan is loaded into
 * a mock segment store and relocated. Any refusal must carry a named rule.
 * Output: one summary line; exit 1 on an unnamed rule or invariant failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../sdk/win16/ne/ne.h"

static struct ne_image image;
static struct ne_workspace work;
static struct ne_buffer store[NE_MAX_SEGMENTS];
static uint8_t *backing[NE_MAX_SEGMENTS];
static unsigned long accepted, refused, relocated, bad;

static int resolve(void *arg, const struct ne_image *n, uint16_t module, uint16_t ordinal,
                   struct ne_string name, struct ne_address *out)
{
    (void)arg;
    if (!module || module > n->module_count) return -1;
    if (name.length && (name.offset >= n->size || name.length > n->size - name.offset)) return -1;
    if (ordinal == 13u) return -1; /* Exercise unresolved imports. */
    out->offset = 0x1000u + ordinal;
    out->selector = (uint16_t)(0x0f00u + module * 8u);
    return 0;
}

static int named(uint32_t rule)
{
    return strcmp(ne_rule_name(rule), "unknown-rule") != 0;
}

static void run(const uint8_t *bytes, uint32_t size)
{
    struct ne_error e;
    uint32_t i, count;
    if (ne_parse(bytes, size, &image, &e)) {
        ++refused;
        if (!named(e.rule) || e.rule == NE_OK || image.valid) ++bad;
        return;
    }
    ++accepted;
    if (ne_load_policy(&image, &e)) { if (!named(e.rule)) ++bad; return; }
    count = image.segment_count;
    for (i = 0; i < count; ++i) {
        uint32_t alloc = image.segments[i].allocation;
        backing[i] = malloc(alloc + 2u);
        if (!backing[i]) { fprintf(stderr, "oom\n"); exit(2); }
        backing[i][0] = backing[i][alloc + 1u] = 0xa5; /* Canaries around the store. */
        store[i].bytes = backing[i] + 1; store[i].size = alloc;
        store[i].selector = (uint16_t)(0x0087u + i * 8u);
        if (ne_load_segment(&image, (uint16_t)(i + 1u), store[i], &e)) ++bad;
    }
    for (i = 0; i < count; ++i) {
        if (!ne_relocate_segment(&image, (uint16_t)(i + 1u), store, count, resolve, 0, &work, &e))
            ++relocated;
        else if (!named(e.rule)) ++bad;
    }
    for (i = 0; i < count; ++i) {
        if (backing[i][0] != 0xa5 || backing[i][image.segments[i].allocation + 1u] != 0xa5) ++bad;
        free(backing[i]);
    }
}

int main(int argc, char **argv)
{
    int a = 1;
    unsigned long cases = 0, mutations = 4096;
    if (argc > 2 && !strcmp(argv[1], "-n")) { mutations = strtoul(argv[2], 0, 10); a = 3; }
    for (; a < argc; ++a) {
        FILE *f = fopen(argv[a], "rb");
        uint8_t *orig, *copy;
        long len;
        uint32_t i, size;
        if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || len > (1L << 24) || fseek(f, 0, SEEK_SET)) {
            fprintf(stderr, "cannot read %s\n", argv[a]); return 2;
        }
        size = (uint32_t)len;
        orig = malloc(size ? size : 1u); copy = malloc(size ? size : 1u);
        if (!orig || !copy || fread(orig, 1, size, f) != size) { fprintf(stderr, "read %s\n", argv[a]); return 2; }
        fclose(f);
        /* Exact-size heap copies let ASan catch any read past the input. */
        for (i = 0; i <= size; i += (i < 160u ? 1u : 13u)) {
            uint8_t *cut = malloc(i ? i : 1u);
            memcpy(cut, orig, i); run(cut, i); free(cut); ++cases;
        }
        for (i = 0; i < mutations && size; ++i) {
            uint32_t at = (uint32_t)((i * 2654435761u) % size);
            memcpy(copy, orig, size);
            switch (i % 4u) {
            case 0: copy[at] ^= (uint8_t)(1u << (i % 8u)); break;
            case 1: copy[at] = 0x00; break;
            case 2: copy[at] = 0xff; break;
            default: copy[at] = (uint8_t)(copy[at] + 1u); if (at + 1u < size) copy[at + 1u] ^= 0x80; break;
            }
            run(copy, size); ++cases;
        }
        free(orig); free(copy);
    }
    printf("ne-mutate mutations=%lu cases=%lu accepted=%lu refused=%lu relocated=%lu bad=%lu\n",
           mutations, cases, accepted, refused, relocated, bad);
    return bad ? 1 : 0;
}
