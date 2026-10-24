/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_HELPFILE_H
#define CIUKI_HELPFILE_H
#include <stddef.h>
#include <stdint.h>
#define HF_INPUT_LIMIT (16u * 1024u * 1024u)
#define HF_TEXT_LIMIT 60000u
#define HF_PAGE_LIMIT (1024u * 1024u)
#define HF_STORAGE_LIMIT (8u * 1024u * 1024u)
#define HF_TOPICS 1024u
#define HF_LINKS 128u
#define HF_KEYWORDS 4096u
#define HF_TOC 2048u
struct hf_link { size_t start, end; char target[256]; };
struct hf_anchor { char name[128]; size_t position; };
struct hf_page {
    char name[256], title[128]; char *text; size_t length;
    uint32_t address, reference;
    uint32_t browse_next, browse_prev;
    struct hf_link *links; unsigned link_count;
    struct hf_anchor *anchors; unsigned anchor_count;
};
struct hf_keyword { char name[128]; unsigned page; };
struct hf_context { uint32_t id, address; };
struct hf_toc { char title[128]; int page; unsigned depth; };
struct hf_document {
    unsigned format, count, keyword_count, context_count, hash_count, home;
    size_t storage;
    uint8_t *input; size_t input_size;
    struct hf_page *pages;
    struct hf_keyword *keywords;
    struct hf_context *contexts, *hashes;
    struct hf_toc *toc; unsigned toc_count;
    char source[256];
    void *archive;
};
uint16_t hf_u16(const void *);
uint32_t hf_u32(const void *);
int hf_range(size_t total, size_t offset, size_t length);
int hf_open(struct hf_document **, const void *, size_t);
int hf_open_file(struct hf_document **, const char *);
void hf_close(struct hf_document *);
int hf_add_page(struct hf_document *, const char *, const char *);
int hf_add_keyword(struct hf_document *, const char *, unsigned);
int hf_page_text(struct hf_document *, unsigned, const void *, size_t);
int hf_page_load(struct hf_document *, unsigned);
int hf_find(struct hf_document *, const char *);
int hf_context(struct hf_document *, uint32_t);
int hf_hash(struct hf_document *, uint32_t);
int hf_keyword(struct hf_document *, const char *, int prefix);
int hf_macro(struct hf_document *, const char *, unsigned current);
uint32_t hf_context_hash(const char *);
int hf_markup(struct hf_page *, const char *, size_t, int sitemap, struct hf_document *);
int hf_hlp_open(struct hf_document *);
int hf_chm_open(struct hf_document *);
int hf_chm_load(struct hf_document *, unsigned);
void hf_chm_close(struct hf_document *);
#endif
