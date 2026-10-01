/* CiukiOS registry storage.  A bounded append-only log keeps the most recent
 * value of each key/name pair.  An incomplete trailing record is discarded
 * on the next write; a record checksum prevents half-written data from being
 * treated as a valid setting.  This is shared by the editor and, later, the
 * Win32 Advapi compatibility bridge. */
#include "app.h"
#include "regstore.h"

#define REG_PATH "C:\\SYSTEM\\CONFIG\\REGISTRY.DAT"
#define REG_HEAD "CIREG01\n"
#define REG_HEAD_BYTES 8
#define REC_MAGIC 0x5243
#define OP_SET 1
#define OP_DELETE 2

#pragma pack(push, 1)
struct record_head {
    u16 magic;
    u8 op, type;
    u16 key_len, name_len, data_len, check;
};
#pragma pack(pop)

static struct record_head rh;
static char rec_key[CREG_KEY_MAX], rec_name[CREG_NAME_MAX];
static u8 rec_data[CREG_DATA_MAX];

static int key_valid(const char *key)
{
    const char *p = key;
    int n = str_len(key);
    if (n < 5 || n >= CREG_KEY_MAX) return 0;
    if (str_nicmp(p, "HKLM\\", 5) && str_nicmp(p, "HKCU\\", 5) &&
        str_nicmp(p, "HKCR\\", 5) && str_nicmp(p, "HKU\\", 4)) return 0;
    while (*p) {
        if (*p == '/' || *p == '|' || *p == '\r' || *p == '\n') return 0;
        p++;
    }
    return 1;
}

static u16 checksum(const struct record_head *h, const char *key,
                    const char *name, const u8 *data)
{
    u16 sum = (u16)(h->op + h->type + h->key_len + h->name_len + h->data_len);
    int i;
    for (i = 0; i < h->key_len; i++) sum = (u16)((sum << 5) + sum + (u8)key[i]);
    for (i = 0; i < h->name_len; i++) sum = (u16)((sum << 5) + sum + (u8)name[i]);
    for (i = 0; i < h->data_len; i++) sum = (u16)((sum << 5) + sum + data[i]);
    return sum;
}

static int read_exact(int h, void *p, int n)
{
    int r = dos_read(h, p, n);
    return r == n;
}

/* 1 record, 0 EOF, -1 invalid/incomplete tail. */
static int next_record(int h)
{
    int r = dos_read(h, &rh, sizeof rh);
    if (!r) return 0;
    if (r != sizeof rh || rh.magic != REC_MAGIC ||
        (rh.op != OP_SET && rh.op != OP_DELETE) ||
        rh.key_len < 5 || rh.key_len >= CREG_KEY_MAX ||
        rh.name_len >= CREG_NAME_MAX || rh.data_len > CREG_DATA_MAX ||
        (rh.op == OP_DELETE && rh.data_len)) return -1;
    if (!read_exact(h, rec_key, rh.key_len) ||
        !read_exact(h, rec_name, rh.name_len) ||
        !read_exact(h, rec_data, rh.data_len)) return -1;
    rec_key[rh.key_len] = 0;
    rec_name[rh.name_len] = 0;
    if (checksum(&rh, rec_key, rec_name, rec_data) != rh.check) return -1;
    return 1;
}

static int open_registry(int writing)
{
    int h;
    char head[REG_HEAD_BYTES];
    h = dos_open(REG_PATH, writing ? 2 : 0);
    if (h < 0 && writing) {
        dos_mkdir("C:\\SYSTEM\\CONFIG");
        h = dos_create_new(REG_PATH);
        if (h < 0) h = dos_open(REG_PATH, 2);
        if (h < 0) return -1;
        if (dos_seek(h, 0, 2) == 0 &&
            dos_write(h, REG_HEAD, REG_HEAD_BYTES) != REG_HEAD_BYTES) {
            dos_close(h); return -1;
        }
        dos_close(h);
        h = dos_open(REG_PATH, 2);
    }
    if (h < 0) return -1;
    if (!read_exact(h, head, REG_HEAD_BYTES) ||
        mem_cmp(head, REG_HEAD, REG_HEAD_BYTES)) { dos_close(h); return -1; }
    return h;
}

int creg_get(const char *key, const char *name, int *type, void *data, int *size)
{
    int h, r, found = 0, max;
    if (!key_valid(key) || str_len(name) >= CREG_NAME_MAX || !size) return -1;
    max = *size;
    h = open_registry(0);
    if (h < 0) return -1;
    while ((r = next_record(h)) > 0) {
        if (str_icmp(rec_key, key) || str_icmp(rec_name, name)) continue;
        if (rh.op == OP_DELETE) { found = 0; continue; }
        if (rh.data_len > max) { found = -2; continue; }
        mem_copy(data, rec_data, rh.data_len);
        *size = rh.data_len;
        if (type) *type = rh.type;
        found = 1;
    }
    dos_close(h);
    return found == 1 ? 0 : found == -2 ? -2 : -1;
}

static int append(const char *key, const char *name, int type,
                  const void *data, int size, int op)
{
    int h, r;
    long last_good, end;
    if (!key_valid(key) || str_len(name) >= CREG_NAME_MAX ||
        size < 0 || size > CREG_DATA_MAX ||
        (op == OP_SET && type != CREG_SZ && type != CREG_BINARY && type != CREG_DWORD)) return -1;
    h = open_registry(1);
    if (h < 0) return -1;
    last_good = dos_seek(h, 0, 1);
    while ((r = next_record(h)) > 0) last_good = dos_seek(h, 0, 1);
    end = dos_seek(h, 0, 2);
    if (end != last_good) {
        /* DOS AH=40h/CX=0 truncates at the current position. */
        if (dos_seek(h, last_good, 0) != last_good || dos_write(h, &rh, 0) < 0) {
            dos_close(h); return -1;
        }
    }
    rh.magic = REC_MAGIC; rh.op = op; rh.type = type;
    rh.key_len = str_len(key); rh.name_len = str_len(name); rh.data_len = size;
    rh.check = checksum(&rh, key, name, (const u8 *)data);
    if (dos_seek(h, 0, 2) < 0 || dos_write(h, &rh, sizeof rh) != sizeof rh ||
        dos_write(h, key, rh.key_len) != rh.key_len ||
        dos_write(h, name, rh.name_len) != rh.name_len ||
        dos_write(h, data, size) != size) { dos_close(h); return -1; }
    dos_close(h);
    return 0;
}

int creg_set(const char *key, const char *name, int type, const void *data, int size)
{
    if (!data || (type == CREG_DWORD && size != 4)) return -1;
    return append(key, name, type, data, size, OP_SET);
}

int creg_delete(const char *key, const char *name)
{
    return append(key, name, 0, "", 0, OP_DELETE);
}
