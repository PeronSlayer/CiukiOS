/* TLS template and two real callbacks; no compiler CRT. SPDX-License-Identifier: MIT */
typedef unsigned long U32;
typedef void (__stdcall *Callback)(void *, U32, void *);
volatile U32 trace;
U32 tls_index;
void __stdcall cb1(void *module, U32 reason, void *reserved) {
    (void)module; (void)reserved; trace = trace*10+reason;
}
void __stdcall cb2(void *module, U32 reason, void *reserved) {
    (void)module; (void)reserved; trace = trace*10+reason+4;
}
#pragma section(".tls", read, write)
#pragma section(".rdata$T", read)
__declspec(allocate(".tls")) const unsigned char template_data[4] = {1,2,3,4};
__declspec(allocate(".rdata$T")) Callback callbacks[] = {cb1, cb2, 0};
struct TlsDirectory { const void *start, *end; U32 *index; Callback *callbacks; U32 zero, flags; };
__declspec(allocate(".rdata$T")) const struct TlsDirectory _tls_used = {
    template_data, template_data+4, &tls_index, callbacks, 8, 0
};
int entry(void) { return trace == 15 ? 0 : 1; }
