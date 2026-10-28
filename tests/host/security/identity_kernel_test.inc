/* sec-user-05 H11: the production ADVAPI32 identity facade
 * (sdk/win32/advapi32/identity.c, compiled separately in W32_HOST_MODEL)
 * over the production kernel token objects of this fixture
 * (proc/win32/objects.c W32_OBJ_TOKEN). Only the transports are adapted:
 * a facade buffer crosses through the calling process's user memory as the
 * real gate would, the per-install machine SID and the ACCOUNTS records are
 * boundary fixtures. Expected SIDs and layouts are written out
 * independently below. Credential admission of the Standard/Guest
 * subjects is faked at the boundary (as tests/host/security/subject_fixture.c),
 * every query uses the real security.c. SPDX-License-Identifier: GPL-2.0-only */
typedef uint16_t H11_WCHAR;
/* sdk/win32/services/bytes.c (linked with the facade). */
uint32_t w32_u32(const void *);
void w32_put32(void *, uint32_t);
extern int OpenProcessToken(void *, uint32_t, void **);
extern int OpenThreadToken(void *, uint32_t, int, void **);
extern int GetTokenInformation(void *, uint32_t, void *, uint32_t, uint32_t *);
extern int CheckTokenMembership(void *, const void *, int *);
extern int AllocateAndInitializeSid(const void *, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                    uint32_t, uint32_t, uint32_t, void **);
extern void *FreeSid(void *);
extern int IsValidSid(const void *);
extern uint32_t GetSidLengthRequired(uint8_t);
extern int InitializeSid(void *, const void *, uint8_t);
extern uint32_t *GetSidSubAuthority(void *, uint32_t);
extern uint8_t *GetSidSubAuthorityCount(void *);
extern int CopySid(uint32_t, void *, const void *);
extern int LookupAccountSidA(const char *, const void *, char *, uint32_t *, char *, uint32_t *, uint32_t *);
extern int LookupAccountSidW(const H11_WCHAR *, const void *, H11_WCHAR *, uint32_t *, H11_WCHAR *, uint32_t *, uint32_t *);
extern int LookupPrivilegeValueA(const char *, const char *, void *);
extern int LookupPrivilegeValueW(const H11_WCHAR *, const H11_WCHAR *, void *);
extern int GetUserNameA(char *, uint32_t *);
extern int GetUserNameW(H11_WCHAR *, uint32_t *);

static struct proc_thread *h11_thread;
static uint32_t h11_scratch, h11_error, h11_object_calls, h11_account_calls;
static uint32_t h11_machine_result;
/* Facade transport: the record's buffer is user memory of the caller. */
uint32_t w32r_object(struct ciuki_w32_object_args *a, void *buffer)
{
    h11_object_calls++;
    a->header = (struct ciuki_w32_header){1, sizeof(*a), W32_OBJECT, 0};
    if (buffer) a->name = h11_scratch;
    if (w32_object_call(h11_thread, a)) return 87;
    if (!a->error && buffer && a->action == W32_OBJ_QUERY)
        CHECK(!ua_read(h11_thread->process->memory, buffer, h11_scratch, a->result));
    return a->error;
}
uint32_t w32r_machine(uint32_t sid[3])
{
    if (h11_machine_result) return h11_machine_result;
    sid[0] = 0x11111111u; sid[1] = 0x22222222u; sid[2] = 0x33333333u; return 0;
}
/* ACCOUNTS LOOKUP=1 boundary fixture: public records only. */
static const struct { uint32_t uid, groups; const char *name; } h11_accounts[] = {
    {500, 3, "CiukiAdministrator"}, {501, 4, "Guest"}, {1000, 3, "Alice"}, {1001, 2, "Bob"}};
int32_t w32r_accounts(uint32_t op, const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used)
{
    h11_account_calls++;
    const uint8_t *q = in; uint8_t *r = out;
    /* LOOKUP: version 1, zero flags/secrets/picture/operation id, one selector. */
    CHECK(op == 1 && n == 312 && cap >= 160 && w32_u32(q) == 1 && !w32_u32(q + 4) && !w32_u32(q + 28) &&
          !w32_u32(q + 304) && !w32_u32(q + 308) && (!w32_u32(q + 24) || !w32_u32(q + 36)));
    uint32_t uid = w32_u32(q + 36), name_bytes = w32_u32(q + 24);
    for (unsigned i = 0; i < sizeof(h11_accounts) / sizeof(*h11_accounts); i++) {
        if (name_bytes ? strcmp((const char *)q + 48, h11_accounts[i].name) : uid != h11_accounts[i].uid) continue;
        memset(r, 0, 160); w32_put32(r, 1); w32_put32(r + 4, 1); w32_put32(r + 8, 77);
        w32_put32(r + 16, h11_accounts[i].uid); w32_put32(r + 20, h11_accounts[i].groups);
        memcpy(r + 32, h11_accounts[i].name, strlen(h11_accounts[i].name));
        *used = 160; return 0;
    }
    *used = 0; return -3; /* ESRCH */
}
int w32r_release(void *);
static void *h11_blocks[16];
void *w32r_alloc(uint32_t n)
{
    for (unsigned i = 0; i < 16; i++) if (!h11_blocks[i]) return h11_blocks[i] = calloc(1, n ? n : 1);
    return NULL;
}
void w32r_free(void *p) { (void)w32r_release(p); }
int w32r_release(void *p)
{
    for (unsigned i = 0; i < 16; i++) if (p && h11_blocks[i] == p) { free(p); h11_blocks[i] = 0; return 1; }
    return 0;
}
void w32r_last_error(uint32_t e) { h11_error = e; }
int GetComputerNameA(char *out, uint32_t *n)
{
    if (*n < 8) { *n = 8; return 0; }
    memcpy(out, "CIUKIOS", 8); *n = 7; return 1;
}
/* Independent SID spellings (Microsoft well-known SIDs; contract 8.1). */
static const uint8_t h11_admins[16] = {1,2,0,0,0,0,0,5, 32,0,0,0, 0x20,2,0,0};
static const uint8_t h11_users[16] = {1,2,0,0,0,0,0,5, 32,0,0,0, 0x21,2,0,0};
static const uint8_t h11_guests[16] = {1,2,0,0,0,0,0,5, 32,0,0,0, 0x22,2,0,0};
static const uint8_t h11_system[12] = {1,1,0,0,0,0,0,5, 18,0,0,0};
static void h11_user_sid(uint8_t out[28], uint32_t uid)
{
    static const uint8_t head[24] = {1,5,0,0,0,0,0,5, 21,0,0,0, 0x11,0x11,0x11,0x11, 0x22,0x22,0x22,0x22, 0x33,0x33,0x33,0x33};
    memcpy(out, head, 24); w32_put32(out + 24, uid);
}
struct h11_sa { void *sid; uint32_t attributes; };
struct h11_groups { uint32_t count; struct h11_sa g[]; };
static void *h11_token(void)
{
    void *token = NULL;
    CHECK(OpenProcessToken((void *)(uintptr_t)W32_CURRENT_PROCESS, 8, &token) && token);
    return token;
}
static void h11_close(void *token)
{
    struct ciuki_w32_object_args c = {.action = W32_OBJ_CLOSE, .handle = (uint32_t)(uintptr_t)token};
    CHECK(!w32r_object(&c, 0));
}
/* Expected view of one credential class. */
static void h11_check_view(uint32_t uid, const uint8_t *const *groups, unsigned group_count,
                           const uint8_t *primary, unsigned primary_bytes, unsigned privileges, uint32_t session)
{
    void *token = h11_token();
    uint8_t buffer[512] __attribute__((aligned(16))); uint32_t need = 0;
    uint8_t user[28]; unsigned user_bytes = uid ? 28 : 12;
    if (uid) h11_user_sid(user, uid); else memcpy(user, h11_system, 12);
    /* TokenUser: sizing, then the self-contained layout. */
    memset(buffer, 0xa5, sizeof(buffer));
    CHECK(!GetTokenInformation(token, 1, NULL, 0, &need) && h11_error == 122 && need == sizeof(struct h11_sa) + user_bytes);
    CHECK(!GetTokenInformation(token, 1, buffer, need - 1, &need) && h11_error == 122 && buffer[0] == 0xa5);
    CHECK(GetTokenInformation(token, 1, buffer, sizeof(buffer), &need) && need == sizeof(struct h11_sa) + user_bytes);
    struct h11_sa *sa = (void *)buffer;
    CHECK(sa->sid == buffer + sizeof(*sa) && !sa->attributes && !memcmp(sa->sid, user, user_bytes));
    /* TokenOwner and TokenPrimaryGroup: one PSID then the SID. */
    CHECK(GetTokenInformation(token, 4, buffer, sizeof(buffer), &need) && need == sizeof(void *) + user_bytes &&
          *(void **)buffer == buffer + sizeof(void *) && !memcmp(buffer + sizeof(void *), user, user_bytes));
    CHECK(GetTokenInformation(token, 5, buffer, sizeof(buffer), &need) && need == sizeof(void *) + primary_bytes &&
          !memcmp(buffer + sizeof(void *), primary, primary_bytes));
    /* TokenGroups: the actual membership, attributes 7. */
    unsigned expected = offsetof(struct h11_groups, g);
    for (unsigned i = 0; i < group_count; i++) expected += sizeof(struct h11_sa) + (groups[i] == h11_system ? 12 : 16);
    CHECK(!GetTokenInformation(token, 2, buffer, 4, &need) && h11_error == 122 && need == expected);
    CHECK(GetTokenInformation(token, 2, buffer, sizeof(buffer), &need) && need == expected);
    struct h11_groups *g = (void *)buffer;
    CHECK(g->count == group_count);
    for (unsigned i = 0; i < group_count; i++)
        CHECK(g->g[i].attributes == 7 && !memcmp(g->g[i].sid, groups[i], groups[i] == h11_system ? 12 : 16) &&
              (uint8_t *)g->g[i].sid >= buffer + offsetof(struct h11_groups, g) + group_count * sizeof(struct h11_sa) &&
              (uint8_t *)g->g[i].sid < buffer + need);
    /* TokenPrivileges: granted entries only, local LUIDs, attributes 3. */
    unsigned count = __builtin_popcount(privileges);
    CHECK(GetTokenInformation(token, 3, buffer, sizeof(buffer), &need) && need == 4 + 12 * count && w32_u32(buffer) == count);
    for (unsigned bit = 0, at = 4; bit < 3; bit++) if (privileges & (1u << bit)) {
        CHECK(w32_u32(buffer + at) == bit + 1 && !w32_u32(buffer + at + 4) && w32_u32(buffer + at + 8) == 3); at += 12;
    }
    CHECK(GetTokenInformation(token, 8, buffer, 4, &need) && need == 4 && w32_u32(buffer) == 1);
    CHECK(GetTokenInformation(token, 12, buffer, 4, &need) && need == 4 && w32_u32(buffer) == session);
    CHECK(!GetTokenInformation(token, 12, buffer, 3, &need) && h11_error == 122 && need == 4);
    CHECK(!GetTokenInformation(token, 20, buffer, sizeof(buffer), &need) && h11_error == 87); /* no TokenElevation story */
    CHECK(!GetTokenInformation(token, 1, buffer, sizeof(buffer), NULL) && h11_error == 998);
    /* Membership of the caller's own credential; an explicit primary token is refused. */
    int member = 7;
    CHECK(CheckTokenMembership(NULL, h11_admins, &member) && member == (groups[0] == h11_admins));
    CHECK(CheckTokenMembership(NULL, h11_users, &member));
    int users = 0; for (unsigned i = 0; i < group_count; i++) users |= groups[i] == h11_users;
    CHECK(member == users);
    CHECK(CheckTokenMembership(NULL, h11_guests, &member) && member == (groups[0] == h11_guests));
    CHECK(CheckTokenMembership(NULL, user, &member) && member == 1);
    uint8_t other[28]; h11_user_sid(other, uid + 1);
    CHECK(CheckTokenMembership(NULL, other, &member) && !member);
    CHECK(!CheckTokenMembership(token, h11_admins, &member) && h11_error == 1349);
    CHECK(!CheckTokenMembership(NULL, "\x02\x00", &member) && h11_error == 1337);
    h11_close(token);
    CHECK(!GetTokenInformation(token, 1, buffer, sizeof(buffer), &need) && h11_error == 6);
}
static struct security_subject h11_subject;
static struct security_subject *h11_swap(struct process *p, uint32_t uid, const uint32_t *groups, unsigned n,
                                         uint32_t primary, uint32_t session)
{
    struct security_subject *saved = p->subject;
    h11_subject = *saved;
    h11_subject.refs = 1; h11_subject.session = NULL; h11_subject.origin = NULL; h11_subject.live = true;
    h11_subject.credential.uid = uid; h11_subject.credential.primary_gid = primary;
    h11_subject.credential.group_count = n; memset(h11_subject.credential.groups, 0, sizeof(h11_subject.credential.groups));
    memcpy(h11_subject.credential.groups, groups, n * sizeof(*groups));
    h11_subject.credential.session_id = session;
    h11_subject.credential.flags = session ? CIUKI_CRED_INTERACTIVE : 0;
    p->subject = &h11_subject;
    return saved;
}
static void test_identity_facades(void)
{
    proc_init();
    struct process *owner, *alice;
    (void)security_fixture_user(&owner);
    struct proc_thread *a = object_process(owner, &alice);
    h11_thread = a; g_current = a->task; g_current->state = T_RUNNING;
    struct ciuki_w32_vm scratch = request(0, PAGE_SIZE, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE);
    CHECK(!w32_vm_alloc(alice->memory, &scratch));
    h11_scratch = scratch.address;
    const struct ciuki_credential *real = security_credential(alice);
    CHECK(real->uid == 1000 && real->session_id);
    uint32_t session = real->session_id;
    /* A machine identity failure (before the per-process cache is filled) is
     * an error, never a fabricated or clock-derived SID. */
    h11_machine_result = 1062; uint8_t first[128] __attribute__((aligned(16))); uint32_t first_need = 0;
    void *probe = h11_token();
    CHECK(!GetTokenInformation(probe, 1, first, sizeof(first), &first_need) && h11_error == 1062);
    CHECK(GetTokenInformation(probe, 2, first, sizeof(first), &first_need)); /* groups need no machine SID */
    char failed[32]; uint32_t failed_n = sizeof(failed); uint8_t foreign[28]; h11_user_sid(foreign, 1000);
    uint32_t dn0 = sizeof(failed), use0 = 0; char dom0[32];
    CHECK(!LookupAccountSidA(NULL, foreign, failed, &failed_n, dom0, &dn0, &use0) && h11_error == 1062);
    h11_close(probe); h11_machine_result = 0;
    /* Administrator (bootstrap uid 1000, Administrators + Users). */
    const uint8_t *admin_groups[] = {h11_admins, h11_users};
    h11_check_view(1000, admin_groups, 2, h11_users, 16, 7, session);
    char name[32]; uint32_t n = 1;
    CHECK(!GetUserNameA(name, &n) && h11_error == 122 && n == 6);
    n = 6; CHECK(GetUserNameA(name, &n) && n == 6 && !strcmp(name, "Alice"));
    H11_WCHAR wide[32]; n = 32;
    CHECK(GetUserNameW(wide, &n) && n == 6 && wide[0] == 'A' && wide[4] == 'e' && !wide[5]);
    CHECK(!GetUserNameA(name, NULL) && h11_error == 87);
    /* Only query rights open a token; adjust/duplicate/impersonate/assign fail first. */
    void *token = (void *)1;
    uint32_t calls = h11_object_calls;
    CHECK(!OpenProcessToken((void *)(uintptr_t)W32_CURRENT_PROCESS, 0x28, &token) && h11_error == 5 && !token &&
          calls == h11_object_calls);
    CHECK(!OpenProcessToken((void *)(uintptr_t)W32_CURRENT_PROCESS, 0x2 | 0x8, &token) && h11_error == 5);
    CHECK(OpenProcessToken((void *)(uintptr_t)W32_CURRENT_PROCESS, 0x20008, &token)); h11_close(token);
    CHECK(OpenProcessToken((void *)(uintptr_t)W32_CURRENT_PROCESS, 0x02000000, &token)); h11_close(token);
    CHECK(!OpenProcessToken((void *)(uintptr_t)W32_CURRENT_PROCESS, 8, NULL) && h11_error == 998);
    CHECK(!OpenProcessToken((void *)(uintptr_t)0x777770, 8, &token) && h11_error == 5 && !token);
    CHECK(!OpenThreadToken((void *)(uintptr_t)W32_CURRENT_THREAD, 8, 0, &token) && h11_error == 1008 && !token);
    /* Local lookups. */
    uint8_t sid[28]; h11_user_sid(sid, 1000);
    char who[40], domain[40]; uint32_t wn = 1, dn = 1, use = 0;
    CHECK(!LookupAccountSidA(NULL, sid, who, &wn, domain, &dn, &use) && h11_error == 122 && wn == 6 && dn == 8);
    wn = sizeof(who); dn = sizeof(domain);
    CHECK(LookupAccountSidA(NULL, sid, who, &wn, domain, &dn, &use) && !strcmp(who, "Alice") && wn == 5 &&
          !strcmp(domain, "CIUKIOS") && dn == 7 && use == 1);
    H11_WCHAR ww[40], wd[40]; wn = 40; dn = 40;
    const H11_WCHAR local[] = {'\\', '\\', 'c', 'i', 'u', 'k', 'i', 'o', 's', 0};
    CHECK(LookupAccountSidW(local, sid, ww, &wn, wd, &dn, &use) && ww[0] == 'A' && !ww[5] && wn == 5 && use == 1);
    wn = sizeof(who); dn = sizeof(domain);
    CHECK(LookupAccountSidA(NULL, h11_admins, who, &wn, domain, &dn, &use) && !strcmp(who, "Administrators") &&
          !strcmp(domain, "BUILTIN") && use == 4);
    wn = sizeof(who); dn = sizeof(domain);
    CHECK(LookupAccountSidA("", h11_system, who, &wn, domain, &dn, &use) && !strcmp(who, "SYSTEM") &&
          !strcmp(domain, "NT AUTHORITY") && use == 5);
    wn = sizeof(who); dn = sizeof(domain);
    CHECK(!LookupAccountSidA("OTHERPC", sid, who, &wn, domain, &dn, &use) && h11_error == 50);
    h11_user_sid(sid, 4242);
    CHECK(!LookupAccountSidA(NULL, sid, who, &wn, domain, &dn, &use) && h11_error == 1332);
    h11_user_sid(sid, 1000); sid[12] ^= 1; /* another machine's domain: no network trust */
    CHECK(!LookupAccountSidA(NULL, sid, who, &wn, domain, &dn, &use) && h11_error == 1332);
    uint8_t luid[8];
    CHECK(LookupPrivilegeValueA(NULL, "seshutdownprivilege", luid) && w32_u32(luid) == 1 && !w32_u32(luid + 4));
    CHECK(LookupPrivilegeValueA(NULL, "SeSystemtimePrivilege", luid) && w32_u32(luid) == 2);
    const H11_WCHAR driver[] = {'S','e','L','o','a','d','D','r','i','v','e','r','P','r','i','v','i','l','e','g','e',0};
    CHECK(LookupPrivilegeValueW(NULL, driver, luid) && w32_u32(luid) == 3);
    CHECK(!LookupPrivilegeValueA(NULL, "SeDebugPrivilege", luid) && h11_error == 1313);
    CHECK(!LookupPrivilegeValueA(NULL, "SeTcbPrivilege", luid) && h11_error == 1313);
    CHECK(!LookupPrivilegeValueA("\\\\SERVER", "SeShutdownPrivilege", luid) && h11_error == 50);
    /* SID helpers: data only, bounded, never an account or a right. */
    void *made = NULL; const uint8_t nt[6] = {0,0,0,0,0,5};
    CHECK(AllocateAndInitializeSid(nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &made) && !memcmp(made, h11_admins, 16));
    CHECK(IsValidSid(made) && GetSidLengthRequired(2) == 16 && *GetSidSubAuthorityCount(made) == 2 &&
          *GetSidSubAuthority(made, 1) == 544 && !GetSidSubAuthority(made, 2) && h11_error == 1337);
    uint8_t copy[16]; memset(copy, 0, sizeof(copy));
    CHECK(!CopySid(15, copy, made) && h11_error == 122 && !copy[0]);
    CHECK(CopySid(16, copy, made) && !memcmp(copy, h11_admins, 16));
    int member = 0;
    CHECK(CheckTokenMembership(NULL, made, &member) && member == 1); /* still the kernel credential's answer */
    CHECK(!FreeSid(made));
    CHECK(!AllocateAndInitializeSid(nt, 9, 1, 2, 3, 4, 5, 6, 7, 8, &made) && h11_error == 1337);
    CHECK(FreeSid(copy) == copy && h11_error == 87);
    uint8_t init[16];
    CHECK(InitializeSid(init, nt, 2) && init[0] == 1 && init[1] == 2 && init[7] == 5 && !InitializeSid(init, nt, 16));
    uint8_t view[128] __attribute__((aligned(16))); uint32_t need = 0;
    /* Standard user (Users), Guest (Guests), SYSTEM: the kernel view only. */
    const uint32_t standard[] = {545}, guest[] = {546}, system[] = {0};
    struct security_subject *saved = h11_swap(alice, 1001, standard, 1, 545, session);
    const uint8_t *standard_groups[] = {h11_users};
    h11_check_view(1001, standard_groups, 1, h11_users, 16, 1, session);
    n = sizeof(name); CHECK(GetUserNameA(name, &n) && !strcmp(name, "Bob"));
    h11_swap(alice, 501, guest, 1, 546, session);
    const uint8_t *guest_groups[] = {h11_guests};
    h11_check_view(501, guest_groups, 1, h11_guests, 16, 1, session);
    h11_swap(alice, 0, system, 1, 0, 0);
    const uint8_t *system_groups[] = {h11_system};
    h11_check_view(0, system_groups, 1, h11_system, 12, 7, 0);
    n = sizeof(name); calls = h11_account_calls;
    CHECK(GetUserNameA(name, &n) && !strcmp(name, "SYSTEM") && calls == h11_account_calls);
    /* An account the service does not publish has no name. */
    h11_swap(alice, 1500, standard, 1, 545, session);
    n = sizeof(name); CHECK(!GetUserNameA(name, &n) && h11_error == 1245);
    alice->subject = saved;
    /* Session revocation invalidates a retained query token (sec-k06). */
    token = h11_token();
    uint32_t state; CHECK(!security_session_end(session, true, &state));
    CHECK(!GetTokenInformation(token, 1, view, sizeof(view), &need) && h11_error == 5); /* caller's session ended */
    g_current = &controller; proc_discard(alice); proc_discard(owner); proc_collect();
    CHECK(!security_session_end(session, false, &state) && state == CIUKI_SECURITY_SESSION_DEAD);
    for (unsigned i = 0; i < 16; i++) CHECK(!h11_blocks[i]);
    printf("H11 kernel tokens: admin/Standard/Guest/SYSTEM views, sizes, membership, lookups, privileges, refusals, revocation (%u object calls, %u account lookups) PASS\n",
           h11_object_calls, h11_account_calls);
}
