/* Self-authored, CRT-free i386 ABI probes. SPDX-License-Identifier: MIT */
typedef unsigned long U32;
typedef unsigned short WCHAR;
typedef long HRESULT;
typedef void *HANDLE;
_Static_assert(sizeof(long) == 4 && sizeof(HANDLE) == 4 && sizeof(WCHAR) == 2, "Win32 widths");
#pragma pack(push, 1)
struct Packed { char byte; U32 word; };
#pragma pack(pop)
_Static_assert(sizeof(struct Packed) == 5, "packing");

volatile U32 observed;
__declspec(noinline) int __stdcall stdcall_add(int a, int b) { return a+b; }
__declspec(noinline) int __cdecl cdecl_add(int a, int b) { return a+b; }
__declspec(noinline) int __cdecl varargs_sum(int n, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    int total = 0;
    while (n--) total += __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return total;
}
struct Object { int value; };
__declspec(noinline) int __attribute__((thiscall)) method(struct Object *self, int b) {
    return self->value + b;
}
__declspec(noinline) unsigned long long __stdcall return64(void) {
    return 0x1122334455667788ULL;
}
struct Vtable { int (__stdcall *invoke)(struct Object *, int); };
__declspec(noinline) int __stdcall com_method(struct Object *self, int b) { return self->value+b; }
__declspec(noinline) int __stdcall callback(int n) { return n+1; }
__declspec(noinline) int nonvolatile(void) {
    __asm__ volatile ("movl $1, %%ebx; movl $2, %%esi; movl $3, %%edi" ::: "ebx", "esi", "edi");
    return 0;
}
int entry(void) {
    struct Object o = {7};
    struct Vtable v = {com_method};
#if CASE == 1
    observed = stdcall_add(4, 5);
#elif CASE == 2
    observed = cdecl_add(4, 5);
#elif CASE == 3
    observed = varargs_sum(3, 1, 2, 3);
#elif CASE == 4
    observed = method(&o, 5);
#elif CASE == 5
    unsigned long long n = return64(); observed = (U32)n ^ (U32)(n>>32);
#elif CASE == 6
    observed = v.invoke(&o, 5);
#elif CASE == 7
    int (__stdcall *cb)(int) = callback; observed = cb(9);
#elif CASE == 8
    observed = nonvolatile();
#elif CASE == 9
    __asm__ volatile("std; cld" ::: "cc"); observed = 0;
#else
    observed = sizeof(struct Packed) + sizeof(HRESULT) + sizeof(WCHAR);
#endif
    return 0;
}
