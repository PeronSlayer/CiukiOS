/* Shared XMS mailbox for a preemptible, user-mode web worker.
 * One producer (desktop) and one consumer (worker); publish sequence last.
 * All fields are little endian. No pointer from the network crosses the ABI.
 */
#ifndef CIUK_WEB_WORKER_ABI_H
#define CIUK_WEB_WORKER_ABI_H
#define CWW_MAGIC 0x32335743UL
#define CWW_ABI 1
#define CWW_BYTES 131072UL
#define CWW_DMA_OFFSET 65536UL
#define CWW_DMA_BYTES 65536UL
#define CWW_INPUT 64UL
#define CWW_OUTPUT 24640UL
#define CWW_DATA_BYTES 24576
#define CWW_PING 1
#define CWW_EXIT 2
#define CWW_TLS_OPEN 16
#define CWW_TLS_STEP 17
#define CWW_TLS_WRITE 18
#define CWW_TLS_CLOSE 19
#define CWW_JS_RESET 32
#define CWW_JS_EVAL 33
#define CWW_JS_EVENT 34
#define CWW_JS_ELEMENT 35
#define CWW_PAGE_SCAN 40
#define CWW_PAGE_RESOURCE 41
#define CWW_PAGE_RUN 42
#define CWW_F_CIPHER 1
#define CWW_F_PLAIN 2
#define CWW_F_READY 4
#define CWW_F_CLOSED 8
#define CWW_F_MORE 16
#ifdef __WATCOMC__
typedef unsigned long cww_u32;
#else
#include <stdint.h>
typedef uint32_t cww_u32;
#endif
#pragma pack(push,1)
/* uint32_t on the worker, u32 on the 16-bit caller. */
struct cww_header {
    cww_u32 magic,abi,sequence,completed;
    cww_u32 operation,error,flags,input_bytes;
    cww_u32 output_bytes,state,consumed,aux;
    cww_u32 heartbeat,reserved[3];
};
#pragma pack(pop)
#endif
