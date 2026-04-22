#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_mz.h"

static int read_whole_file(const char *path, unsigned char **buf_out, size_t *size_out) {
    FILE *fp;
    long file_len;
    unsigned char *buf;
    size_t read_len;

    if (!path || !buf_out || !size_out) {
        return 0;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    file_len = ftell(fp);
    if (file_len < 0) {
        fclose(fp);
        return 0;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    if (file_len == 0) {
        fclose(fp);
        return 0;
    }

    buf = (unsigned char *)malloc((size_t)file_len);
    if (!buf) {
        fclose(fp);
        return 0;
    }

    read_len = fread(buf, 1U, (size_t)file_len, fp);
    fclose(fp);

    if (read_len != (size_t)file_len) {
        free(buf);
        return 0;
    }

    *buf_out = buf;
    *size_out = (size_t)file_len;
    return 1;
}

int main(int argc, char **argv) {
    unsigned char *file_buf = NULL;
    size_t file_size = 0;
    dos_mz_info_t info;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <exe-path>\n", argv[0]);
        return 3;
    }

    if (!read_whole_file(argv[1], &file_buf, &file_size)) {
        fprintf(stderr, "[probe] cannot read file: %s\n", argv[1]);
        return 3;
    }

    if (file_size < 2U || file_buf[0] != 'M' || file_buf[1] != 'Z') {
        free(file_buf);
        return 2;
    }

    if (!dos_mz_parse(file_buf, (u32)file_size, &info)) {
        fprintf(stderr, "[probe] parse failed: %s\n", argv[1]);
        free(file_buf);
        return 1;
    }

    printf("[probe] PASS %s module=0x%X reloc=%u entry=0x%X runtime=0x%X\n",
           argv[1],
           info.module_size_bytes,
           (unsigned)info.relocation_count,
           info.entry_offset,
           info.runtime_required_bytes);

    free(file_buf);
    return 0;
}
