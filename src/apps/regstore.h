#ifndef CIUKIOS_REGSTORE_H
#define CIUKIOS_REGSTORE_H

/* Persistent registry substrate for the future Win32 API bridge.  The hive
 * names and value type numbers match the Win32 ANSI registry contract; the
 * on-disk format and editor are original CiukiOS implementations. */
#define CREG_SZ      1
#define CREG_BINARY  3
#define CREG_DWORD   4
#define CREG_KEY_MAX 96
#define CREG_NAME_MAX 64
#define CREG_DATA_MAX 256

int creg_get(const char *key, const char *name, int *type, void *data, int *size);
int creg_set(const char *key, const char *name, int type, const void *data, int size);
int creg_delete(const char *key, const char *name);

#endif
