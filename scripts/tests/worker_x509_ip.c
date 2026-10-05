/* Bounded DER parser regressions. These fixtures exercise SAN extraction only;
 * BearSSL remains responsible for all certificate-chain validation. */
#include <stdio.h>
#include <string.h>
#include "../../src/web/worker_x509_ip.h"

static unsigned checks, failures;
#define CHECK(ok, text) do { ++checks; if (!(ok)) { ++failures; fprintf(stderr,"FAIL: %s\n",text); } } while (0)

static size_t put_tlv(unsigned char *out, unsigned tag,
    const unsigned char *data, size_t length)
{
    size_t n = 0, count, i, value = length;
    out[n++] = (unsigned char)tag;
    if (length < 128) {
        out[n++] = (unsigned char)length;
    } else {
        count = 0;
        while (value) { ++count; value >>= 8; }
        out[n++] = (unsigned char)(0x80u | count);
        for (i = count; i != 0; --i) out[n++] = (unsigned char)(length >> ((i - 1) * 8));
    }
    if (length) memcpy(out + n, data, length);
    return n + length;
}

static size_t make_certificate(unsigned char *out, int include_san,
    unsigned san_tag, const unsigned char *san_data, size_t san_length)
{
    static const unsigned char oid[] = {0x55,0x1D,0x11};
    static const unsigned char serial[] = {1};
    unsigned char scratch[512], names[128], octets[192];
    unsigned char extension[240], extension_seq[256], explicit_extensions[280];
    unsigned char fields[420], tbs[460], signature[8], outer_body[500];
    size_t n=0, names_len=0, san_len=0, ext_len=0;
    size_t ext_seq_len=0, explicit_len=0, fields_len=0, tbs_len=0;
    size_t sig_len, outer_len;

    if (include_san) {
        names_len=put_tlv(scratch,san_tag,san_data,san_length);
        names_len=put_tlv(names,0x30,scratch,names_len);
        san_len=put_tlv(scratch,0x04,names,names_len);
        ext_len=put_tlv(extension,0x06,oid,sizeof oid);
        memcpy(extension+ext_len,scratch,san_len); ext_len+=san_len;
        ext_len=put_tlv(octets,0x30,extension,ext_len);
        ext_seq_len=put_tlv(extension_seq,0x30,octets,ext_len);
        explicit_len=put_tlv(explicit_extensions,0xA3,extension_seq,ext_seq_len);
    }

    fields_len=0;
    fields_len+=put_tlv(fields+fields_len,0xA0,(const unsigned char[]) {0x02,0x01,0x02},3);
    fields_len+=put_tlv(fields+fields_len,0x02,serial,sizeof serial);
    fields_len+=put_tlv(fields+fields_len,0x30,0,0); /* signature algorithm */
    fields_len+=put_tlv(fields+fields_len,0x30,0,0); /* issuer */
    fields_len+=put_tlv(fields+fields_len,0x30,0,0); /* validity */
    fields_len+=put_tlv(fields+fields_len,0x30,0,0); /* subject */
    fields_len+=put_tlv(fields+fields_len,0x30,0,0); /* subject public key info */
    if (include_san) { memcpy(fields+fields_len,explicit_extensions,explicit_len); fields_len+=explicit_len; }
    tbs_len=put_tlv(tbs,0x30,fields,fields_len);
    sig_len=put_tlv(signature,0x03,(const unsigned char[]){0},1);
    n=0;
    memcpy(outer_body+n,tbs,tbs_len); n+=tbs_len;
    n+=put_tlv(outer_body+n,0x30,0,0);
    memcpy(outer_body+n,signature,sig_len); n+=sig_len;
    outer_len=put_tlv(out,0x30,outer_body,n);
    return outer_len;
}

int main(void)
{
    static const unsigned char wanted[4]={192,0,2,17};
    static const unsigned char wrong[4]={192,0,2,18};
    unsigned char cert[600], malformed[600], dns_data[4]={192,0,2,17};
    unsigned char wrong_length[16]={192,0,2,17};
    size_t length;
    length=make_certificate(cert,1,0x87,wanted,sizeof wanted);
    CHECK(wx509_has_ipv4_san(cert,length,wanted),"exact IPv4 iPAddress SAN is accepted");
    CHECK(!wx509_has_ipv4_san(cert,length,wrong),"different IPv4 address is rejected");
    length=make_certificate(cert,0,0,0,0);
    CHECK(!wx509_has_ipv4_san(cert,length,wanted),"missing SAN extension is rejected without CN fallback");
    length=make_certificate(cert,1,0x82,dns_data,sizeof dns_data);
    CHECK(!wx509_has_ipv4_san(cert,length,wanted),"embedded-NUL DNS SAN cannot impersonate an iPAddress SAN");
    length=make_certificate(cert,1,0x87,wrong_length,sizeof wrong_length);
    CHECK(!wx509_has_ipv4_san(cert,length,wanted),"non-four-byte iPAddress SAN is rejected for IPv4");
    length=make_certificate(cert,1,0x87,wanted,sizeof wanted);
    memcpy(malformed,cert,length); malformed[1]=0xFF;
    CHECK(!wx509_has_ipv4_san(malformed,length,wanted),"malformed DER lengths fail closed");
    printf("Worker IPv4 SAN parser: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
