# f6-browser fixture certificates (test only)

Pinned material for the `f6-net` browser HTTPS cases (directive f6-18). The
host fixture server (`scripts/test/https_fixture.py`) serves these through the
QEMU user network (`guestfwd` to 10.0.2.100:80/443); the kernel test profile
resolves `fixture.test` and `untrusted.test` to that address only under the
`f6:browser` selector, and the browser trusts `ca.pem` only in that profile.

| File | Role |
| --- | --- |
| `ca.pem` | fixture CA (EC P-256, SHA-256, 2026-01-01 to 2046-01-01); converted at browser build time into the test-profile trust anchor (`test_anchors.h`) |
| `fixture.pem`, `fixture.key` | `fixture.test` server certificate signed by the CA (SAN `DNS:fixture.test`, serverAuth) |
| `untrusted.pem`, `untrusted.key` | self-signed `untrusted.test` certificate, served for any other SNI name: the certificate-refusal case |

The CA private key was discarded after signing; the leaf keys are fixture
secrets with no value outside this test. Regenerate the whole set (never a
single file) with OpenSSL 3:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -sha256 \
  -keyout ca.key -out ca.pem -subj "/CN=CiukiOS f6-net fixture CA (test only)" \
  -not_before 20260101000000Z -not_after 20460101000000Z \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
  -keyout fixture.key -out fixture.csr -subj "/CN=fixture.test"
printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:fixture.test\n' > leaf.ext
openssl x509 -req -in fixture.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -extfile leaf.ext \
  -not_before 20260101000000Z -not_after 20460101000000Z -out fixture.pem
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -sha256 \
  -keyout untrusted.key -out untrusted.pem -subj "/CN=untrusted.test" \
  -not_before 20260101000000Z -not_after 20460101000000Z \
  -addext "subjectAltName=DNS:untrusted.test" -addext "basicConstraints=critical,CA:FALSE" -addext "extendedKeyUsage=serverAuth"
rm ca.key ca.srl fixture.csr leaf.ext
```

A regenerated `ca.pem` changes the browser's test anchor header and therefore
the canonical image: rebuild before running the suite.
