/* SPDX-License-Identifier: MIT
 * fx-14 (replaces the f6-14 amendment-1 constants: 8192 DOM nodes, 8192-byte
 * text chunks, 128 KiB of document text, 8192 boxes, depth 64, 64 columns).
 * A document is admitted against the memory actually free when it starts,
 * not against fixed tiny limits: the DOM ledger (libdom hubbub binding) and
 * the box ledger (box construction and layout) may each charge
 * free/CIUKI_DOC_FREE_SHARE bytes, so one document's structure never takes
 * more than half of the free memory and the other half stays for computed
 * styles, layout work, images, the cache and the rest of the system. The
 * ledgers charge before allocating (cumulative charges, as amendment 1
 * required); there is no per-chunk or per-run cap below the totals. Only an
 * exceeded budget, an excessive nesting depth or a table wider than HTML's
 * colspan limit rejects a page, each with its own message.
 * Rationale and host evidence: build/codex/fx-14-report.md. */
#ifndef CIUKI_NETSURF_DOCMEM_H
#define CIUKI_NETSURF_DOCMEM_H
#include <stddef.h>
/* Host model of free memory while NetSurf parses on the 128 MiB baseline:
 * about 124 MiB usable, about 40 MiB in use with desktop, audio and network
 * running (T23 serial of e831e685: 10121 pages of zone 2 in use), and the
 * browser's non-heap share of its 32 MiB target (docs/design/
 * netsurf-port-plan.md: 6 MiB image, 12 MiB surfaces, 1.5 MiB stacks, about
 * 3 MiB fonts and caches): about 61 MiB, rounded down to 56 MiB. CiukiOS
 * queries the kernel instead (docmem_query.h). */
#define CIUKI_DOC_MODEL_FREE_BYTES (56u*1024u*1024u)
#define CIUKI_DOC_FREE_SHARE 4u
/* Charged per DOM node or attribute in addition to its string bytes: libdom
 * node, element/attribute record, interned name, hubbub stack entry and
 * allocator headers; measured on the x86-64 host build (larger pointers than
 * i686, so conservative for CiukiOS). */
#define CIUKI_DOM_NODE_BYTES 352u
/* Charged once per document: the empty HTML document's own records and
 * memoised strings (measured 54.5 KB live on the host). */
#define CIUKI_DOM_DOCUMENT_BYTES (64u*1024u)
/* Charged per box in addition to its text: struct box on i686 (272 bytes,
 * measured with ciuki-cc), its 48-byte talloc header and the allocator's
 * header, rounded up. Computed styles are shared and belong to the other
 * half of free memory. */
#define CIUKI_BOX_BYTES 352u
/* Nesting: every recursive DOM/box walk stays far inside the 1 MiB main
 * stack (CIUKI_MAIN_STACK_BASE..LIMIT). Boxes add anonymous inline
 * containers and implied table parts, hence twice the DOM depth. */
#define CIUKI_DOM_DEPTH 128u
#define CIUKI_BOX_DEPTH 256u
/* HTML's colspan maximum (HTML Living Standard, 4.9.11: 1000). */
#define CIUKI_TABLE_COLUMNS 1000u
enum ciuki_doc_reason { CIUKI_DOC_ADMITTED=0, CIUKI_DOC_MEMORY=1, CIUKI_DOC_DEPTH=2, CIUKI_DOC_COLUMNS=3 };
/* Queries free memory and caches the per-ledger budget derived from it. */
size_t ciuki_doc_budget_refresh(void);
/* The cached budget (refreshed on first use). */
size_t ciuki_doc_budget(void);
/* Boxes the cached budget admits at CIUKI_BOX_BYTES each. */
size_t ciuki_doc_box_limit(void);
/* The page text for a rejection reason; NULL for CIUKI_DOC_ADMITTED. */
const char *ciuki_doc_message(int reason);
/* Logs one "NetSurf structure rejected:" line for a reason. */
void ciuki_doc_report(int reason,size_t charged,size_t budget);
#endif
