/* sec-user-05: boundaries of the ADVAPI32 identity facade (identity.c) for
 * the registry host tests: the caller is uid 1000 of session 3,
 * Administrators+Users, published account "Ciuki". H11
 * (tests/host/security/test_identity_facades.py) runs the same facade on the
 * kernel's token objects. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <ciuki/abi.h>
#include <ciuki/w32_transport.h>
#include <assert.h>
#include <string.h>
uint32_t w32_u32(const void *);
void w32_put32(void *, uint32_t);
int w32r_release(void *p) { (void)p; return 0; }
uint32_t w32r_machine(uint32_t sid[3]) { sid[0] = 1; sid[1] = 2; sid[2] = 3; return 0; }
uint32_t w32r_object(struct ciuki_w32_object_args *a, void *buffer)
{
    if (a->action == W32_OBJ_CREATE) {
        assert(a->type == W32_OBJ_TOKEN && a->access == 8 && !buffer);
        a->result = 0x1234; return 0;
    }
    if (a->action == W32_OBJ_CLOSE) return a->handle == 0x1234 ? 0 : 6;
    assert(a->action == W32_OBJ_QUERY && buffer);
    if (a->handle != 0x1234) return 6;
    struct ciuki_credential c = {.version = 1, .size = 64, .uid = 1000, .primary_gid = 545, .group_count = 2,
                                 .groups = {544, 545}, .session_id = 3, .flags = 1};
    memcpy(buffer, &c, sizeof(c)); a->result = 64; a->type = W32_OBJ_TOKEN; a->value = 7; return 0;
}
int32_t w32r_accounts(uint32_t op, const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used)
{
    const uint8_t *q = in; uint8_t *r = out;
    assert(op == 1 && n == 312 && cap >= 160);
    if (w32_u32(q + 36) != 1000) return -3;
    memset(r, 0, 160); w32_put32(r, 1); w32_put32(r + 4, 1); w32_put32(r + 16, 1000); w32_put32(r + 20, 3);
    memcpy(r + 32, "Ciuki", 5); *used = 160; return 0;
}
