/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ciuki/registry.h>
#include <ciuki/devices.h>
#include "desktop/device_fixture.h"
static const char *race_file; static unsigned race_reads;
static int nonreplacing, crash_sync, crash_rename, fail_sync, syncs, renames;
int __real_fsync(int);
int __wrap_fsync(int fd)
{
    int result = __real_fsync(fd); syncs++;
    if (crash_sync && syncs == crash_sync) _exit(77);
    if (fail_sync && syncs == fail_sync) { errno = EIO; return -1; }
    return result;
}
int __real_rename(const char *, const char *);
int __wrap_rename(const char *a, const char *b)
{
    struct stat st; if (nonreplacing && !stat(b, &st)) { errno = EEXIST; return -1; }
    int result = __real_rename(a, b);
    if (!result && crash_rename && ++renames == crash_rename) _exit(77);
    return result;
}
FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *file, const char *mode)
{
    if (race_file && !strcmp(file, race_file) && !strcmp(mode, "rb") && ++race_reads == 2) {
        FILE *f = __real_fopen(file, "wb"); assert(f);
        assert(fputs("; ciuki-registry generation=50\n[System]\nexternal = 99\n", f) >= 0); assert(!fclose(f));
    }
    return __real_fopen(file, mode);
}
static int full_inventory;
int ciuki_devices(struct ciuki_devices_info *s)
{
    device_fixture(s);
    if (full_inventory) {
        struct ciuki_device_record source = s->devices[7];
        s->generation = 43; s->count = CIUKI_DEVICES_MAX;
        for (unsigned i = 0; i < s->count; i++) {
            s->devices[i] = source; s->devices[i].bus = i / 32; s->devices[i].slot = i % 32;
            s->devices[i].resource_count = CIUKI_DEVICE_RESOURCES;
            for (unsigned j = 0; j < CIUKI_DEVICE_RESOURCES; j++)
                s->devices[i].resources[j] = (struct ciuki_device_resource){CIUKI_RESOURCE_MEMORY, 0, 0xfe000000, 0xfe001000};
        }
    }
    return 0;
}
static char *path(const char *directory, const char *name)
{
    size_t n = strlen(directory) + strlen(name) + 2; char *p = malloc(n); assert(p);
    snprintf(p, n, "%s/%s", directory, name); return p;
}
static void write_file(const char *file, const char *text)
{
    FILE *f = fopen(file, "wb"); assert(f); assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(!fclose(f));
}
static char *read_file(const char *file)
{
    FILE *f = fopen(file, "rb"); assert(f); assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n >= 0); rewind(f);
    char *s = malloc((size_t)n + 1); assert(s); assert(fread(s, 1, (size_t)n, f) == (size_t)n); s[n] = 0; fclose(f); return s;
}
static void no_journal(const char *dir)
{
    const char *const suffixes[] = {"System.reg.new", "System.reg.bak", "System.reg.txn"};
    for (unsigned i = 0; i < 3; i++) { char *p = path(dir, suffixes[i]); assert(access(p, F_OK) && errno == ENOENT); free(p); }
}
static void roundtrip(const char *dir)
{
    struct reg_root *a = reg_open_root_at("system", dir), *b = reg_open_root_at("System", dir); assert(a && b);
    assert(!reg_generation(a));
    assert(!reg_set_string(a, "sYsTeM\\Version", "name", "Ciuki\n\"dog\"\\; è"));
    assert(!reg_set_int32(a, "System\\Version", "integer", INT32_MIN));
    assert(!reg_set_bool(a, "System\\Version", "active", 1));
    unsigned char bytes[] = {0, 255, 127}; assert(!reg_set_binary(a, "System\\Version", "bytes", bytes, sizeof(bytes)));
    assert(!reg_set_string(a, "System\\Straße\\ÉCOLE", "Σ", "Unicode"));
    assert(!reg_set_int32(b, "SYSTEM\\strasse\\école", "σ", 42)); /* Latest edit replaces same Unicode name. */
    assert(reg_generation(a) == 6 && reg_generation(b) == 6);
    int32_t value; assert(!reg_get_int32(a, "System\\Version", "integer", &value) && value == INT32_MIN);
    assert(!reg_get_int32(a, "System\\STRASSE\\École", "ς", &value) && value == 42);
    int active; assert(!reg_get_bool(a, "System\\Version", "active", &active) && active);
    char s[128]; size_t n = 1; s[0] = 'x'; assert(reg_get_string(a, "System\\Version", "name", s, &n) && errno == ERANGE && s[0] == 'x');
    size_t need = n; n = sizeof(s); assert(!reg_get_string(a, "System\\Version", "NAME", s, &n) && n == need && !strcmp(s, "Ciuki\n\"dog\"\\; è"));
    unsigned char out[3]; n = sizeof(out); assert(!reg_get_binary(a, "System\\Version", "bytes", out, &n) && !memcmp(out, bytes, n));
    assert(reg_get_int32(a, "System\\Version", "name", &value) && errno == EINVAL);
    char child[257]; assert(!reg_enum_keys(a, "System", 0, child, sizeof(child)) && reg_name_equal(child, "Version"));
    assert(!reg_enum_keys(a, "System", 1, child, sizeof(child)) && reg_name_equal(child, "Straße"));
    assert(reg_enum_keys(a, "System", 2, child, sizeof(child)) && errno == ENOENT);
    assert(reg_enum_keys(a, "System\\Version", 0, child, sizeof(child)) && errno == ENOENT);
    struct reg_value_info info; assert(!reg_enum_values(a, "System\\Version", 0, &info) && !strcmp(info.name, "name") && info.type == REG_STRING);
    uint64_t before = reg_generation(a); assert(reg_set_text(a, "System", "bad", "2147483648") && errno == EINVAL && reg_generation(a) == before);
    assert(!reg_set_text(a, "System", "", "hex:"));
    char *file = path(dir, "System.reg"); char *text = read_file(file);
    assert(strstr(text, "[System\\Version]") && !strstr(text, "[sYsTeM"));
    assert(strstr(text, "generation=7") && strstr(text, "\\n\\\"dog\\\"\\\\;"));
    char *export = path(dir, "../branch.reg"); /* Export rejects lexical traversal. */
    assert(reg_export(a, "System\\Version", export) && errno == EINVAL); free(export);
    size_t cap = strlen(dir) + 32; export = malloc(cap); assert(export); char *slash = strrchr(dir, '/'); assert(slash);
    snprintf(export, cap, "%.*s/branch.reg", (int)(slash - dir), dir);
    assert(!reg_export(a, "System\\Version", export));
    char *branch = read_file(export); assert(strstr(branch, "Version") && !strstr(branch, "ÉCOLE")); free(branch);
    assert(reg_export(a, "System", file) && errno == EACCES);
    assert(!reg_delete(a, "System\\Version", NULL)); assert(reg_get_bool(b, "System\\Version", "active", &active) && errno == ENOENT);
    assert(!reg_import(b, export)); assert(!reg_get_bool(a, "System\\Version", "active", &active));
    assert(!reg_delete(a, "System\\STRASSE", NULL)); assert(reg_get_int32(b, "System\\strasse\\école", "σ", &value) && errno == ENOENT);
    assert(reg_delete(a, "System", NULL) && errno == EACCES);
    reg_close_root(a); reg_close_root(b); free(text); free(file); free(export); no_journal(dir);
    puts("PASS registry: typed/Unicode roundtrip, stale handles, branch export/import/delete, generation");
}
static void parser(const char *dir)
{
    char *file = path(dir, "System.reg");
    const char *good = "; first comment\r\n[System\\Sample]\r\nname = \"a;b\" ; trailing comment\r\n\"a=b\" = true\r\n[system\\sample]\r\nname = \"last\"\r\n";
    write_file(file, good); struct reg_root *r = reg_open_root_at("System", dir); assert(r);
    assert(!reg_set_int32(r, "System\\Sample", "count", 2147483647));
    char *text = read_file(file); assert(strstr(text, "; first comment") && strstr(text, "; trailing comment")); free(text);
    char out[128]; size_t n = sizeof(out); assert(!reg_get_string(r, "System\\sample", "name", out, &n) && !strcmp(out, "last"));
    const char *const bad[] = {"[Desktop]", "[System] garbage", "value = 2", "[System]\nx = hex:0", "[System]\nx = hex:gg",
        "[System]\nx = \"oops", "[System]\nx = \"\\q\"", "[System]\nx = True", "[System]\nx = -2147483649",
        "; ciuki-registry generation=-1\n[System]", "; ciuki-registry generation=1\n; ciuki-registry generation=2\n",
        "[System\\]\n", "[System\\ bad]\n", "[System\\a\xc0\xaf]\n", "[System]\nx = 1\n[Users]\ny=2"};
    char *import = path(dir, "input.reg");
    uint64_t generation = reg_generation(r);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) { write_file(import, bad[i]); assert(reg_import(r, import)); assert(reg_generation(r) == generation); }
    char key[400]; strcpy(key, "System\\"); memset(key + 7, 'a', 65); key[72] = 0;
    assert(reg_create_key(r, key) && errno == EINVAL);
    strcpy(key, "System"); for (unsigned i = 0; i < 15; i++) strcat(key, "\\a"); assert(!reg_create_key(r, key));
    strcat(key, "\\b"); assert(reg_create_key(r, key) && errno == EINVAL);
    char *large = malloc(REG_DATA_MAX + 2); assert(large); memset(large, 'a', REG_DATA_MAX + 1); large[REG_DATA_MAX + 1] = 0;
    assert(reg_set_string(r, "System", "big", large) && errno == E2BIG); large[REG_DATA_MAX] = 0;
    assert(!reg_set_string(r, "System", "big", large)); free(large);
    unsigned char *binary = calloc(REG_DATA_MAX + 1, 1); assert(binary);
    assert(reg_set_binary(r, "System", "bigbin", binary, REG_DATA_MAX + 1) && errno == E2BIG);
    assert(!reg_set_binary(r, "System", "bigbin", binary, REG_DATA_MAX)); free(binary);
    n = 0; assert(!reg_get_text(r, "System", "bigbin", NULL, &n) && n == REG_DATA_MAX * 2 + 5);
    char *literal = malloc(n); assert(literal); assert(!reg_get_text(r, "System", "bigbin", literal, &n)); assert(!reg_set_text(r, "System", "copybin", literal)); free(literal);
    assert(!reg_name_equal("bad\xff", "bad\xff"));
    reg_close_root(r); free(file); free(import); puts("PASS registry: parser/comment preservation, invalid imports unchanged, UTF-8/depth/name/data limits");
}
static void recover(const char *dir)
{
    char *file = path(dir, "System.reg");
    for (int native = 0; native < 2; native++) {
        for (int phase = 1; phase <= 5; phase++) {
            write_file(file, "; ciuki-registry generation=8\n[System]\nvalue = 10\n");
            pid_t pid = fork(); assert(pid >= 0);
            if (!pid) { struct reg_root *r = reg_open_root_at("System", dir); assert(r); nonreplacing = native; syncs = 0; crash_sync = phase; assert(!reg_set_int32(r, "System", "value", 20)); _exit(0); }
            int status; assert(waitpid(pid, &status, 0) == pid); assert(WIFEXITED(status));
            struct reg_root *r = reg_open_root_at("System", dir); assert(r); int32_t v; assert(!reg_get_int32(r, "System", "value", &v));
            assert(v == (phase == 1 ? 10 : 20)); assert(reg_generation(r) == (phase == 1 ? 8 : 9)); reg_close_root(r); no_journal(dir);
        }
        for (int phase = 1; phase <= (native ? 2 : 1); phase++) {
            write_file(file, "; ciuki-registry generation=8\n[System]\nvalue = 10\n");
            pid_t pid = fork(); assert(pid >= 0);
            if (!pid) { struct reg_root *r = reg_open_root_at("System", dir); assert(r); nonreplacing = native; crash_rename = phase; renames = 0; assert(!reg_set_int32(r, "System", "value", 30)); _exit(0); }
            int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 77);
            struct reg_root *r = reg_open_root_at("System", dir); assert(r); int32_t v; assert(!reg_get_int32(r, "System", "value", &v) && v == 30); reg_close_root(r); no_journal(dir);
        }
    }
    write_file(file, "; ciuki-registry generation=8\n[System]\nvalue = 10\n");
    struct reg_root *r = reg_open_root_at("System", dir); assert(r); syncs = 0; fail_sync = 1;
    assert(reg_set_int32(r, "System", "value", 40) && errno == EIO); fail_sync = 0;
    int32_t v; assert(!reg_get_int32(r, "System", "value", &v) && v == 10); reg_close_root(r); no_journal(dir);
    char *txn = path(dir, "System.reg.txn"), *temp = path(dir, "System.reg.new"), *bak = path(dir, "System.reg.bak");
    write_file(txn, "ciuki-registry 9 999 0\n"); write_file(temp, "[System]\nx = hex:xx");
    r = reg_open_root_at("System", dir); assert(r && reg_generation(r) == 8); reg_close_root(r); no_journal(dir);
    assert(!rename(file, bak)); write_file(txn, "ciuki-registry 9 999 0\n"); write_file(temp, "broken");
    r = reg_open_root_at("System", dir); assert(r && reg_generation(r) == 8); reg_close_root(r); no_journal(dir);
    write_file(txn, "broken intent"); assert(!reg_open_root_at("System", dir) && errno == EIO); assert(!unlink(txn));
    free(file); free(txn); free(temp); free(bak);
    puts("PASS registry: atomic POSIX/native fallback, fsync/rename crash checkpoints, I/O failure, invalid temp, backup recovery");
}
static void limits(const char *dir)
{
    char *file = path(dir, "System.reg"), *import = path(dir, "limit.reg");
    write_file(file, "[System]\n"); struct reg_root *r = reg_open_root_at("System", dir); assert(r);
    race_file = file; race_reads = 0;
    assert(reg_set_bool(r, "System", "mine", 1) && errno == EAGAIN); race_file = NULL;
    int32_t value; assert(reg_generation(r) == 50 && !reg_get_int32(r, "System", "external", &value) && value == 99);
    int active; assert(reg_get_bool(r, "System", "mine", &active) && errno == ENOENT);
    FILE *f = fopen(import, "wb"); assert(f);
    for (unsigned i = 0; i < REG_KEYS_MAX; i++) fprintf(f, "[System\\k%u]\n", i);
    fclose(f); assert(reg_import(r, import) && errno == E2BIG && reg_generation(r) == 50);
    f = fopen(import, "wb"); assert(f); fputs("[System]\n", f);
    for (unsigned i = 0; i <= REG_VALUES_MAX; i++) fprintf(f, "v%u = 1\n", i);
    fclose(f); assert(reg_import(r, import) && errno == E2BIG && reg_generation(r) == 50);
    f = fopen(import, "wb"); assert(f); for (unsigned i = 0; i <= REG_TEXT_MAX; i++) fputc(';', f); fclose(f);
    assert(reg_import(r, import) && errno == E2BIG && reg_generation(r) == 50);
    char key[REG_NAME_BYTES + 16]; strcpy(key, "System\\");
    for (unsigned i = 0; i < 64; i++) strcat(key, "🐶"); assert(!reg_create_key(r, key));
    assert(!reg_set_int32(r, "System\\a;quoted\"", "value;=", 1));
    assert(!reg_get_int32(r, "System\\a;quoted\"", "value;=", &value) && value == 1);
    write_file(file, "; ciuki-registry generation=18446744073709551614\n[System]\nx=1\n");
    assert(reg_set_int32(r, "System", "x", 2) && errno == EOVERFLOW && reg_generation(r) == UINT64_MAX - 1);
    reg_close_root(r); free(file); free(import);
    puts("PASS registry: generation race/overflow, whole-root key/value/text limits, 64 Unicode scalars, punctuation roundtrip");
}
static void hardware(const char *dir)
{
    struct reg_root *r = reg_open_root_at("Hardware", dir); assert(r && reg_read_only(r) && reg_generation(r) == 42);
    int32_t v; assert(!reg_get_int32(r, "Hardware\\PCI\\00:05.0", "vendor", &v) && v == 0x8086);
    assert(!reg_get_int32(r, "Hardware\\Audio", "count", &v) && v == 1);
    assert(!reg_get_int32(r, "Hardware\\PCI\\00:05.0", "state", &v) && v == CIUKI_DEVICE_QUARANTINED);
    char out[80]; size_t n = sizeof(out); assert(!reg_get_string(r, "Hardware\\PCI\\00:05.0\\Resources\\2", "end", out, &n) && !strcmp(out, "0x00000000fe000000"));
    assert(reg_set_int32(r, "Hardware", "bad", 1) && errno == EROFS); assert(reg_delete(r, "Hardware\\Audio", NULL) && errno == EROFS);
    assert(reg_import(r, "/missing") && errno == EROFS);
    char *file = path(dir, "Hardware.reg"); assert(access(file, F_OK) && errno == ENOENT); free(file);
    full_inventory = 1;
    assert(reg_generation(r) == 43);
    assert(!reg_get_int32(r, "Hardware\\Network", "count", &v) && v == 64);
    n = sizeof(out); assert(!reg_get_string(r, "Hardware\\Network\\63", "pci", out, &n) && !strcmp(out, "Hardware\\PCI\\01:1f.0"));
    n = sizeof(out); assert(!reg_get_string(r, "Hardware\\PCI\\01:1f.0\\Resources\\7", "end", out, &n));
    const char *slash = strrchr(dir, '/'); assert(slash);
    char *parent = strndup(dir, (size_t)(slash - dir)); assert(parent);
    char *export = path(parent, "hardware.reg"); free(parent);
    assert(!reg_export(r, "Hardware", export)); char *text = read_file(export);
    assert(strlen(text) <= REG_TEXT_MAX && strstr(text, "[Hardware\\PCI\\01:1f.0\\Resources\\7]")); free(text); free(export);
    full_inventory = 0;
    reg_close_root(r); puts("PASS registry: read-only Hardware, PCI/class summaries, resource extents, full 64-device inventory, generation refresh");
}
int main(int argc, char **argv)
{
    assert(argc == 2); roundtrip(argv[1]); parser(argv[1]); recover(argv[1]); limits(argv[1]); hardware(argv[1]); return 0;
}
