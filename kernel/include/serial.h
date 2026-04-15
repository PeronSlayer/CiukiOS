#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *s);
void serial_write_hex64(u64 value);

#endif
