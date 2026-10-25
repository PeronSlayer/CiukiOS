/* Production index/mutations against independent ordered and page oracles.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main proc_fixture_main
#include "proc_test.c"
#undef main

static uint32_t random_state = 0x7ead54c1;
static uint32_t random_next(void)
{ return random_state = random_state * 1664525u + 1013904223u; }

/* The pre-index lookup, deliberately kept linear as the differential oracle. */
static struct ua_extent *linear_find(const struct uaddr *u, uint32_t va)
{
    for (struct ua_extent *e = u->head; e && e->base <= va; e = e->next)
        if (va < e->end) return e;
    return 0;
}
static struct ua_extent *linear_bound(const struct uaddr *u, uint32_t va)
{
    for (struct ua_extent *e = u->head; e; e = e->next)
        if (e->end > va) return e;
    return 0;
}
static void check_address(struct uaddr *u, uint32_t va)
{
    CHECK(ua_find(u, va) == linear_find(u, va));
    CHECK(ua_lower_bound(u, va) == linear_bound(u, va));
}
static unsigned check_tree(struct ua_extent *e, struct ua_extent *parent,
                           struct ua_extent **ordered, unsigned *count)
{
    if (!e) return 0;
    CHECK(e->parent == parent);
    unsigned left = check_tree(e->left, e, ordered, count);
    CHECK(*ordered == e && e->base < e->end);
    if (e->previous) CHECK(e->previous->next == e && e->previous->end <= e->base);
    else CHECK(!*count);
    if (e->next) CHECK(e->next->previous == e);
    *ordered = e->next; (*count)++;
    unsigned right = check_tree(e->right, e, ordered, count);
    CHECK(left <= right + 1 && right <= left + 1);
    CHECK(e->height == 1 + (left > right ? left : right));
    return e->height;
}
static void check_index(struct uaddr *u)
{
    struct ua_extent *ordered = u->head;
    unsigned count = 0, height = check_tree(u->root, 0, &ordered, &count);
    CHECK(!ordered && count == u->extents);
    unsigned log = 0;
    for (unsigned n = count; n; n >>= 1) log++;
    CHECK(height <= 2 * log);
    if (u->last_hit) CHECK(linear_find(u, u->last_hit->base) == u->last_hit);
    for (struct ua_extent *e = u->head; e; e = e->next) {
        CHECK(ua_find(u, e->base) == e && ua_find(u, e->end - 1) == e);
        CHECK(ua_lower_bound(u, e->end) == e->next);
    }
    for (unsigned i = 0; i < 12; i++) {
        check_address(u, random_next());
        check_address(u, 0x10000u + random_next() % (2048 * 4 * PAGE_SIZE));
    }
}

static void index_permutations(void)
{
    enum { N = 2048 };
    struct ua_extent *nodes = calloc(N, sizeof(*nodes)); CHECK(nodes);
    unsigned order[N];
    for (unsigned pass = 0; pass < 3; pass++) {
        struct uaddr u = {0};
        for (unsigned i = 0; i < N; i++) order[i] = pass == 1 ? N - 1 - i : i;
        if (pass == 2) for (unsigned i = N - 1; i; i--) {
            unsigned j = random_next() % (i + 1), tmp = order[i];
            order[i] = order[j]; order[j] = tmp;
        }
        for (unsigned i = 0; i < N; i++) {
            struct ua_extent *e = &nodes[order[i]];
            *e = (struct ua_extent){ .base = 0x10000u + order[i] * 4 * PAGE_SIZE,
                                    .end = 0x10000u + (order[i] * 4 + 2) * PAGE_SIZE };
            extent_insert(&u, e); u.extents++;
            CHECK(!u.last_hit); check_index(&u);
            unsigned points = preempt_points, blocks = heap_blocks;
            CHECK(ua_find(&u, e->base) == e && ua_find(&u, e->end - 1) == e);
            CHECK(preempt_points == points && heap_blocks == blocks && !u.pins);
        }
        /* Repeated root deletion exercises both successor transplant cases. */
        for (unsigned i = 0; i < N; i++) {
            struct ua_extent *e = pass ? &nodes[order[i]] : u.root;
            CHECK(ua_find(&u, e->base) == e);
            extent_remove(&u, e); u.extents--;
            CHECK(!u.last_hit); check_index(&u);
        }
        CHECK(!u.head && !u.root && !u.last_hit);
    }
    free(nodes);
    puts("uaddr AVL: ascending/descending/random insert, root/random erase, bounds, cache, invariants after every mutation PASS");
}

static void native_mutations(void)
{
    enum { PAGES = 96 };
    int oracle[PAGES];
    for (unsigned i = 0; i < PAGES; i++) oracle[i] = -1;
    struct uaddr u; CHECK(!ua_init(&u, 0));
    for (unsigned step = 0; step < 4000; step++) {
        unsigned first = random_next() % PAGES, bytes = 1 + random_next() % 8;
        if (bytes > PAGES - first) bytes = PAGES - first;
        uint32_t base = CIUKI_MMAP_BASE + first * PAGE_SIZE;
        uint32_t prot = (random_next() >> 8) % 3;
        prot = prot == 2 ? PROT_READ | PROT_WRITE : prot;
        unsigned op = (random_next() >> 16) % 3;
        bool covered = true, empty = true;
        for (unsigned i = first; i < first + bytes; i++) {
            covered &= oracle[i] >= 0; empty &= oracle[i] < 0;
        }
        int result;
        if (!op) {
            result = ua_map_at(&u, base, bytes * PAGE_SIZE, prot,
                               PROT_READ | PROT_WRITE, UA_ANON, 0);
            CHECK(result == (empty ? 0 : -EINVAL));
        } else if (op == 1) {
            result = ua_mprotect(&u, base, bytes * PAGE_SIZE, prot);
            CHECK(result == (covered ? 0 : -ENOMEM));
        } else {
            result = ua_munmap(&u, base, bytes * PAGE_SIZE); CHECK(!result);
        }
        if (!result) for (unsigned i = first; i < first + bytes; i++)
            oracle[i] = op == 2 ? -1 : (int)prot;
        check_index(&u);
        for (unsigned i = 0; i < PAGES; i++) {
            struct ua_extent *e = ua_find(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE);
            CHECK(oracle[i] < 0 ? !e : e && e->prot == (unsigned)oracle[i]);
        }
    }
    ua_destroy(&u); check_index(&u);
    CHECK(!ua_init(&u, 0));
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, 3 * PAGE_SIZE, PROT_READ, 3, UA_ANON, 0));
    CHECK(ua_find(&u, CIUKI_MMAP_BASE));
    alloc_fail = 0;
    CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE + PAGE_SIZE, PAGE_SIZE, 3) == -ENOMEM);
    alloc_fail = -1; check_index(&u); CHECK(u.extents == 1);
    /* Insert succeeds but page-table allocation fails: cached/linked rollback. */
    alloc_fail = 2;
    CHECK(ua_map_at(&u, CIUKI_MMAP_BASE + 0x400000, PAGE_SIZE, 3, 3, UA_ANON, 0) == -ENOMEM);
    alloc_fail = -1; check_index(&u); CHECK(u.extents == 1);
    ua_destroy(&u); check_index(&u);
    CHECK(!pages_used && !heap_blocks);
    puts("uaddr native: 4000 randomized map/protect/unmap transactions, overlaps/holes/splits against page oracle PASS");
}

static void win32_mutations(void)
{
    struct uaddr u; CHECK(!ua_init(&u, 0)); u.win32 = true;
    uint32_t base = 0x10000000, bytes = 16 * PAGE_SIZE;
    CHECK(!ua_map_at(&u, base, bytes, 0, 7, UA_W32, 0));
    struct ua_extent *e = ua_find(&u, base);
    e->w32_allocation = base; e->w32_state = W32_MEM_RESERVE;
    e->w32_type = W32_MEM_PRIVATE;
    check_index(&u);
    CHECK(!ua_w32_change(&u, base, bytes, W32_VM_ALLOC, W32_PAGE_READWRITE, 3, false));
    check_index(&u);
    for (unsigned i = 0; i < 500; i++) {
        uint32_t at = base + (random_next() % 16) * PAGE_SIZE;
        CHECK(ua_find(&u, at));
        CHECK(!ua_w32_change(&u, at, PAGE_SIZE, W32_VM_PROTECT, W32_PAGE_READONLY, 1, false));
        check_index(&u);
        CHECK(ua_find(&u, at)->prot == 1);
        CHECK(!ua_w32_change(&u, at, PAGE_SIZE, W32_VM_PROTECT, W32_PAGE_READWRITE, 3, false));
        check_index(&u); CHECK(u.extents == 1);
    }
    unsigned state[16], protection[16];
    for (unsigned i = 0; i < 16; i++) { state[i] = W32_MEM_COMMIT; protection[i] = W32_PAGE_READWRITE; }
    for (unsigned step = 0; step < 2000; step++) {
        unsigned first = random_next() % 16, pages = 1 + ((random_next() >> 8) % 5);
        if (pages > 16 - first) pages = 16 - first;
        unsigned op = (random_next() >> 16) % 3;
        uint32_t operation = op == 0 ? W32_VM_ALLOC : op == 1 ? W32_VM_PROTECT : W32_VM_FREE;
        uint32_t protect = random_next() & 0x100 ? W32_PAGE_READONLY : W32_PAGE_READWRITE;
        bool committed = true;
        for (unsigned i = first; i < first + pages; i++) committed &= state[i] == W32_MEM_COMMIT;
        int err = ua_w32_change(&u, base + first * PAGE_SIZE, pages * PAGE_SIZE,
                               operation, protect, protect == W32_PAGE_READONLY ? 1 : 3, false);
        CHECK(err == (op == 1 && !committed ? -EEXIST : 0));
        if (!err) for (unsigned i = first; i < first + pages; i++) {
            if (op == 2) { state[i] = W32_MEM_RESERVE; protection[i] = 0; }
            else if (op == 1 || state[i] != W32_MEM_COMMIT) {
                state[i] = W32_MEM_COMMIT; protection[i] = protect;
            }
        }
        check_index(&u);
        for (unsigned i = 0; i < 16; i++) {
            e = ua_find(&u, base + i * PAGE_SIZE);
            CHECK(e && e->w32_state == state[i] && e->w32_protect == protection[i]);
        }
    }
    CHECK(!ua_w32_change(&u, base, bytes, W32_VM_ALLOC, W32_PAGE_READWRITE, 3, false));
    check_index(&u);
    CHECK(!ua_w32_change(&u, base, bytes, W32_VM_PROTECT, W32_PAGE_READWRITE, 3, false));
    check_index(&u); CHECK(u.extents == 1);
    struct ua_pin pin; CHECK(!ua_pin(&u, &pin, base, PAGE_SIZE, true));
    CHECK(ua_w32_release(&u, base, false) == -EBUSY); check_index(&u);
    ua_unpin(&u, &pin);
    CHECK(!ua_w32_release(&u, base, false)); check_index(&u);
    ua_destroy(&u);
    CHECK(!pages_used && !heap_blocks);
    puts("uaddr Win32: repeated two-boundary split/merge, cached-node retirement, pinned release PASS");
}
int main(void)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller;
    index_permutations(); native_mutations(); win32_mutations();
    free(ram);
    printf("uaddr index: checks=%u full cleanup PASS\n", checks);
    return 0;
}
