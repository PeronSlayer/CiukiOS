# CiukWeb large-response handling

## Finding

The browser's 16 KiB response buffer was being treated as a hard maximum for
the complete HTTP wire response. `webnet.c` failed the request as soon as an
in-order TCP segment exceeded the remaining capacity. This surfaced as a
memory-limit error even when much of the response consisted of markup that
CiukWeb's text renderer does not display.

A bounded host request on 4 October 2026 observed:

- `http://google.com/` responds `301` with `Location: http://www.google.com/`
  and a 219-byte body.
- Following that redirect responds `200` with chunked transfer encoding and
  about 84 KiB read (two bounded requests measured 84,294 and 84,369 bytes).
  The HTML `<body>` begins at byte 62,066; the first form
  and input begin at bytes 64,425 and 64,569. The early response is dominated
  by inline JavaScript, so rendering only the first raw 16 KiB would yield
  little or no useful page content.

The root integration runtime later followed this redirect and rendered the
Google response while receiving 88,229 body bytes.

This follows HTTP's separation between representation length and transfer
framing: `Content-Length` can delimit a response, while `Transfer-Encoding`
can carry chunked content without that length. A redirect is a 3xx response
with a `Location` target. See [RFC 9110 sections 8.6](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.6),
[6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.1) and
[10.2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.2.2).

Relative redirect paths may also contain `.` and `..` path segments. RFC 3986
section 5.2.4 defines their removal during reference resolution
([primary specification](https://www.rfc-editor.org/rfc/rfc3986.html#section-5.2.4)).
CiukWeb currently rejects redirect targets containing literal dot segments
with a clear message rather than issue a request to a path it has not
normalized. Its normal relative-path handling ignores slashes in the query
when choosing the containing directory.

## Implementation decision

Keep CiukWeb's fixed 16 KiB memory ceiling and cap decoded response bodies at
1 MiB with a 60-second total-fetch deadline. These bounds prevent a peer that
continues streaming from retaining the network claim indefinitely or consuming
unbounded processing time. Decode HTTP chunk framing
incrementally across arbitrary TCP segment boundaries, then filter script,
style and comment blocks while retaining other HTML bytes in the existing
response buffer. Track decoded body bytes separately from retained, filtered
bytes, and compare decoded bytes with `Content-Length` or the terminal chunk
before accepting a close. Continue acknowledging and consuming the response
after the retained document reaches capacity; render the usable prefix with
an explicit partial-page indication instead of reporting a network-memory
failure. This does not promise a complete representation when the visible
document itself exceeds the buffer.

Follow a bounded number of HTTP redirects with an HTTP `Location` target.
HTTPS redirects remain unsupported and are reported explicitly. No heap or
larger response allocation is added.

## Regression cases for the bounded host harness

- Split a chunk-size line, chunk payload and trailing CRLF at every possible
  TCP segment boundary; verify the exact decoded body.
- Split opening/closing script and style tags across segments, including
  quoted attributes and mixed-case closing tags; verify their contents are
  omitted while surrounding visible markup is preserved.
- Feed a response with more than 16 KiB of script before visible body text;
  verify the visible text is retained and no memory-limit failure is reported.
- Feed a response whose filtered visible content exceeds 16 KiB; verify a
  bounded prefix is returned, later bytes are acknowledged/discarded, and
  the UI labels the page partial.
- Verify `google.com`'s HTTP redirect target is followed, redirect loops are
  bounded, and an HTTPS `Location` reports unsupported transport.
- Verify the exact 1 MiB decoded-body boundary, rejection of a larger declared
  `Content-Length`, and the 60-second tick deadline across 16-bit tick wrap.
- Verify relative redirect paths containing literal `.` or `..` segments are
  rejected explicitly, while `/` characters in the source query do not alter
  directory resolution.

## Executed validation

The production decoder host harness in `scripts/tests/webnet_http_decoder.c`
passed 35 assertions with AddressSanitizer and UndefinedBehaviorSanitizer:
bytewise chunk/header splits, a 65,536-byte script preceding the visible body,
script/style/comment removal, visible overflow, redirect extraction, malformed
chunk sizes, the header bound and declared-length completion. Browser redirect
resolution itself is outside this decoder harness; the real Google redirect
was checked in the guest.

In the integrated Linux QEMU run, `http://google.com/` followed its HTTP redirect
and rendered a 200 response from `http://www.google.com/`: 88,229 decoded source
bytes, 2,626 retained bytes. The page displayed useful links and a partial-page
notice: oversized tags/attributes may also be omitted by the 128-byte tag bound.
This removes the former fatal 16 KiB response error; it does not implement Google
forms, JavaScript, CSS or HTTPS. See the [actual capture](2026-10-04-post-release-fixes/google-http.png).

The OpenWatcom build uses 63,728 bytes for the complete browser CAPP, under its
65,536-byte segment ceiling. The document allocation remains 16 KiB. No heap or
VM memory limit increase was needed. The production-decoder host harness
is `scripts/tests/webnet_http_decoder.c`; its compile recipe is in the source.
The final checks also cover the 1 MiB boundary, oversized declared length,
timer wrap at the total deadline and literal redirect dot-segment rejection.

A focused run of the final browser binary, after the transfer bounds were
added, again passed real Google HTTP 301→200 and returned to the desktop:
88,305 source bytes and 2,626 retained bytes.
