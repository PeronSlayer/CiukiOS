/* Host platform boundary for the unchanged production w32srv main. */
#ifndef DESK_W32_HOST_RUNTIME_H
#define DESK_W32_HOST_RUNTIME_H
#include <ciuki/abi.h>
#include <stdint.h>
uint32_t host_w32_call(void *);
int ciuki_display_query(struct ciuki_display_info *);
#define CU_PTR(p) (p)
#define CU_CALL(n,b,c,d,s,i,p) host_w32_call(b)
#endif
