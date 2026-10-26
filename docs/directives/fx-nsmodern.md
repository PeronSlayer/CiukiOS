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
