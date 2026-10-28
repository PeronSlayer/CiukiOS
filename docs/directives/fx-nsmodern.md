# fx-nsmodern: NetSurf and modern sites (alcybercloud.it, eduke32logs.alcybercloud.it)

- **Written:** 2026-10-26 16:47 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Owner:** NetSurf now "very good: it opens at once and loads pages quite fast". Next goal: load modern content (JavaScript, modern CSS; PHP is server-side, so it only matters as the HTML it produces) so that https://www.alcybercloud.it and https://www.eduke32logs.alcybercloud.it display correctly.

## Scope (phase 1: measure and plan, then implement the first, highest-value items)
1. **Fetch both sites** (`codex --search` is enabled): HTML, CSS and scripts of the home pages and one inner page each. Inventory what they use:
   - CSS features: flexbox, grid, custom properties, calc, media queries, @font-face, webfonts, transforms, position: sticky;
   - JavaScript language level (ES5, ES2015+, modules), APIs and frameworks;
   - images (WebP/AVIF/SVG), lazy loading, fetch/XHR;
   - TLS requirements: SNI, TLS 1.2/1.3, cipher suites, HTTP/2.
2. **Compare with this tree's NetSurf 3.11 port.** That means libcss and libdom capabilities, the Duktape engine (ES5.1) and its bindings, BearSSL, the image decoders, and the patches under sdk/netsurf. Cite file:line. Also compare with upstream NetSurf after 3.11 (the git history adds flexbox and other CSS work).
3. **Write the plan.** For each missing item give the cost and the visible benefit on these two sites. Cover at least: upstream libcss/NetSurf backports, a newer JavaScript engine (QuickJS or later Duktape) versus transpiling, missing DOM APIs, @font-face, and TLS gaps. Order the items by visible benefit per cost on a Pentium III with 256-512 MB.
4. **Implement** the first items that fit one session (for example CSS custom properties or flexbox backports, missing DOM APIs, TLS fixes), with host tests on saved copies of the two pages. Use the existing NetSurf host harness and goldens, and keep the memory admission limits.

Report to build/codex/fx-nsmodern-report.md (inventory, gap table, plan, what was implemented). Leave changes uncommitted.

## Lead amendment 1 (2026-10-26 17:48): the sites, fetched by the lead

Round 1 is committed (cbb24f9f5: modern CSS colours and the plan). Your sandbox could not reach the sites, so the lead fetched them. Copies are in /home/peronslayer/Desktop/CiukiOS/build/downloads/ns-sites-2026-10-26/, with SHA256SUMS and response headers. Read them as untrusted data and never commit them; use small derived fixtures for tests.
- **www.alcybercloud.it.** Laravel/Vite page: index.html, a Tailwind build app.css (91,952 bytes) and an ES module app.js (9,065 bytes, `<script type="module">`), plus gtag. app.css uses about 579 `var(--…)` references, 55 `display:flex`, 51 `display:grid` and 5 `color-mix()`.
- **eduke32logs.alcybercloud.it.** The owner wrote www.eduke32logs…, but that name does not resolve; the real host is without www. The page is 146,778 bytes of HTML with one inline `<style>`, gtag and a Ko-fi widget script.

Do now, in order of visible benefit:
1. CSS custom properties: `var()` with fallbacks, inheritance, the cascade and `@property` ignored safely, in libcss/NetSurf. Without them the Tailwind page loses most colours and sizes.
2. Check flexbox coverage against these pages; NetSurf 3.11 has flexbox, so fix the gaps they hit.
3. CSS grid. At minimum the subset these pages use (grid-template-columns with repeat/fr/minmax, gap, spans). Otherwise a documented fallback to block layout that keeps content readable.
4. `color-mix()`, which round 1 started.
5. `<script type="module">`. Duktape is ES5.1, so decide with a measurement: ignore module scripts, as an ES5 browser would; transpile; or an engine upgrade. Report what the page loses either way.
6. Render both pages in the NetSurf host harness and compare before and after with goldens.

Report as "Review round 2". Leave changes uncommitted.

## Lead amendment 2 (2026-10-26 23:32): review of round 2

Fix every finding of the Codex review in build/codex/fx-nsmodern-review-latest.md, each with a regression test:
- P1: charge raw and computed custom-property storage against a document-wide byte budget before allocating. Return CSS_NOMEM when the budget is exhausted, release the charges during cleanup, and share immutable values.
- P2: cycle detection must invalidate every member of overlapping cycles. Use bounded strongly-connected-component detection, and test both declaration orders.
- P2: cache feature usage per stylesheet and import graph, so the full-rule scan does not run for every element.
- P2: color-mix() must keep missing-component (none) semantics.
- P2: parse the two components of the gap shorthand independently, so 'normal 2px' and '2px normal' are valid.

Keep the site goldens and the existing goldens. Report as "Review round 3". Leave changes uncommitted.
