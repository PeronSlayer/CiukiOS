/* sec-user-05 H11: production native identity adapters (sdk/libciuki/pwd.c
 * and account_link.c, the ACCOUNTS CSC1 client) against a boundary fixture
 * of the SYSTEM account service: call 101 CONNECT and the channel calls are
 * faked; the fixture speaks the frozen wire format (user-security-contract.md
 * 4.1: 312-byte selector body, LOOKUP 16+144 reply, PROFILE 272 reply) and
 * applies the PROFILE authorization rule (own mapping, administrators any).
 * Expected entries are written out independently. Migrated uid 1007 keeps
 * its retained root u00000003 although u000003ef belongs to another owner.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static unsigned checks;
struct ciuki_security_args;
int ciuki_security(struct ciuki_security_args *);
int ciuki_channel_send(int, const struct ciuki_message *, uint32_t);
int ciuki_channel_recv(int, struct ciuki_message *, uint32_t);
/* Host-width addresses behind the SDK's 32-bit CU_PTR fields. */
static const void *pointers[16];
static unsigned next_pointer;
uint32_t h11_pointer(const void *p) { pointers[next_pointer % 16] = p; return ++next_pointer % 16 ? next_pointer % 16 : 16; }
static void *pointer(uint32_t token) { CHECK(token >= 1 && token <= 16); return (void *)(uintptr_t)pointers[(token - 1) % 16]; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static const struct { uint32_t uid, groups; const char *name, *root; } accounts[] = {
    {501, 4, "Guest", "/Windows/Profiles/u000001f5"},
    {1007, 3, "Alice", "/Windows/Profiles/u00000003"},
    {1008, 2, "Bob", "/Windows/Profiles/u000003ef"}};
static uint32_t caller_uid = 1007, connects, closes, endpoint_generation = 41;
static int caller_admin = 1, corrupt, open_links;
static uint8_t request[8192], reply[8192];
static uint32_t request_bytes, reply_bytes, reply_at, request_op, request_id;
static int32_t reply_status;
int ciuki_security(struct ciuki_security_args *a)
{
    CHECK(a->version == 1 && a->size == 64 && a->operation == CIUKI_SECURITY_SERVICE_CONNECT && a->output_bytes == 64);
    struct ciuki_security_link *in = pointer(a->spawn_address), *out = pointer(a->output_address);
    CHECK(in->service == CIUKI_SECURITY_SERVICE_ACCOUNTS && in->size == 64);
    memset(out, 0, sizeof(*out));
    out->endpoint_fd = 900; out->endpoint_generation = ++endpoint_generation; out->service_generation = 3;
    out->server_role = CIUKI_SECURITY_ROLE_ACCOUNT; out->peer_pid = 5;
    connects++; open_links++; request_bytes = reply_bytes = reply_at = 0;
    return 0;
}
int close(int fd) { CHECK(fd == 900 && open_links == 1); open_links--; closes++; return 0; }
static int account(uint32_t uid, const char *name)
{
    for (unsigned i = 0; i < sizeof(accounts) / sizeof(*accounts); i++)
        if (name ? !strcasecmp(name, accounts[i].name) : uid == accounts[i].uid) return (int)i;
    return -1;
}
/* The fixture service: answers once the whole body arrived. */
static void serve(void)
{
    CHECK(request_bytes == 312 && rd32(request) == 1 && !rd32(request + 4) && !rd32(request + 304) && !rd32(request + 308));
    uint32_t name_bytes = rd32(request + 24), uid = rd32(request + 36);
    CHECK(!name_bytes != !uid);
    char name[33] = {0};
    if (name_bytes) { CHECK(name_bytes <= 31); memcpy(name, request + 48, name_bytes); }
    int i = account(uid, name_bytes ? name : NULL);
    reply_status = 0; reply_bytes = 0; reply_at = 0;
    if (i < 0) { reply_status = -ESRCH; return; }
    if (request_op == 1) {
        memset(reply, 0, 160); wr32(reply, 1); wr32(reply + 4, 1); wr32(reply + 8, 77);
        wr32(reply + 16, accounts[i].uid); wr32(reply + 20, accounts[i].groups);
        memcpy(reply + 32, accounts[i].name, strlen(accounts[i].name));
        reply_bytes = 160;
    } else {
        CHECK(request_op == 8);
        if (accounts[i].uid != caller_uid && !caller_admin) { reply_status = -EACCES; return; }
        memset(reply, 0, 272); wr32(reply, 1); wr32(reply + 4, 272); wr32(reply + 8, accounts[i].uid);
        wr32(reply + 16, 77); wr32(reply + 24, 9);
        snprintf((char *)reply + 32, 64, "%s", accounts[i].root);
        snprintf((char *)reply + 96, 96, "%s/Documents", accounts[i].root);
        snprintf((char *)reply + 192, 80, "/Programs/terminal");
        if (corrupt == 1) reply[96 + 95] = 'x'; /* unterminated home */
        reply_bytes = 272;
    }
}
int ciuki_channel_send(int fd, const struct ciuki_message *m, uint32_t flags)
{
    CHECK(fd == 900 && flags == 1 && !m->fd_count && m->length >= 64);
    const uint8_t *h = m->data;
    uint32_t take = rd32(h + 36);
    CHECK(rd32(h) == 0x31435343u && rd32(h + 4) == 1 && rd32(h + 8) == 64 && rd32(h + 12) == 2 && rd32(h + 20) == 1 &&
          rd32(h + 32) == endpoint_generation && take <= 192 && m->length == 64 + take && rd32(h + 56) == request_bytes &&
          !rd32(h + 60));
    if (!request_bytes) { request_op = rd32(h + 16); request_id = rd32(h + 24); }
    CHECK(rd32(h + 16) == request_op && rd32(h + 24) == request_id && request_bytes + take <= rd32(h + 52));
    memcpy(request + request_bytes, h + 64, take); request_bytes += take;
    if (request_bytes == rd32(h + 52)) serve();
    return 1;
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    CHECK(fd == 900 && flags == 1);
    memset(m, 0, sizeof(*m));
    uint8_t *h = m->data;
    uint32_t take = reply_status ? 0 : reply_bytes - reply_at;
    if (take > 192) take = 192;
    wr32(h, 0x31435343u); wr32(h + 4, 1); wr32(h + 8, 64); wr32(h + 12, 2); wr32(h + 16, request_op); wr32(h + 20, 2);
    wr32(h + 24, request_id); wr32(h + 32, endpoint_generation); wr32(h + 36, take);
    wr32(h + 48, (uint32_t)reply_status); wr32(h + 52, reply_status ? 0 : reply_bytes); wr32(h + 56, reply_at);
    memcpy(h + 64, reply + reply_at, take);
    m->length = 64 + take; m->sender_pid = corrupt == 2 ? 6 : 5;
    reply_at += take;
    return 1;
}
static void expect(const struct passwd *p, const char *name, uid_t uid, gid_t gid, const char *dir)
{
    CHECK(p && !strcmp(p->pw_name, name) && p->pw_uid == uid && p->pw_gid == gid && !strcmp(p->pw_dir, dir) &&
          !strcmp(p->pw_shell, "/Programs/terminal") && !strcmp(p->pw_passwd, "*"));
}
int main(void)
{
    /* Own entry by uid and by case-insensitive name: the retained root's spelling. */
    caller_uid = 1007; caller_admin = 0;
    expect(getpwuid(1007), "Alice", 1007, 545, "/Windows/Profiles/u00000003/Documents");
    expect(getpwnam("alice"), "Alice", 1007, 545, "/Windows/Profiles/u00000003/Documents");
    CHECK(open_links == 0 && connects == closes && connects == 4);
    /* Another user's private mapping: EACCES for a Standard caller, no path. */
    errno = 0; CHECK(!getpwuid(1008) && errno == EACCES);
    struct passwd pw, *found = (void *)1; char buffer[256];
    CHECK(getpwnam_r("Bob", &pw, buffer, sizeof(buffer), &found) == EACCES && !found);
    /* Administrators may read other committed mappings. */
    caller_admin = 1;
    expect(getpwuid(1008), "Bob", 1008, 545, "/Windows/Profiles/u000003ef/Documents");
    expect(getpwnam("Guest"), "Guest", 501, 546, "/Windows/Profiles/u000001f5/Documents");
    /* Unknown account: not found, errno unchanged; SYSTEM is uid 0 without a lookup. */
    errno = 1234; CHECK(!getpwnam("Mallory") && errno == 1234);
    CHECK(getpwuid_r(4242, &pw, buffer, sizeof(buffer), &found) == 0 && !found);
    unsigned before = connects;
    expect(getpwuid(0), "SYSTEM", 0, 0, "/");
    expect(getpwnam("system"), "SYSTEM", 0, 0, "/");
    CHECK(connects == before);
    /* Caller storage: ERANGE without partial output, then success in place. */
    memset(buffer, 0x5a, sizeof(buffer));
    CHECK(getpwuid_r(1007, &pw, buffer, 20, &found) == ERANGE && !found && (uint8_t)buffer[0] == 0x5a);
    CHECK(getpwuid_r(1007, &pw, buffer, sizeof(buffer), &found) == 0 && found == &pw);
    CHECK(pw.pw_name >= buffer && pw.pw_shell < buffer + sizeof(buffer));
    expect(&pw, "Alice", 1007, 545, "/Windows/Profiles/u00000003/Documents");
    CHECK(getpwnam_r(NULL, &pw, buffer, sizeof(buffer), &found) == EINVAL && !found);
    CHECK(getpwnam_r("", &pw, buffer, sizeof(buffer), &found) == 0 && !found);
    /* Corrupt service data and a forged sender never become an entry. */
    corrupt = 1; errno = 0; CHECK(!getpwuid(1007) && errno == EIO);
    corrupt = 2; errno = 0; CHECK(!getpwuid(1007) && errno == EPIPE);
    corrupt = 0;
    CHECK(open_links == 0 && connects == closes);
    printf("H11 native passwd: own/admin/foreign PROFILE, retained root, SYSTEM, ERANGE, unknown, corrupt/forged replies; %u links all closed; checks=%u PASS\n",
           connects, checks);
    return 0;
}
