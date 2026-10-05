# CiukWeb CSS style worker

## Decision, 2026-10-04

Keep style parsing and selector matching in a separate 16-bit CAPP worker so
the network browser stays within its conventional-memory segment budget. The
worker accepts stylesheet data in bounded chunks, retains at most 16 KiB and
128 rules, and tracks at most 64 open elements. Its initial CSS 2.1 subset
handles type, class and ID selectors, descendant and child relationships,
source order, specificity, `!important`, basic inheritance, and a documented
set of color and background colors, bold/normal and italic/normal fonts,
inline/block/none display, pixel or zero margins/padding/width/height,
text alignment, and uniform border width/color/style. Values are limited to
named colors or `#RGB`/`#RRGGBB`; sizes are integer `px` or zero. Shorthand
`margin`, `padding`, and simple uniform `border` are accepted. `inherit` is
supported for the listed properties. Unsupported selectors and property
values do not match or alter computed styles; fixed-capacity overflow is
reported to the caller. Limits are 16 KiB total stylesheet input, 128 rules,
79 selector characters per rule, 127 declaration characters per rule, and 64
nested elements. The worker does not implement attribute/pseudo/sibling
selectors, percentages, font-size, background images, border side variations,
floats, positioning, media queries, or layout.

CSS 2.1 defines selector grouping, type/class/ID matching, descendant and
child relationships in [Selectors](https://www.w3.org/TR/CSS21/selector.html).
Its cascade orders applicable declarations by importance, specificity and
source order, and computes inherited values from the parent in
[Cascade and Inheritance](https://www.w3.org/TR/CSS21/cascade.html). The
[box model](https://www.w3.org/TR/CSS21/box.html) and
[visual formatting model](https://www.w3.org/TR/CSS21/visuren.html) define
the spacing, dimensions, alignment and display properties that this worker
passes to layout. The implementation deliberately bounds stylesheet size and
tree depth for the 16-bit CAPP ABI; it does not claim full CSS 2.1 or browser
compatibility.

Browser line layout measures each text run with the same normal/bold glyph
advances used by the desktop painter. The CAPP measurement call now initializes
its font flags and accepts the run's style explicitly; measuring every bold
word with the normal font made adjacent words overlap their spaces. This
implements the text-run geometry required by the CSS 2.1 inline formatting
model above. Parsing a property does not imply complete layout support: the
native renderer applies its supported color, spacing, display and font-weight
subset; a true italic face and the remaining CSS layout properties are still
limited by the desktop font/rendering API.
