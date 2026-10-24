/* SPDX-License-Identifier: MIT
 * Bounded, polled YMODEM, CRC16 ZMODEM and basic Kermit. No UART or UI calls. */
#ifndef CIUKI_SERIAL_TRANSFER_H
#define CIUKI_SERIAL_TRANSFER_H
#include <stddef.h>
#include <stdint.h>
enum transfer_protocol { TRANSFER_YMODEM=1, TRANSFER_ZMODEM, TRANSFER_KERMIT };
struct transfer_io {
    int (*read)(void *,uint8_t *,size_t);
    int (*write)(void *,const uint8_t *,size_t);
    int (*seek)(void *,uint32_t);
    void *ctx;
};
struct transfer {
    struct transfer_io io;
    int protocol,sending,state,phase; /* state: 0 running, 1 done, -1 failed, -2 cancelled */
    unsigned seq,retries,quiet,cans;
    uint32_t position,size; int known_size;
    char name[128],message[96];
    uint8_t frame[2056],last[2400],out[8192];
    size_t used,want,last_length,out_length;
    int parse,escape,end,crc_bytes,header_type,hex_half;
    uint8_t header[7]; unsigned header_used;
    unsigned kmax,kquote,keol,kpad,kpadchar; int k8; unsigned kbin;
};
int transfer_init(struct transfer *,int protocol,int sending,const struct transfer_io *,const char *name,uint32_t size);
size_t transfer_input(struct transfer *,const uint8_t *,size_t);
size_t transfer_take(struct transfer *,uint8_t *,size_t);
void transfer_tick(struct transfer *,unsigned milliseconds);
void transfer_cancel(struct transfer *);
#endif
