/* Test-only syscall/TCB substitution; compile the production pthread source. */
#define ciuki_tcb(...) __ciuki_target_tcb(__VA_ARGS__)
#include <ciuki/runtime.h>
#undef ciuki_tcb
struct ciuki_tcb *__ciuki_host_tcb(void);
#define ciuki_tcb(...) __ciuki_host_tcb(__VA_ARGS__)
