/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "../desktop/users.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int acct_import_g32(const void *bytes, size_t n, const struct acct_import_options *options,
                    struct acct_db *out, struct acct_import_map maps[16], unsigned *count)
{
    if (!bytes || !options || !out || !maps || !count || n > 1400) return -EINVAL;
    struct users_db old = {0};
    int e = users_decode(&old, bytes, n);
    if (e) { acct_wipe(&old, sizeof(old)); return -EIO; }
    if (!old.count) { acct_wipe(&old, sizeof(old)); return -ENODATA; }
    for (unsigned i = 1; i < old.count; ++i) {
        struct users_record record = old.records[i];
        unsigned j = i;
        while (j && old.records[j - 1].id > record.id) { old.records[j] = old.records[j - 1]; --j; }
        old.records[j] = record;
        acct_wipe(&record, sizeof(record));
    }
    int chosen = -1;
    for (unsigned i = 0; i < old.count; ++i)
        if (old.records[i].id == options->first_legacy_id) chosen = (int)i;
    if (chosen < 0) { acct_wipe(&old, sizeof(old)); return -EINVAL; }
    struct acct_db draft = {0};
    struct acct_import_map mapping[16] = {0};
    draft.generation = 1; draft.origin = ACCT_UPGRADE; draft.flags = ACCT_MIGRATED;
    draft.next_uid = 1000 + old.count; draft.count = old.count + 2;
    draft.records[0].uid = 500; draft.records[0].flags = ACCT_BUILTIN; draft.records[0].groups = ACCT_ADMIN;
    strcpy(draft.records[0].name, "CiukiAdministrator");
    draft.records[1].uid = 501; draft.records[1].flags = ACCT_BUILTIN; draft.records[1].groups = ACCT_GUEST;
    strcpy(draft.records[1].name, "Guest");
    unsigned next = 3;
    for (unsigned i = 0; i < old.count; ++i) {
        const struct users_record *source = &old.records[i];
        const char *name = options->replacement_names[i][0] ? options->replacement_names[i] : source->name;
        if (!acct_name_valid(name) || acct_reserved_name(name)) { e = -EINVAL; break; }
        unsigned index = i == (unsigned)chosen ? 2 : next++;
        struct acct_record *r = &draft.records[index];
        r->uid = 998 + index; r->legacy_id = source->id;
        r->groups = i == (unsigned)chosen ? ACCT_ADMIN : ACCT_STANDARD;
        r->flags = ACCT_PASSWORD_SET | ACCT_CHANGE_REQUIRED;
        r->kdf = 1; r->iterations = ACCT_ITERATIONS;
        strcpy(r->name, name); memcpy(r->salt, source->salt, 16); memcpy(r->verifier, source->verifier, 32);
        uint8_t blank[32];
        e = acct_derive("", r->salt, ACCT_ITERATIONS, blank);
        if (!e && !acct_equal_secret(blank, r->verifier, 32)) r->flags |= ACCT_ENABLED;
        acct_wipe(blank, sizeof(blank));
        if (e) break;
        mapping[i].legacy_id = source->id; mapping[i].uid = r->uid;
        snprintf(mapping[i].root, 64, "/Windows/Profiles/u%08x", source->id);
    }
    if (!e) for (unsigned i = 2; i < draft.count; ++i)
        for (unsigned j = 2; j < i; ++j)
            if (acct_name_equal(draft.records[i].name, draft.records[j].name)) e = -EEXIST;
    if (!e) { *out = draft; memcpy(maps, mapping, sizeof(mapping)); *count = old.count; }
    acct_wipe(&draft, sizeof(draft)); acct_wipe(mapping, sizeof(mapping)); acct_wipe(&old, sizeof(old));
    return e;
}
