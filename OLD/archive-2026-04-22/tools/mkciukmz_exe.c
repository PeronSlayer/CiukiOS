/*
 * mkciukmz_exe — host-side generator for CIUKMZ.EXE.
 *
 * Wraps a flat x86_64 payload binary with a deterministic DOS MZ (.EXE)
 * header plus the CIUKEX64 native-dispatch marker understood by the
 * CiukiOS stage2 MZ launch path. Output is byte-identical for a given
 * input payload, so the artifact is reproducible from source.
 *
 * Layout of the produced file (all little-endian):
 *
 *   [0x00 .. 0x1F]  MZ header (32 bytes, header_paragraphs = 2).
 *   [0x20 .. 0x27]  "CIUKEX64" marker.
 *   [0x28 .. 0x2B]  entry offset within the loaded module (fixed 0x0000000C).
 *   [0x2C ..    ]   payload bytes verbatim (x86_64 machine code for com_main).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void write_le16(FILE *f, uint16_t v) {
    unsigned char b[2] = {
        (unsigned char)(v & 0xFFU),
        (unsigned char)((v >> 8) & 0xFFU),
    };
    fwrite(b, 1, sizeof(b), f);
}

static void write_le32(FILE *f, uint32_t v) {
    unsigned char b[4] = {
        (unsigned char)(v & 0xFFU),
        (unsigned char)((v >> 8) & 0xFFU),
        (unsigned char)((v >> 16) & 0xFFU),
        (unsigned char)((v >> 24) & 0xFFU),
    };
    fwrite(b, 1, sizeof(b), f);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <payload.bin> <out.exe>\n", argv[0]);
        return 2;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(in);
        return 1;
    }
    long payload_size_signed = ftell(in);
    if (payload_size_signed <= 0) {
        fprintf(stderr, "mkciukmz_exe: empty or invalid payload: %s\n", argv[1]);
        fclose(in);
        return 1;
    }
    rewind(in);

    size_t payload_size = (size_t)payload_size_signed;
    size_t module_size = 12U + payload_size;        /* marker(8) + entry_off(4) + code */
    size_t total_file = 32U + module_size;          /* header(32) + module */

    /* Keep the wrapper deterministic and within single-page MZ bounds. */
    if (total_file > 512U) {
        fprintf(stderr,
                "mkciukmz_exe: wrapped file %zu bytes exceeds single-page MZ limit (512)\n",
                total_file);
        fclose(in);
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        fclose(in);
        return 1;
    }

    /* MZ header (32 bytes, header_paragraphs = 2). */
    if (fwrite("MZ", 1, 2, out) != 2U) {
        goto io_fail;
    }
    write_le16(out, (uint16_t)total_file); /* bytes in last page */
    write_le16(out, 1U);                    /* total pages */
    write_le16(out, 0U);                    /* relocation count (no fixups needed) */
    write_le16(out, 2U);                    /* header paragraphs (32 bytes) */
    write_le16(out, 0U);                    /* min alloc */
    write_le16(out, 0xFFFFU);               /* max alloc */
    write_le16(out, 0U);                    /* ss */
    write_le16(out, 0U);                    /* sp */
    write_le16(out, 0U);                    /* checksum */
    write_le16(out, 0U);                    /* ip */
    write_le16(out, 0U);                    /* cs */
    write_le16(out, 0x001CU);               /* relocation table offset */
    write_le16(out, 0U);                    /* overlay number */
    write_le16(out, 0U);                    /* pad */
    write_le16(out, 0U);                    /* pad -> fills 32-byte header */

    /* Module: native-dispatch marker + entry offset (module-relative) + payload. */
    if (fwrite("CIUKEX64", 1, 8, out) != 8U) {
        goto io_fail;
    }
    write_le32(out, 12U);

    /* Stream payload verbatim. */
    unsigned char buf[512];
    size_t remaining = payload_size;
    while (remaining > 0U) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, in);
        if (got == 0U) {
            fprintf(stderr, "mkciukmz_exe: short read on payload\n");
            goto io_fail;
        }
        if (fwrite(buf, 1, got, out) != got) {
            goto io_fail;
        }
        remaining -= got;
    }

    fclose(in);
    if (fclose(out) != 0) {
        perror("fclose out");
        return 1;
    }
    return 0;

io_fail:
    fclose(in);
    fclose(out);
    fprintf(stderr, "mkciukmz_exe: write failure on %s\n", argv[2]);
    return 1;
}
