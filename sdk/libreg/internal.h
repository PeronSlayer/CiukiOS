/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_REG_INTERNAL_H
#define CIUKI_REG_INTERNAL_H
#include <ciuki/registry.h>
struct rv { char *name; enum reg_type type; void *data; size_t size; struct rv *next; };
struct rk { char *path; struct rv *values; struct rk *next; };
struct rt { struct rk *keys; char *comments; uint64_t generation; unsigned nk, nv; };
struct reg_root { const char *name; char *directory; struct rt *tree; char *snapshot; size_t snapshot_size; };
int reg_fail(int);
void reg_tree_free(struct rt *);
struct rt *reg_tree_new(const char *);
struct rk *reg_key(struct rt *, const char *);
struct rk *reg_ensure(struct rt *, const char *);
int reg_put(struct rt *, const char *, const char *, enum reg_type, const void *, size_t);
struct rt *reg_hardware(uint64_t previous);
char *reg_serialise(struct rt *, const char *, size_t *);
struct rt *reg_parse(const char *, const char *, size_t);
int reg_path_valid(const char *, const char *);
int reg_load(struct reg_root *);
int reg_commit(struct reg_root *, struct rt *, uint64_t);
int reg_file_read(const char *, char **, size_t *);
int reg_recover(const char *, const char *);
int reg_file_write(const char *, const char *, const char *, size_t, uint64_t);
#endif
