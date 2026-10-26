# NetSurf modern site compatibility

Scope: `docs/directives/fx-nsmodern.md`. This is a partial measurement and
implementation record; it does not qualify either target site as working.

## Research and evidence limits

The [ALcyberCloud home page](https://www.alcybercloud.it/) is available to
the web research tool as rendered text, redirected to `/it`. Its navigation
and main content exist in that response. This does not expose its stylesheets,
script source or HTTP handshake. The inner
[projects page](https://www.alcybercloud.it/it/progetti) is also readable as
rendered text. The services page and both forms of the EDuke32Logs hostname
could not be retrieved by that tool. Direct `curl`
requests from the implementation sandbox fail DNS resolution for both target
hosts. Consequently CSS/JavaScript/framework/image inventories and server TLS
requirements are **unmeasured**, rather than inferred from page appearance.
Raw saved pages and their dependent resources are required for site replay.
The implementer requested local snapshot/source paths while continuing the
port inspection. See the worktree report for exact commands and outcomes.

## Port inspection and primary sources

Paths beginning `netsurf/` or `libcss/` below are relative to the verified
`build/external/netsurf-all-3.11/` archive extraction; other paths are in
the repository. Generated host copies under `netsurf-host/source/` include
the repository patches.

| Capability | Evidence in this tree | Consequence |
| --- | --- | --- |
| Flex layout | `netsurf/content/handlers/html/layout_flex.c:1048`; `libcss/include/libcss/properties.h:134` | Already present in 3.11; test the actual site before choosing fixes. |
| Grid | `netsurf/content/handlers/html/box_construct.c:111` maps grid to a block | Parsing the display keyword does not provide grid layout. |
| Custom properties | `libcss/src/parse/language.c:1845` (`parseProperty`) dispatches only known properties | Requires cascade-time, element-scoped token storage/substitution; a global text replacement would be incorrect. |
| CSS math | `libcss/src/parse/properties/utils.c:915` (`css__parse_unit_specifier`) | Numeric/unit parser, no `calc()` evaluation. |
| Media queries | `libcss/src/parse/language.c:544`, `libcss/src/parse/mq.c` | Parser exists; viewport/feature coverage needs site replay. |
| Fonts | `libcss/src/parse/language.c:832`; `sdk/netsurf/port/font.c:130` | `@font-face` descriptors parse, but the font adapter selects bounded system faces, not downloaded fonts. |
| Sticky/transforms | `libcss/include/libcss/properties.h:774`; property list at `:14` | Sticky enum alone does not implement sticky scrolling; no transform property pipeline. |
| JS language | `netsurf/content/handlers/javascript/duktape/duktape.h:185` | Duktape 2.7.0, ES5.1 with selected later features, not general ES2015+ or modules. |
| DOM bindings | `netsurf/content/handlers/javascript/duktape/Element.bnd:395`, `EventTarget.bnd:141` | `classList` and event listeners have implementations; IDL declarations alone are not API implementations. Selector queries, CSSOM and asynchronous web APIs need separate work. |
| libdom | `libdom/include/dom/core/element.h:27`, `netsurf/content/handlers/javascript/duktape/Document.bnd:279` | Attribute/tag/tree APIs and ID lookup exist; no general selector-query engine in libdom's element interface. New JS methods still need DOM-backed behavior and lifetime handling. |
| TLS | `sdk/netsurf/port/tls.c:100` | TLS 1.2 only; SNI/hostname passed to BearSSL reset. ECDHE RSA/ECDSA with ChaCha20-Poly1305 or AES-128-GCM; no HTTP/2 client. Actual target-server overlap is unmeasured. |
| Images | `sdk/netsurf/build.py:29` | PNG/JPEG/GIF/BMP/WebP/SVG enabled, AVIF absent. Decoder/resource limits remain binding. |

[Upstream NetSurf flex layout](https://raw.githubusercontent.com/netsurf-browser/netsurf/master/content/handlers/html/layout_flex.c)
still uses the same staged flex layout model. The release already contains
this implementation, so updating it must be evaluated as individual fixes.
[NetSurf history](https://github.com/netsurf-browser/netsurf/commits/master/)
and [libcss history](https://github.com/netsurf-browser/libcss/commits/master/)
also contain later maintenance (including relative-position fixes).
[Current libcss declaration parsing](https://raw.githubusercontent.com/netsurf-browser/libcss/master/src/parse/language.c)
still dispatches known properties. A general custom-property backport cannot
be assumed to exist in upstream master.

[CSS Variables](https://www.w3.org/TR/css-variables-1/) requires inheritance,
case-sensitive names, fallback, cycle handling and substitution after cascade.
Retaining an approximate `:root` string map in the fetcher would lose those
semantics, especially across media queries and element overrides.

[Duktape's feature description](https://duktape.org/) confirms the language
limit; simply upgrading its version does not promise modern syntax.
[QuickJS's manual](https://bellard.org/quickjs/quickjs.html) documents a modern
language engine, allocator/memory and stack limits, interrupts and job execution.
It supplies no browser DOM. Adoption requires a new binding backend and
integration with navigation lifetimes, scheduling and the current admission
policy, including i686/x87 value tests. The published small-program footprint
is not a browser memory measurement.

[BearSSL's supported crypto](https://bearssl.org/support.html) confirms the
TLS version boundary. A server offering TLS 1.3 need not require it. Measure
TLS 1.2 overlap with certificate verification and SNI before replacing TLS;
there is no evidence here justifying a security or verification relaxation.

## Implementation decision

[CSS Color 4](https://www.w3.org/TR/css-color-4/#rgb-functions) defines modern
space-separated RGB, optional slash alpha and interchangeable `rgb`/`rgba`
names, as well as alpha hex notation. The pinned parser rejects these forms
and also scales legacy numeric alpha incorrectly when RGB channels use
percentages. Implement this bounded parser improvement first: no extra
retained data, new decoder, JS engine or admission change is needed. Preserve
legacy comma syntax, reject mixed separators, validate all input, clamp before
fixed-point scaling and preserve the existing quirks treatment of bare hex.
Modern syntax rounds to the nearest byte; legacy comma syntax retains its
existing truncation (including alpha `.5` becoming 127), preserving the
pinned upstream `test/data/parse/colours.dat` expectations. Numeric alpha with
percentage RGB is scaled independently of RGB's unit; this fixes that parser
bug without changing existing ordinary comma-color expectations.
`none` components resolve to zero in this non-interpolating color pipeline.
Color interpolation, relative colors, other Color 4 functions and CSS math
remain outside this change. Site-specific benefit remains unmeasured until
the resources are available.

## Ordered next work (estimates, not commitments)

Costs below are implementer sessions including host tests; they assume the
site snapshots become available. Order must be revisited against their
actual feature counts. No memory limit should be raised on this evidence.

| Order | Work | Cost | Expected visible benefit and Pentium III tradeoff |
| --- | --- | --- | --- |
| 1 | Acquire four pages plus CSS/JS/image/font dependencies, hash them, replay at two viewport widths; measure TLS 1.2 | Less than 1 | Establish which work actually exposes content on each site; no runtime cost. |
| 2 | Modern RGB/alpha hex parser (this change) | 1 | Restores authored colors where used; bounded token parsing, no retained memory. |
| 3 | Targeted upstream flex/media/relative-position fixes | 1–3 | Potentially fixes navigation/card alignment on both sites if their CSS uses supported layout; avoid an unmeasured whole-engine update. |
| 4 | Correct CSS custom properties and deferred math | 4–8+ | Likely high for variable-based themes on either site, but not measured. Requires token budgets, cycle/work bounds, cascade/inheritance and invalid-at-computed-value tests. |
| 5 | Only the missing DOM APIs actually used, including selector queries/CSSOM as needed | 2–6+ | Enables menus, consent controls and progressive interaction where ES5 scripts can execute; bounded traversal and DOM lifetime tests are needed. |
| 6 | QuickJS feasibility/backend, versus controlled offline transpilation | 5–10+ | Modern syntax alone cannot enable either site. QuickJS is preferred for general browsing only after footprint, interrupts, job queue and DOM costs are measured; no JIT needed. Offline Babel-style transforms can help a controlled site but expand scripts, still need APIs/polyfills, and do not justify a runtime proxy or changes to site payloads. Later Duktape is not a substitute for full ES2015+ support. |
| 7 | Downloaded fonts, WOFF/WOFF2 and per-origin face selection | 3–5+ | Improves typography/icons only if those assets are used. Preserve fallback text, cap decoded faces and glyph caches; measure decompression/FreeType cost before adding dependencies. |
| 8 | Grid, transforms and sticky layout | 5–10+ | Potentially major layout improvement, dependent on the actual site. Grid is not just a display enum; requires new layout, overflow/hit-testing and bounded work. |
| 9 | TLS 1.3 or additional decoders | 3–8+ each | Only if measured transport/format requirements block these sites. HTTP/2 is optional when HTTP/1.1 is offered; no reason to force it merely because a server supports it. |

Acceptance is host parser tests, the existing framebuffer goldens and memory
admission tests, followed by snapshot replay when available. Hardware and
canonical image validation belong to the lead. This record does not authorize
an engine replacement, a site rewrite, a proxy or a memory-budget increase.
