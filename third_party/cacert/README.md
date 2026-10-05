# Mozilla CA root bundle snapshot

`CACERT.PEM` is the 2026-09-25 snapshot distributed by the curl project at
<https://curl.se/ca/cacert.pem>. Curl converts Mozilla NSS `certdata.txt` into
the PEM bundle; the upstream header is preserved unchanged.

- SHA-256: `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`
- Bundle source notes: <https://curl.se/docs/caextract.html>
- Mozilla source: <https://raw.githubusercontent.com/mozilla-firefox/firefox/refs/heads/release/security/nss/lib/ckfw/builtins/certdata.txt>
- Mozilla Public License 2.0: see the complete upstream text in `MPL-2.0.txt`, also available at <https://www.mozilla.org/MPL/2.0/>.

This is a pinned trust-store snapshot, not a permanent list. Refresh it only
from the upstream bundle, update the hash/date here, and run the TLS trust
tests. The certificates are trust anchors; changing this file changes which
HTTPS servers CiukiOS accepts.
