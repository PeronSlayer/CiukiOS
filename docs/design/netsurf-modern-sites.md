# NetSurf modern site compatibility

Scope: `docs/directives/fx-nsmodern.md`. This is a partial measurement and
implementation record; it does not qualify either target site as working.

## Research and evidence limits (round 1)

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

## Implementation decision (round 1)

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

## Review round 2: amendment implementation contract

The lead supplied hashed home-page snapshots under its read-only
`build/downloads/ns-sites-2026-10-26/`. Inspection finds 579 `var()` calls
and 44 custom declarations in ALcyberCloud CSS, 178 calls and 30 declarations
in EDuke32Logs CSS. The supplied ALcyberCloud stylesheet has no `@layer`;
its recorded Tailwind/Vite label does not imply a particular cascade model.
Neither original snapshot nor full stylesheet is added to tracked fixtures.

Implement token-based custom properties in libcss: preserve declaration order,
collect matched custom declarations before resolving ordinary values, retain
case-sensitive names, inherit computed token values, process fallback dependencies
for cycles, and apply invalid-at-computed-value declarations as `unset` rather
than revealing a lower-priority declaration. The normative reference is
[CSS Variables 1](https://www.w3.org/TR/css-variables-1/), especially sections
2.3, 3 and 3.1. Unknown `@property` rules remain ignored, including their bodies;
this implements unregistered inherited properties, not registration descriptors.
Keep all data under existing document admission, add explicit token/variable/work
bounds, and return allocation errors through existing paths. Preserve the fast
selection path for stylesheets without custom declarations or `var()`.

[CSS Flexbox 1](https://www.w3.org/TR/css-flexbox-1/) and
[CSS Grid 1](https://www.w3.org/TR/css-grid-1/) distinguish flex layout from
track sizing and grid placement. Measure site-derived fixtures before changing
layout; grid's existing block fallback is acceptable under amendment 1 if its
readability is verified and its limits are reported. For color mixing follow
[CSS Color 5](https://www.w3.org/TR/css-color-5/#color-mix), including percentage
normalization and premultiplied alpha; only measured color spaces are candidates.
[HTML script processing](https://html.spec.whatwg.org/multipage/scripting.html#attr-script-type)
distinguishes module scripts from classic scripts. [Duktape](https://duktape.org/)
is ES5.1; measure ignored-module behavior and the APIs used by the supplied
bundle before proposing a different engine. Do not run modules as classic JS.

### Implemented behavior and limits

Patch `0020-css-custom-properties.patch` extends the pinned libcss parser and
selector with retained tokens, matched-style replay and reference-counted
per-element environments. It keeps declaration order and the existing origin /
importance / specificity comparison; inline styles use author origin and inline
specificity. Ancestors contribute already substituted custom tokens, including
across elements whose styles contain no custom declarations. Pseudo-elements
inherit their originating element's variables and may override them.

Fallback dependencies participate in cycle detection even when unused. An empty
custom value remains valid and suppresses its fallback. Missing/cyclic variables
and invalid substituted property values produce `unset`, including every
longhand of a shorthand; they do not reveal an older declaration. Names remain
case sensitive. [Variables 1 section 2.1](https://www.w3.org/TR/css-variables-1/#syntax)
also requires URL resolution in the **consuming** stylesheet, which is tested
across two distinct stylesheet bases. Custom values are not pre-resolved as URLs.

The pinned lexer needed two prerequisite corrections: two leading hyphens now
form an identifier while `-->` remains a CDC token; and `url(...)` split after
its initial `u` resumes the correct lexer state. Empty custom declarations are
accepted. Unknown at-rule recovery retains its trailing-whitespace state across
input chunks, preventing an ignored `@property` block from consuming the next
rule. These decisions follow [CSS Syntax 3](https://www.w3.org/TR/css-syntax-3/).
Registration syntax and descriptors remain unsupported and ignored.

Limits are 1,024 tokens per value/expansion, 65,536 retained tokens and 8,192
deferred declarations per sheet, 128 locally declared variable names per element,
64 dependency/substitution levels, 4,096 matched styles and 262,144 work units per
selection. Allocation / admission failures use `CSS_NOMEM`; excessive expansion
invalidates the value. [Variables 1 section 3.3](https://www.w3.org/TR/css-variables-1/#long-variables)
requires a bound against exponential expansion. Document, JavaScript and image
admission limits are unchanged. Environments without overrides share their
computed parent environment; ordinary computed styles still use the arena.
Cousin/sibling style selection sharing is bypassed when variables or gaps are
active, because identical ordinary styles can inherit different custom values.
Stylesheets without either feature retain the existing selection path.

Flex fixes cover the measured three-part shorthands (`flex:0 0 auto`,
`flex:1 1 0`, and the derived fixed-basis regression), plus one/two-value `gap`,
`row-gap` and the existing `column-gap`. Gutters participate in line wrapping,
free-space allocation, placement, reversed directions and cross-line spacing.
A private row-gap record participates in computed-style hashing, equality,
cloning and composition; the public Ciuki SDK ABI is unchanged. Relative row
lengths compute before inheritance. Normal flex gaps are zero; percentages
against indefinite flex axes are zero. Aggregate gutter sizes are capped at
32,767 device pixels for fixed-point arithmetic. References:
[Flexbox 1](https://www.w3.org/TR/css-flexbox-1/) and
[Box Alignment 3 gutters](https://www.w3.org/TR/css-align-3/#gaps).
This is a targeted flex extension, not complete Flexbox/Box Alignment conformance.
Math functions in gaps, general justification and all automatic minimum-size
behavior remain outside this implementation.

Grid retains the existing `display:grid` to block and `inline-grid` to inline-block
fallback in NetSurf's `box_construct.c`. Tracks (`repeat`, `fr`, `minmax`), spans,
placement and grid gutters are ignored. A small derived page checks source-order
text and pixel-equivalence to an explicit block version, and saved-page goldens
record the actual fallback. This is the fallback expressly permitted by amendment
1, not grid layout support. Calendars and multi-column cards lose their authored
track arrangement. [Grid 1](https://www.w3.org/TR/css-grid-1/) defines the missing
track-sizing and placement algorithms.

Patch `0021-css-srgb-mix.patch` implements the measured two-color `in srgb`
`color-mix()` forms with omitted/complementary/normalized percentages,
premultiplied alpha, transparent colors and bounded nesting. Unsupported color
spaces and `currentColor` operands are rejected. The current
[Color 5 algorithm](https://www.w3.org/TR/css-color-5/#color-mix) specifies a
transparent result for a zero total weight; tests cover that behavior as well.
Color 4 math, `clamp`/`calc`, transforms, sticky positioning, downloaded fonts and
complete modern CSS remain missing.

Patch `0022-ignore-module-scripts.patch` chooses the ES5-browser behavior allowed
by the amendment: ignore inline/external module scripts before context creation
or fetching, including ASCII-case variants, and keep classic `nomodule` scripts.
The baseline host probe executed an external module's side effect as classic JS;
the corrected probe does not. Module/classic distinction follows
[HTML script processing](https://html.spec.whatwg.org/multipage/scripting.html#attr-script-type).
The supplied bundle has 61 arrow tokens, optional chaining, nullish operators,
async/await and numerous browser APIs. Ignoring it loses mobile-menu toggling,
consent/preferences, RSS clipboard controls, video embedding and carousel controls.
Its initial classic inline script adds a `js` class; the supplied CSS then hides
mobile navigation until the module toggles a data attribute. Thus offline
script-free replay is not evidence that the interactive mobile menu works.
Transpiling syntax alone cannot supply matchMedia, selector queries, storage,
clipboard, ResizeObserver or geometry/scroll bindings. An engine migration also
requires those bindings and preservation of heap/stack/time admission;
[Duktape features](https://duktape.org/) and
[QuickJS's API](https://bellard.org/quickjs/quickjs.html) document the distinction.
No transpiler, engine upgrade, network proxy or site rewrite was added.

Validation is recorded in `build/codex/fx-nsmodern-report.md`, Review round 2.
Full snapshots remain untracked; tests contain only small derived fixtures and
hashes. Site replay disables scripts and replaces missing media references,
uses the supplied CSS, checks both 800/1280-pixel viewports and makes no live
requests. These goldens are regressions of this fallback renderer, not modern
browser reference images. Final live-site, image and hardware evidence belongs
to the lead. This round does not qualify either site as fully supported.

The fixed flex parser intentionally disagrees with pinned libcss selection case
65, which expects `flex:0 0` to leave `flex-shrink:1`. The
[Flexbox shorthand grammar](https://www.w3.org/TR/css-flexbox-1/#flex-property)
requires that second number to be shrink zero. The host harness applies the
single explicit hunk in `fixtures/flex-shorthand-oracle.patch` to a disposable
copy of `tests1.dat`; the pinned archive and extracted original remain intact.
The other pinned expectations and all earlier framebuffer golden hashes remain
unchanged. New direct longhand-equivalence cases cover both zero shorthands.


## Review round 3: amendment 2 repair contract

The [CSS Variables cycle rules](https://www.w3.org/TR/css-variables-1/#cycles)
require all members of overlapping dependency cycles to be invalid, including
edges in unused fallbacks. Use bounded Tarjan strongly connected components;
retain the existing entry, recursion, expansion and operation bounds.
[Color 4 missing-component interpolation](https://www.w3.org/TR/css-color-4/#interpolation-missing)
requires carrying missing RGB/alpha flags through nested colors, filling from
the other operand before premultiplication, and retaining components missing
from both operands. The [gap shorthand grammar](https://www.w3.org/TR/css-gaps-1/#gap-shorthand)
permits independent `normal`/length values on either axis.

The raw and computed custom-property representations need byte admission in
addition to per-selection counts. Use one shared pool for all documents and
cached/imported sheets: this is stricter than separate per-document budgets,
prevents sharing or document recreation from resetting admission, and counts
shared immutable values only once. Its limit is one eighth of the same free
memory query used by the existing DOM/box admission (7 MiB in the existing
56 MiB host model). No DOM, box, image or JS limit increases. Reserve before
allocation/growth and retained-string references; charge vector capacity and
conservative allocator/string overhead, declaration arrays, environments and
match arrays. Roll back failed allocations and release on final reference.
Literal computed custom values share their immutable raw token vectors;
inheritance continues sharing computed values. Exhaustion returns CSS_NOMEM
through existing error paths. This new pool comes from the half of free memory
already reserved for styles, caches and work; it is not a measured total-heap
cap and does not replace the existing ledgers.

Cache deferred-feature usage in each stylesheet for the complete import graph.
Invalidate the local cache on rule/custom-declaration changes and destruction;
advance a shared generation for mutations of imported sheets and import
registration. Unrelated inline sheets leave the graph caches intact. Selection on an unchanged ordinary sheet reads
the cache without visiting its rules; a generation change recomputes each
reachable sheet once. Depth remains bounded, conservatively enabling custom
selection on excessive/cyclic imports. This does not change upstream import
ownership or claim general cyclic-import selection support.

The admission-failure regression exposed pending lexer-token references in the
pinned parser destructor (also present in the inspected
[upstream parser](https://raw.githubusercontent.com/netsurf-browser/libcss/master/src/parse/parse.c)).
Destroying a parser after CSS_NOMEM now discards its pending vector and releases
the separately owned pushback token before freeing either storage. The raw-sheet
exhaustion test verifies final custom charges and the interned-string dictionary.
